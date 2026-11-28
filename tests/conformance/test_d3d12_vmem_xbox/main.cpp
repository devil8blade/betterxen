/*
 * Conformance test for the Xbox virtual-memory configuration decoder in
 * d3d12_xs.dll!D3D12ConfigureVirtualMemoryX (D3D12XBOX_VIRTUAL_MEMORY_CONFIGURATION).
 *
 * Expected values come from the authorized GDK documentation of the function and the
 * structure, written out here rather than taken from the shim's header:
 *  - the default page-table size (5 x 4 MB) reports a minimum graphics address of 2 GiB and
 *    a maximum of 159.375 GiB; the input fields are left as the caller set them;
 *  - the page-table size is clamped to 33, the frame-buffer preserve size to 64 MiB, and -1
 *    (the legacy size) is kept;
 *  - after a D3D12 device exists the call fails with E_FAIL and writes nothing;
 *  - every accepted configuration is recorded only (applied=0) and still emits the
 *    unimplemented-API stub, and leaves a cached BXVM backing heap untouched.
 * Inferred, and marked as such in the log: the maximum for other page-table sizes scales
 * linearly (max_estimated=1), and a page-table size of 0, an unreadable configuration and
 * unwritable outputs fail with E_INVALIDARG.
 *
 * Several fixtures share one BX_LOG_FILE, so every log check reads only the
 * bytes appended since the step it checks began.
 */
#include <d3d12.h>
#include <windows.h>

#include <cstddef>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "bx_config.h"
#include "bx_log.h"
#include "bx_xmem_internal.h"

extern "C" {
__declspec(dllimport) HRESULT WINAPI D3D12XboxCreateDevice(void* params, REFIID riid, void** ppv);
__declspec(dllimport) HRESULT WINAPI D3D12ConfigureVirtualMemoryX(void* config);
__declspec(dllimport) PVOID WINAPI XMemVirtualAlloc(PVOID address, SIZE_T size, DWORD alloc_type, DWORD protect);
__declspec(dllimport) BOOL WINAPI XMemAllocatePhysicalPages(SIZE_T* page_count, PULONG_PTR pages, uint64_t flags);
__declspec(dllimport) BOOL WINAPI XMemFreePhysicalPages(SIZE_T* page_count, PULONG_PTR pages, uint64_t flags);
__declspec(dllimport) PVOID WINAPI XMemMapPhysicalPages(PVOID address, SIZE_T page_count, uint64_t flags,
                                                        PULONG_PTR pages);
}

namespace {

constexpr uint32_t kBxvmMagic = 0x4D565842u;
constexpr uint32_t kBxvmFlagBacking = 0x1u;
constexpr uint64_t k64K = 64ull * 1024ull;

struct BxvmConfig {
    uint32_t magic;
    uint32_t struct_size;
    uint32_t version;
    uint32_t flags;
    uint64_t base_va;
    uint64_t size_bytes;
    uint64_t reserved;
    uint64_t cpu_pointer;
};

struct XboxConfig {
    uint32_t flags;
    uint32_t page_table_4mb_pages;
    uint32_t frame_buffer_preserve_bytes;
    uint32_t padding;
    uint64_t min_gpu_va;
    uint64_t max_gpu_va;
};
static_assert(sizeof(BxvmConfig) == 48);
static_assert(offsetof(BxvmConfig, magic) == 0);
static_assert(offsetof(BxvmConfig, struct_size) == 4);
static_assert(offsetof(BxvmConfig, version) == 8);
static_assert(offsetof(BxvmConfig, flags) == 12);
static_assert(offsetof(BxvmConfig, base_va) == 16);
static_assert(offsetof(BxvmConfig, size_bytes) == 24);
static_assert(offsetof(BxvmConfig, reserved) == 32);
static_assert(offsetof(BxvmConfig, cpu_pointer) == 40);
static_assert(sizeof(XboxConfig) == 32);
static_assert(offsetof(XboxConfig, flags) == 0);
static_assert(offsetof(XboxConfig, page_table_4mb_pages) == 4);
static_assert(offsetof(XboxConfig, frame_buffer_preserve_bytes) == 8);
static_assert(offsetof(XboxConfig, padding) == 12);
static_assert(offsetof(XboxConfig, min_gpu_va) == 16);
static_assert(offsetof(XboxConfig, max_gpu_va) == 24);

/* From the documentation: 2 GB minimum, about 159.375 GB maximum at the default size. */
constexpr uint64_t kMinGpuVa = 0x80000000ull;
constexpr uint64_t kDefaultMaxGpuVa = 0x27D8000000ull;
constexpr uint64_t kOutputSentinel = 0xA5A5A5A5A5A5A5A5ull;

int g_failed = 0;

void check(bool cond, const char* fmt, ...) {
    if (cond) {
        return;
    }
    ++g_failed;
    va_list args;
    va_start(args, fmt);
    std::fprintf(stderr, "FAIL: ");
    std::vfprintf(stderr, fmt, args);
    std::fprintf(stderr, "\n");
    va_end(args);
}

unsigned long ul(HRESULT hr) {
    return static_cast<unsigned long>(hr);
}

bool read_file_from(const char* path, long start, std::string& out) {
    out.clear();
    FILE* f = std::fopen(path, "rb");
    if (!f) {
        return false;
    }
    if (start > 0 && std::fseek(f, start, SEEK_SET) != 0) {
        std::fclose(f);
        return false;
    }
    char buf[4096];
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        out.append(buf, n);
    }
    std::fclose(f);
    return true;
}

/* Current size of the log, used as the start offset of the next step. */
long log_size(const char* path) {
    FILE* f = std::fopen(path, "rb");
    if (!f) {
        return 0;
    }
    long size = 0;
    if (std::fseek(f, 0, SEEK_END) == 0) {
        size = std::ftell(f);
    }
    std::fclose(f);
    return size < 0 ? 0 : size;
}

bool log_contains_since(const char* path, long start, const char* needle) {
    std::string text;
    return read_file_from(path, start, text) && text.find(needle) != std::string::npos;
}

constexpr const char* kStubNeedle = "d3d12_xs.dll!D3D12ConfigureVirtualMemoryX";

BxvmConfig make_bxvm(uint64_t base_va, uint64_t size_bytes, uint32_t flags) {
    BxvmConfig cfg{};
    cfg.magic = kBxvmMagic;
    cfg.struct_size = sizeof(BxvmConfig);
    cfg.version = 1;
    cfg.flags = flags;
    cfg.base_va = base_va;
    cfg.size_bytes = size_bytes;
    cfg.reserved = 0;
    cfg.cpu_pointer = 0xA5A5A5A5A5A5A5A5ull;
    return cfg;
}

XboxConfig make_xbox(uint32_t pages, uint32_t preserve = 0) {
    XboxConfig cfg{};
    cfg.page_table_4mb_pages = pages;
    cfg.frame_buffer_preserve_bytes = preserve;
    cfg.min_gpu_va = kOutputSentinel;
    cfg.max_gpu_va = kOutputSentinel;
    return cfg;
}

/* The caller's input fields, which the decoder must not change. */
bool inputs_unchanged(const XboxConfig& after, const XboxConfig& before) {
    return after.flags == before.flags && after.page_table_4mb_pages == before.page_table_4mb_pages &&
           after.frame_buffer_preserve_bytes == before.frame_buffer_preserve_bytes &&
           after.padding == before.padding;
}

} // namespace

int main() {
    const bx_runtime_config_t* cfg = bx_get_runtime_config();
    if (!cfg) {
        std::fprintf(stderr, "FAIL: test_d3d12_vmem_xbox (no runtime config)\n");
        return 1;
    }

    char log_path[MAX_PATH] = {};
    const DWORD log_len = GetEnvironmentVariableA("BX_LOG_FILE", log_path, sizeof(log_path));
    const bool have_log = log_len > 0 && log_len < sizeof(log_path);
    check(have_log, "BX_LOG_FILE must be set");

    long mark = have_log ? log_size(log_path) : 0;
    const long test_start = mark;

    /* 1. The default configuration reports the documented address range. */
    XboxConfig def = make_xbox(5);
    const XboxConfig def_before = def;
    HRESULT hr = D3D12ConfigureVirtualMemoryX(&def);
    check(hr == S_OK, "default Xbox config must return S_OK (hr=0x%08lx)", ul(hr));
    check(inputs_unchanged(def, def_before), "Xbox decoder must not change the input fields");
    check(def.min_gpu_va == kMinGpuVa, "minimum graphics address must be 2 GiB (got 0x%llx)",
          static_cast<unsigned long long>(def.min_gpu_va));
    check(def.max_gpu_va == kDefaultMaxGpuVa, "default maximum graphics address must be 159.375 GiB (got 0x%llx)",
          static_cast<unsigned long long>(def.max_gpu_va));
    if (have_log) {
        check(log_contains_since(log_path, mark,
                                 "decoder=xbox applied=0 recorded_only=1 page_table_4mb_pages=5 clamped=0 "
                                 "frame_buffer_preserve_bytes=0 min_gpu_va=0x80000000 max_gpu_va=0x27d8000000 "
                                 "max_estimated=0 repeat=0"),
              "log must record the default Xbox config as recorded only");
        check(log_contains_since(log_path, mark, kStubNeedle),
              "a recorded-only Xbox config must still emit the unimplemented-API stub");
        check(!log_contains_since(log_path, mark, "applied=1"),
              "a recorded-only Xbox config must not report applied=1");
        mark = log_size(log_path);
    }

    /* 2. Repeated identical call (repeat=1). */
    hr = D3D12ConfigureVirtualMemoryX(&def);
    check(hr == S_OK, "repeated identical Xbox config must return S_OK (hr=0x%08lx)", ul(hr));
    if (have_log) {
        check(log_contains_since(log_path, mark, "page_table_4mb_pages=5 clamped=0") &&
                  log_contains_since(log_path, mark, "repeat=1"),
              "repeated identical Xbox config must log repeat=1");
        mark = log_size(log_path);
    }

    /* 3. The page-table sizes observed at title startup, 4 and 3: the maximum is estimated. */
    for (uint32_t pages : {4u, 3u}) {
        XboxConfig cfg = make_xbox(pages);
        const XboxConfig before = cfg;
        hr = D3D12ConfigureVirtualMemoryX(&cfg);
        check(hr == S_OK, "Xbox config with %u pages must return S_OK (hr=0x%08lx)", pages, ul(hr));
        check(inputs_unchanged(cfg, before), "%u-page config input fields must stay unchanged", pages);
        check(cfg.min_gpu_va == kMinGpuVa, "%u-page config minimum must be 2 GiB", pages);
        check(cfg.max_gpu_va == kDefaultMaxGpuVa / 5u * pages, "%u-page config maximum must scale from the default",
              pages);
        if (have_log) {
            char needle[96];
            std::snprintf(needle, sizeof(needle), "page_table_4mb_pages=%u clamped=0", pages);
            check(log_contains_since(log_path, mark, needle) &&
                      log_contains_since(log_path, mark, "max_estimated=1 repeat=0"),
                  "%u-page config must log its size, max_estimated=1 and repeat=0", pages);
            mark = log_size(log_path);
        }
    }

    /* 4. Documented clamps: page table to 33, frame-buffer preserve to 64 MiB; -1 is the legacy size. */
    XboxConfig big = make_xbox(100u, 128u * 1024u * 1024u);
    hr = D3D12ConfigureVirtualMemoryX(&big);
    check(hr == S_OK, "oversized Xbox config must be clamped, not refused (hr=0x%08lx)", ul(hr));
    check(big.page_table_4mb_pages == 100u, "clamping must not rewrite the caller's page count");
    check(big.max_gpu_va == kDefaultMaxGpuVa / 5u * 33u, "clamped maximum must use 33 pages");
    if (have_log) {
        check(log_contains_since(log_path, mark,
                                 "page_table_4mb_pages=33 clamped=1 frame_buffer_preserve_bytes=67108864"),
              "oversized config must log the clamped page count and preserve size");
        mark = log_size(log_path);
    }
    XboxConfig legacy = make_xbox(5u, 0xFFFFFFFFu);
    hr = D3D12ConfigureVirtualMemoryX(&legacy);
    check(hr == S_OK, "legacy frame-buffer preserve size must be accepted (hr=0x%08lx)", ul(hr));
    if (have_log) {
        check(log_contains_since(log_path, mark, "frame_buffer_preserve_bytes=4294967295"),
              "the legacy preserve size (-1) must be recorded as given");
        mark = log_size(log_path);
    }

    /* 5. Refused configurations write nothing. */
    XboxConfig zero = make_xbox(0);
    const XboxConfig zero_before = zero;
    hr = D3D12ConfigureVirtualMemoryX(&zero);
    check(hr == E_INVALIDARG, "a page-table size of 0 must fail with E_INVALIDARG (hr=0x%08lx)", ul(hr));
    check(std::memcmp(&zero, &zero_before, sizeof(zero)) == 0, "a refused config must not be written");

    SYSTEM_INFO ro_sys{};
    GetSystemInfo(&ro_sys);
    const SIZE_T ro_bytes = ro_sys.dwPageSize ? ro_sys.dwPageSize : 4096;
    void* ro_page = VirtualAlloc(nullptr, ro_bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    check(ro_page != nullptr, "VirtualAlloc for the read-only Xbox probe must succeed");
    if (ro_page) {
        const XboxConfig ro_cfg = make_xbox(5);
        std::memcpy(ro_page, &ro_cfg, sizeof(ro_cfg));
        DWORD old_protect = 0;
        if (VirtualProtect(ro_page, ro_bytes, PAGE_READONLY, &old_protect)) {
            hr = D3D12ConfigureVirtualMemoryX(ro_page);
            check(hr == E_INVALIDARG, "read-only Xbox outputs must fail with E_INVALIDARG (hr=0x%08lx)", ul(hr));
            check(std::memcmp(ro_page, &ro_cfg, sizeof(ro_cfg)) == 0, "read-only Xbox config must stay unchanged");
            DWORD ignored = 0;
            VirtualProtect(ro_page, ro_bytes, old_protect, &ignored);
        } else {
            check(false, "Xbox config page must become read-only");
        }
        VirtualFree(ro_page, 0, MEM_RELEASE);
    }

    void* edge_mem = VirtualAlloc(nullptr, ro_bytes * 2, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    check(edge_mem != nullptr, "VirtualAlloc for the Xbox page-edge probe must succeed");
    if (edge_mem) {
        auto* edge_base = static_cast<uint8_t*>(edge_mem);
        DWORD old_prot = 0;
        check(VirtualProtect(edge_base + ro_bytes, ro_bytes, PAGE_NOACCESS, &old_prot) != 0,
              "second page must be PAGE_NOACCESS");
        uint8_t* edge = edge_base + ro_bytes - 8;
        const uint32_t header[2] = {0u, 5u};
        std::memcpy(edge, header, sizeof(header));
        hr = D3D12ConfigureVirtualMemoryX(edge);
        check(hr == E_INVALIDARG, "an Xbox config cut off by an unreadable page must fail (hr=0x%08lx)", ul(hr));
        VirtualProtect(edge_base + ro_bytes, ro_bytes, PAGE_READWRITE, &old_prot);
        VirtualFree(edge_mem, 0, MEM_RELEASE);
    }
    if (have_log) {
        for (const char* needle : {"decoder=xbox applied=0 reason=zero_page_table_pages hr=0x80070057",
                                   "decoder=xbox applied=0 reason=unwritable_output hr=0x80070057",
                                   "decoder=xbox applied=0 reason=unreadable_config hr=0x80070057"}) {
            check(log_contains_since(log_path, mark, needle), "missing refused-config log: %s", needle);
        }
        mark = log_size(log_path);
    }

    /*
     * 4. Verify XMemVirtualAlloc reservation works after Xbox configuration
     *    (matching the caller's MEM_RESERVE + console high-bit pattern).
     */
    PVOID reserved_va = XMemVirtualAlloc(nullptr, 2ull * 1024ull * 1024ull, 0x20402000u, 0x21u);
    check(reserved_va != nullptr, "XMemVirtualAlloc reservation after Xbox config must succeed");
    if (reserved_va) {
        check(VirtualFree(reserved_va, 0, MEM_RELEASE) != 0, "release the XMemVirtualAlloc reservation");
    }

    /*
     * 5. Configure a BXVM backing heap, create and cache it, then apply an Xbox
     *    configuration. The Xbox path does not apply a backing configuration, so the
     *    cached BXVM backing heap, its cpu_pointer and its device link must
     *    survive.
     */
    ID3D12Device* device = nullptr;
    hr = D3D12XboxCreateDevice(nullptr, __uuidof(ID3D12Device), reinterpret_cast<void**>(&device));
    check(hr == S_OK && device != nullptr, "D3D12XboxCreateDevice must succeed (hr=0x%08lx)", ul(hr));

    SIZE_T page_count = 2;
    ULONG_PTR pages[2] = {};
    const BOOL phys_ok = XMemAllocatePhysicalPages(&page_count, pages, 0);
    check(phys_ok && page_count == 2, "XMemAllocatePhysicalPages(2) must succeed");
    uint8_t* mapped_region = phys_ok ? static_cast<uint8_t*>(XMemMapPhysicalPages(nullptr, 2, 0, pages)) : nullptr;
    check(mapped_region != nullptr, "XMemMapPhysicalPages(2) must succeed");

    if (device && mapped_region) {
        const uint64_t region_va = reinterpret_cast<uint64_t>(mapped_region);
        const uint64_t region_bytes = 2ull * k64K;

        BxvmConfig bxvm = make_bxvm(region_va, region_bytes, kBxvmFlagBacking);
        hr = D3D12ConfigureVirtualMemoryX(&bxvm);
        check(hr == S_OK, "BXVM backing config must succeed (hr=0x%08lx)", ul(hr));
        check(bxvm.cpu_pointer == 0, "BXVM cpu_pointer must start at 0 before CreateHeap");

        D3D12_HEAP_DESC backing_desc{};
        backing_desc.SizeInBytes = region_bytes;
        backing_desc.Alignment = k64K;
        backing_desc.Properties.Type = D3D12_HEAP_TYPE_CUSTOM;
        backing_desc.Properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_BACK;
        backing_desc.Properties.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
        backing_desc.Flags = D3D12_HEAP_FLAG_NONE;

        ID3D12Heap* backing_heap = nullptr;
        hr = device->CreateHeap(&backing_desc, __uuidof(ID3D12Heap), reinterpret_cast<void**>(&backing_heap));
        check(hr == S_OK && backing_heap != nullptr, "CreateHeap for BXVM backing must succeed (hr=0x%08lx)", ul(hr));

        BxvmConfig query = make_bxvm(region_va, region_bytes, kBxvmFlagBacking);
        hr = D3D12ConfigureVirtualMemoryX(&query);
        check(hr == S_OK && query.cpu_pointer != 0,
              "BXVM query after CreateHeap must return non-zero cached cpu_pointer (hr=0x%08lx)", ul(hr));

        /* Once a device exists the Xbox call fails, writes nothing and leaves the cached heap alone. */
        XboxConfig switch_cfg = make_xbox(4);
        const XboxConfig switch_before = switch_cfg;
        const long switch_mark = have_log ? log_size(log_path) : 0;
        hr = D3D12ConfigureVirtualMemoryX(&switch_cfg);
        check(hr == E_FAIL, "Xbox config after device creation must fail with E_FAIL (hr=0x%08lx)", ul(hr));
        check(std::memcmp(&switch_cfg, &switch_before, sizeof(switch_cfg)) == 0,
              "Xbox config refused after device creation must not be written");
        if (have_log) {
            check(log_contains_since(log_path, switch_mark,
                                     "decoder=xbox applied=0 reason=device_already_created hr=0x80004005"),
                  "Xbox config after device creation must log device_already_created");
        }

        if (backing_heap) {
            ID3D12Device* still_dev = nullptr;
            const HRESULT dev_hr = backing_heap->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&still_dev));
            check(SUCCEEDED(dev_hr) && still_dev != nullptr,
                  "cached BXVM backing heap must keep its device after an Xbox config (hr=0x%08lx)", ul(dev_hr));
            if (still_dev) {
                still_dev->Release();
            }
        }

        BxvmConfig after_xbox = make_bxvm(region_va, region_bytes, kBxvmFlagBacking);
        hr = D3D12ConfigureVirtualMemoryX(&after_xbox);
        check(hr == S_OK && after_xbox.cpu_pointer == query.cpu_pointer,
              "BXVM cpu_pointer must be unchanged after an Xbox config (hr=0x%08lx)", ul(hr));

        if (backing_heap) {
            backing_heap->Release();
            backing_heap = nullptr;
        }

        /* Ordinary D3D12 heap creation still works after the refused Xbox call. */
        D3D12_HEAP_DESC ordinary_desc{};
        ordinary_desc.SizeInBytes = k64K;
        ordinary_desc.Alignment = k64K;
        ordinary_desc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
        ordinary_desc.Flags = D3D12_HEAP_FLAG_NONE;
        ID3D12Heap* ordinary_heap = nullptr;
        hr = device->CreateHeap(&ordinary_desc, __uuidof(ID3D12Heap), reinterpret_cast<void**>(&ordinary_heap));
        check(hr == S_OK && ordinary_heap != nullptr,
              "ordinary CreateHeap must succeed after a refused Xbox config (hr=0x%08lx)", ul(hr));
        if (ordinary_heap) {
            ordinary_heap->Release();
        }
    }

    if (mapped_region) {
        XMemMapPhysicalPages(mapped_region, 2, 0, nullptr);
    }
    if (phys_ok) {
        SIZE_T free_count = 2;
        XMemFreePhysicalPages(&free_count, pages, 0);
    }
    if (device) {
        device->Release();
    }

    /* 6. Malformed and page-edge unreadable inputs to the shared entry point. */
    if (have_log) {
        mark = log_size(log_path);
    }
    check(D3D12ConfigureVirtualMemoryX(nullptr) == S_OK, "null config must fall back with S_OK");

    uint32_t unknown_hdr[8] = {0x11223344u, 4u, 0u, 0u, 0u, 0u, 0u, 0u};
    uint32_t unknown_hdr_before[8];
    std::memcpy(unknown_hdr_before, unknown_hdr, sizeof(unknown_hdr));
    check(D3D12ConfigureVirtualMemoryX(unknown_hdr) == S_OK, "unrecognized header must fall back with S_OK");
    check(std::memcmp(unknown_hdr, unknown_hdr_before, sizeof(unknown_hdr)) == 0,
          "unrecognized header bytes must stay unchanged");

    BxvmConfig readonly_bxvm = make_bxvm(0, 0, 0);
    SYSTEM_INFO readonly_sys{};
    GetSystemInfo(&readonly_sys);
    const SIZE_T readonly_page_bytes = readonly_sys.dwPageSize ? readonly_sys.dwPageSize : 4096;
    void* readonly_page =
        VirtualAlloc(nullptr, readonly_page_bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    check(readonly_page != nullptr, "VirtualAlloc for read-only BXVM probe must succeed");
    if (readonly_page) {
        std::memcpy(readonly_page, &readonly_bxvm, sizeof(readonly_bxvm));
        DWORD old_protect = 0;
        const bool protected_readonly =
            VirtualProtect(readonly_page, readonly_page_bytes, PAGE_READONLY, &old_protect) != 0;
        check(protected_readonly, "BXVM config page must become read-only");
        if (protected_readonly) {
            check(D3D12ConfigureVirtualMemoryX(readonly_page) == S_OK,
                  "read-only BXVM output must fall back with S_OK");
            check(std::memcmp(readonly_page, &readonly_bxvm, sizeof(readonly_bxvm)) == 0,
                  "read-only BXVM config bytes must stay unchanged");
            DWORD ignored_protect = 0;
            const bool writable =
                VirtualProtect(readonly_page, readonly_page_bytes, old_protect, &ignored_protect) != 0;
            check(writable, "make the BXVM page writable for the invalid-size case");
            if (writable) {
                const BxvmConfig invalid_bxvm = make_bxvm(0, 1, 0);
                std::memcpy(readonly_page, &invalid_bxvm, sizeof(invalid_bxvm));
                const bool readonly_again =
                    VirtualProtect(readonly_page, readonly_page_bytes, PAGE_READONLY, &ignored_protect) != 0;
                check(readonly_again, "protect the invalid-size BXVM page");
                if (readonly_again) {
                    check(D3D12ConfigureVirtualMemoryX(readonly_page) == E_INVALIDARG,
                          "invalid BXVM size must be rejected before the read-only fallback");
                    check(std::memcmp(readonly_page, &invalid_bxvm, sizeof(invalid_bxvm)) == 0,
                          "rejected BXVM config bytes must stay unchanged");
                }
            }
        }
        VirtualFree(readonly_page, 0, MEM_RELEASE);
    }

    SYSTEM_INFO sys{};
    GetSystemInfo(&sys);
    const SIZE_T page_bytes = sys.dwPageSize ? sys.dwPageSize : 4096;
    void* guard_mem = VirtualAlloc(nullptr, page_bytes * 2, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    check(guard_mem != nullptr, "VirtualAlloc for page-edge probe must succeed");
    if (guard_mem) {
        DWORD old_prot = 0;
        auto* base = static_cast<uint8_t*>(guard_mem);
        check(VirtualProtect(base + page_bytes, page_bytes, PAGE_NOACCESS, &old_prot) != 0,
              "second page must be PAGE_NOACCESS");

        /* Unreadable header directly on NOACCESS page. */
        check(D3D12ConfigureVirtualMemoryX(base + page_bytes) == S_OK,
              "unreadable header must not fault and must return S_OK");

        VirtualProtect(base + page_bytes, page_bytes, PAGE_READWRITE, &old_prot);
        VirtualFree(guard_mem, 0, MEM_RELEASE);
    }

    if (have_log) {
        constexpr const char* expected_fallbacks[] = {
            "\"event\":\"vmem_config_fallback\",\"detail\":\"decoder=none applied=0 reason=null_config\"",
            "\"event\":\"vmem_config_fallback\",\"detail\":\"decoder=none applied=0 reason=unrecognized_header\"",
            "\"event\":\"vmem_config_fallback\",\"detail\":\"decoder=none applied=0 reason=unreadable_header\"",
            "\"event\":\"vmem_config_fallback\",\"detail\":\"decoder=bxvm applied=0 reason=unwritable_output\"",
        };
        for (const char* needle : expected_fallbacks) {
            check(log_contains_since(log_path, mark, needle), "missing expected fallback log: %s", needle);
        }
        check(log_contains_since(log_path, test_start, "\"event\":\"vmem_config_recorded\""),
              "the Xbox path must log vmem_config_recorded");
    }

    if (g_failed != 0) {
        std::fprintf(stderr, "FAIL: test_d3d12_vmem_xbox (%d failed)\n", g_failed);
        return 1;
    }
    std::printf("PASS: test_d3d12_vmem_xbox\n");
    return 0;
}
