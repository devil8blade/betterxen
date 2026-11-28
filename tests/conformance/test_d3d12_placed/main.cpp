/*
 * M2-C1 (#464): D3D12ConfigureVirtualMemoryX and placed resources.
 *
 * The configuration block is BetterXen-defined and must match shims/d3d12_xs/vmem.h.
 * The export is PVOID with no size, so the block starts with magic 'BXVM' and the
 * struct size. cpu_pointer is written only during that call. After the backing heap
 * exists, a second call with the same region reads the persistent map. That map is
 * the WRITE_BACK buffer used by the write-through and the fence-separated GPU copy.
 */
#ifndef WIDL_EXPLICIT_AGGREGATE_RETURNS
#define WIDL_EXPLICIT_AGGREGATE_RETURNS
#endif
#include <windows.h>
#include <unknwn.h>
#include <d3d12.h>

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "bx_log.h"

extern "C" {
__declspec(dllimport) HRESULT WINAPI D3D12ConfigureVirtualMemoryX(void* config);
__declspec(dllimport) HRESULT WINAPI D3D12XboxCreateDevice(void* params, REFIID riid, void** ppv);
__declspec(dllimport) PVOID WINAPI XMemAlloc(SIZE_T size, uint64_t attributes);
__declspec(dllimport) VOID WINAPI XMemFree(PVOID address, uint64_t attributes);
__declspec(dllimport) BOOL WINAPI XMemAllocatePhysicalPages(SIZE_T* count, PULONG_PTR pages, uint64_t flags);
__declspec(dllimport) BOOL WINAPI XMemFreePhysicalPages(SIZE_T* count, PULONG_PTR pages, uint64_t flags);
__declspec(dllimport) PVOID WINAPI XMemMapPhysicalPages(PVOID base, SIZE_T count, uint64_t flags, PULONG_PTR pages);
__declspec(dllimport) BOOL WINAPI XMemGetWorkingSetStatistics(PVOID out_stats, SIZE_T stats_size);
}

struct bx_xmem_stats_t {
    uint64_t title_limit_bytes;
    uint64_t committed_virtual_bytes;
    uint64_t allocated_physical_bytes;
    uint64_t peak_committed_bytes;
};
static_assert(sizeof(bx_xmem_stats_t) == 32, "xmem stats struct is 32 bytes");

struct BxD3d12VirtualMemoryConfig {
    uint32_t magic;
    uint32_t struct_size;
    uint32_t version;
    uint32_t flags;
    uint64_t base_va;
    uint64_t size_bytes;
    uint64_t reserved;
    uint64_t cpu_pointer;
};
static_assert(sizeof(BxD3d12VirtualMemoryConfig) == 48, "config block is 48 bytes");
static_assert(offsetof(BxD3d12VirtualMemoryConfig, magic) == 0, "magic offset");
static_assert(offsetof(BxD3d12VirtualMemoryConfig, struct_size) == 4, "struct_size offset");
static_assert(offsetof(BxD3d12VirtualMemoryConfig, version) == 8, "version offset");
static_assert(offsetof(BxD3d12VirtualMemoryConfig, flags) == 12, "flags offset");
static_assert(offsetof(BxD3d12VirtualMemoryConfig, base_va) == 16, "base_va offset");
static_assert(offsetof(BxD3d12VirtualMemoryConfig, size_bytes) == 24, "size_bytes offset");
static_assert(offsetof(BxD3d12VirtualMemoryConfig, reserved) == 32, "reserved offset");
static_assert(offsetof(BxD3d12VirtualMemoryConfig, cpu_pointer) == 40, "cpu_pointer offset");

namespace {

constexpr uint32_t kMagic = 0x4D565842u;
constexpr uint32_t kFlagBacking = 0x1u;
constexpr uint32_t kVersion = 1;
constexpr uint64_t kAlign = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
constexpr uint32_t kBufBytes = 64;
constexpr uint32_t kTexW = 4;
constexpr uint32_t kTexH = 4;
constexpr uint32_t kChannels = 4;
constexpr uint32_t kRowPitch = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
constexpr uint32_t kPlacement = D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
constexpr uint32_t kFootprint = kTexH * kRowPitch;
constexpr uint32_t kUploadBytes = kPlacement + kFootprint;
constexpr uint64_t kRegionBytes = kAlign * 2;

static_assert(kMagic == 0x4D565842u, "magic is BXVM");
static_assert(kFlagBacking == 0x1u, "backing-heap opt-in is bit 0");
static_assert(kAlign == 65536, "placement alignment is 64 KiB");
static_assert(kRowPitch == 256, "texture row pitch is 256 bytes");
static_assert(kPlacement == 512, "texture placement alignment is 512 bytes");
static_assert(kBufBytes <= kRowPitch, "buffer pattern fits in the first texture row");
static_assert(kFootprint <= kAlign, "texture footprint fits in one 64 KiB page");
static_assert(kRegionBytes == 128ull * 1024ull, "configured region is 128 KiB");

int g_checks = 0;
int g_skipped = 0;
int g_failures = 0;

__attribute__((format(gnu_printf, 2, 3)))
bool check(bool ok, const char* fmt, ...) {
    g_checks++;
    if (!ok) {
        g_failures++;
        char msg[512];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(msg, sizeof(msg), fmt, ap);
        va_end(ap);
        std::fprintf(stderr, "FAIL: %s\n", msg);
        std::fflush(stderr);
    }
    return ok;
}

unsigned long ul(HRESULT hr) { return static_cast<unsigned long>(hr); }

bool file_contains(const char* path, const char* needle) {
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        return false;
    }
    bool found = false;
    LARGE_INTEGER size = {};
    if (GetFileSizeEx(f, &size) && size.QuadPart > 0 && size.QuadPart < (64ll << 20)) {
        auto* buf = static_cast<char*>(HeapAlloc(GetProcessHeap(), 0, static_cast<SIZE_T>(size.QuadPart) + 1));
        DWORD got = 0;
        if (buf && ReadFile(f, buf, static_cast<DWORD>(size.QuadPart), &got, nullptr)) {
            buf[got] = '\0';
            found = std::strstr(buf, needle) != nullptr;
        }
        if (buf) HeapFree(GetProcessHeap(), 0, buf);
    }
    CloseHandle(f);
    return found;
}

uint8_t texel_byte(uint32_t x, uint32_t y, uint32_t channel) {
    return static_cast<uint8_t>(0x3Cu ^ ((y * kTexW + x) * kChannels + channel + 1u));
}

void write_texture(uint8_t* dst) {
    std::memset(dst, 0, kFootprint);
    for (uint32_t y = 0; y < kTexH; ++y) {
        for (uint32_t x = 0; x < kTexW; ++x) {
            for (uint32_t c = 0; c < kChannels; ++c) {
                dst[y * kRowPitch + x * kChannels + c] = texel_byte(x, y, c);
            }
        }
    }
}

uint8_t reverse_byte(uint32_t i) { return static_cast<uint8_t>(0xA5u ^ (i * 3u + 1u)); }

void write_reverse_pattern(uint8_t* dst) {
    for (uint32_t i = 0; i < kBufBytes; ++i) {
        dst[i] = reverse_byte(i);
    }
}

uint32_t count_reverse(const uint8_t* got) {
    uint32_t match = 0;
    for (uint32_t i = 0; i < kBufBytes; ++i) {
        if (got[i] == reverse_byte(i)) {
            ++match;
        }
    }
    return match;
}

void write_buffer_pattern(uint8_t* dst) {
    std::memset(dst, 0, kBufBytes);
    for (uint32_t i = 0; i < kBufBytes; ++i) {
        const uint32_t x = i / kChannels;
        const uint32_t c = i % kChannels;
        if (x < kTexW) {
            dst[i] = texel_byte(x, 0, c);
        }
    }
}

uint32_t count_buffer(const uint8_t* got) {
    uint8_t want[kBufBytes];
    write_buffer_pattern(want);
    uint32_t match = 0;
    for (uint32_t i = 0; i < kBufBytes; ++i) {
        if (got[i] == want[i]) {
            ++match;
        }
    }
    return match;
}

uint32_t count_texture(const uint8_t* got) {
    uint8_t want[kFootprint];
    write_texture(want);
    uint32_t match = 0;
    for (uint32_t i = 0; i < kFootprint; ++i) {
        if (got[i] == want[i]) {
            ++match;
        }
    }
    return match;
}

BxD3d12VirtualMemoryConfig make_block(uint64_t base, uint64_t size, uint32_t flags) {
    BxD3d12VirtualMemoryConfig block{};
    block.magic = kMagic;
    block.struct_size = static_cast<uint32_t>(sizeof(block));
    block.version = kVersion;
    block.flags = flags;
    block.base_va = base;
    block.size_bytes = size;
    return block;
}

bool inputs_unchanged(const BxD3d12VirtualMemoryConfig& got, const BxD3d12VirtualMemoryConfig& sent) {
    return got.magic == sent.magic && got.struct_size == sent.struct_size && got.version == sent.version &&
           got.flags == sent.flags && got.base_va == sent.base_va && got.size_bytes == sent.size_bytes &&
           got.reserved == sent.reserved;
}

D3D12_HEAP_DESC heap_of(D3D12_HEAP_TYPE type, uint64_t bytes, D3D12_HEAP_FLAGS flags) {
    D3D12_HEAP_DESC desc{};
    desc.SizeInBytes = bytes;
    desc.Properties.Type = type;
    desc.Flags = flags;
    return desc;
}

D3D12_HEAP_DESC backing_desc() {
    D3D12_HEAP_DESC desc{};
    desc.SizeInBytes = kRegionBytes;
    desc.Alignment = 0;
    desc.Properties.Type = D3D12_HEAP_TYPE_CUSTOM;
    desc.Properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_BACK;
    desc.Properties.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
    desc.Flags = D3D12_HEAP_FLAG_NONE;
    return desc;
}

D3D12_RESOURCE_DESC buffer_desc(uint64_t bytes) {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return desc;
}

D3D12_RESOURCE_DESC texture_desc() {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = kTexW;
    desc.Height = kTexH;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    return desc;
}

/* First and last unsigned values of `key` in a JSONL log. */
void log_u64_ends(const char* path, const char* key, uint64_t* first, uint64_t* last, int* count) {
    *first = 0;
    *last = 0;
    *count = 0;
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        return;
    }
    LARGE_INTEGER size = {};
    if (GetFileSizeEx(f, &size) && size.QuadPart > 0 && size.QuadPart < (64ll << 20)) {
        auto* buf = static_cast<char*>(HeapAlloc(GetProcessHeap(), 0, static_cast<SIZE_T>(size.QuadPart) + 1));
        DWORD got = 0;
        if (buf && ReadFile(f, buf, static_cast<DWORD>(size.QuadPart), &got, nullptr)) {
            buf[got] = '\0';
            const size_t key_len = std::strlen(key);
            for (char* p = buf; (p = std::strstr(p, key)) != nullptr; p += key_len) {
                const uint64_t value = std::strtoull(p + key_len, nullptr, 16);
                if (*count == 0) {
                    *first = value;
                }
                *last = value;
                *count += 1;
            }
        }
        if (buf) HeapFree(GetProcessHeap(), 0, buf);
    }
    CloseHandle(f);
}

/* Last unsigned value of `key` (for example "gpu_heap_bytes=") in a JSONL log. */
uint64_t last_log_u64(const char* path, const char* key) {
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        return ~0ull;
    }
    uint64_t found = ~0ull;
    LARGE_INTEGER size = {};
    if (GetFileSizeEx(f, &size) && size.QuadPart > 0 && size.QuadPart < (64ll << 20)) {
        auto* buf = static_cast<char*>(HeapAlloc(GetProcessHeap(), 0, static_cast<SIZE_T>(size.QuadPart) + 1));
        DWORD got = 0;
        if (buf && ReadFile(f, buf, static_cast<DWORD>(size.QuadPart), &got, nullptr)) {
            buf[got] = '\0';
            const size_t key_len = std::strlen(key);
            for (char* p = buf; (p = std::strstr(p, key)) != nullptr; p += key_len) {
                found = std::strtoull(p + key_len, nullptr, 10);
            }
        }
        if (buf) HeapFree(GetProcessHeap(), 0, buf);
    }
    CloseHandle(f);
    return found;
}

bool same_object(IUnknown* a, IUnknown* b);

bool get_device_closed(ID3D12Heap* heap) {
    if (!heap) {
        return false;
    }
    ID3D12Device* got = nullptr;
    const HRESULT hr = heap->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&got));
    const bool closed = hr == E_FAIL && got == nullptr;
    if (got) {
        got->Release();
    }
    return closed;
}

struct DeviceStartupCheck {
    HANDLE start;
    HRESULT device_hr = E_FAIL;
    HRESULT heap_hr = E_FAIL;
};

DWORD WINAPI check_device_startup(LPVOID context) {
    auto& result = *static_cast<DeviceStartupCheck*>(context);
    if (WaitForSingleObject(result.start, INFINITE) != WAIT_OBJECT_0) {
        return 1;
    }
    ID3D12Device* device = nullptr;
    result.device_hr = D3D12XboxCreateDevice(nullptr, __uuidof(ID3D12Device), reinterpret_cast<void**>(&device));
    if (SUCCEEDED(result.device_hr) && device) {
        const auto desc = heap_of(D3D12_HEAP_TYPE_UPLOAD, 600ull << 20, D3D12_HEAP_FLAG_NONE);
        ID3D12Heap* heap = nullptr;
        result.heap_hr = device->CreateHeap(&desc, __uuidof(ID3D12Heap), reinterpret_cast<void**>(&heap));
        if (heap) heap->Release();
        device->Release();
    }
    return 0;
}

void check_concurrent_device_startup() {
    HANDLE start = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    if (!check(start != nullptr, "startup barrier for concurrent devices")) {
        return;
    }
    DeviceStartupCheck results[2] = {{start}, {start}};
    HANDLE threads[2] = {
        CreateThread(nullptr, 0, check_device_startup, &results[0], 0, nullptr),
        CreateThread(nullptr, 0, check_device_startup, &results[1], 0, nullptr),
    };
    SetEvent(start);
    for (size_t i = 0; i < 2; ++i) {
        if (check(threads[i] != nullptr, "concurrent device worker %zu started", i)) {
            const DWORD waited = WaitForSingleObject(threads[i], INFINITE);
            check(waited == WAIT_OBJECT_0 && results[i].device_hr == S_OK && results[i].heap_hr == E_OUTOFMEMORY,
                  "concurrent device %zu enforces the cap (device=0x%08lx heap=0x%08lx)", i,
                  ul(results[i].device_hr), ul(results[i].heap_hr));
            CloseHandle(threads[i]);
        }
    }
    CloseHandle(start);
}

void check_placed_heap_charge(ID3D12Device* device, const char* log_path, uint64_t region_base, uint64_t cap) {
    const bool capped = cap == (512ull << 20);
    const uint64_t heap_bytes = capped ? 300ull << 20 : kAlign;
    const auto heap_desc = heap_of(D3D12_HEAP_TYPE_UPLOAD, heap_bytes, D3D12_HEAP_FLAG_NONE);
    ID3D12Heap* heap = nullptr;
    HRESULT hr = device->CreateHeap(&heap_desc, __uuidof(ID3D12Heap), reinterpret_cast<void**>(&heap));
    if (!check(hr == S_OK && heap, "heap for placed-resource charge check hr=0x%08lx", ul(hr))) {
        return;
    }
    ID3D12Resource* resource = nullptr;
    const auto desc = buffer_desc(kBufBytes);
    hr = device->CreatePlacedResource(heap, 0, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                      __uuidof(ID3D12Resource), reinterpret_cast<void**>(&resource));
    heap->Release();
    if (!check(hr == S_OK && resource, "placed resource can outlive the caller's heap reference hr=0x%08lx", ul(hr))) {
        return;
    }

    void* data = nullptr;
    hr = resource->Map(0, nullptr, &data);
    if (check(hr == S_OK && data, "retained placed resource still maps hr=0x%08lx", ul(hr))) {
        *static_cast<uint8_t*>(data) = 0x5a;
        resource->Unmap(0, nullptr);
    }
    auto cfg = make_block(region_base, kRegionBytes, kFlagBacking);
    hr = D3D12ConfigureVirtualMemoryX(&cfg);
    check(hr == S_OK && last_log_u64(log_path, "gpu_heap_bytes=") == kRegionBytes + heap_bytes,
          "live placed resource keeps the original heap charge");

    if (capped) {
        ID3D12Heap* second = nullptr;
        hr = device->CreateHeap(&heap_desc, __uuidof(ID3D12Heap), reinterpret_cast<void**>(&second));
        check(hr == E_OUTOFMEMORY && !second, "second 300 MiB heap exceeds the 512 MiB cap hr=0x%08lx", ul(hr));
        if (second) second->Release();
    }

    resource->Release();
    hr = D3D12ConfigureVirtualMemoryX(&cfg);
    check(hr == S_OK && last_log_u64(log_path, "gpu_heap_bytes=") == kRegionBytes,
          "final placed resource release credits the original heap");

    if (capped) {
        ID3D12Heap* second = nullptr;
        hr = device->CreateHeap(&heap_desc, __uuidof(ID3D12Heap), reinterpret_cast<void**>(&second));
        check(hr == S_OK && second, "second 300 MiB heap fits after releasing the resource hr=0x%08lx", ul(hr));
        if (second) second->Release();
    }
}

/* #552: graphics-first / xmem-first pairing through the public exports.
 * With a 512 MiB cap, a 300 MiB D3D12 heap registers an external charge in
 * xmem. A 300 MiB XMemAlloc must then fail (300+300 > 512), and a 200 MiB
 * XMemAlloc must succeed (300+200 <= 512). After releasing the heap the
 * 300 MiB XMemAlloc must succeed. This is the graphics-first overcommit
 * that the private-ledger version allowed. */
void check_graphics_xmem_pairing(ID3D12Device* device, uint64_t cap) {
    if (cap != (512ull << 20)) {
        /* The pairing sizes are fixed against a 512 MiB cap, so at any other cap the
         * cross-DLL budget check does not run. Say so rather than pass silently. */
        ++g_skipped;
        std::printf("SKIP: graphics/xmem pairing needs a 512 MiB cap "
                    "(BX_MEM_LIMIT_MB=512 bx-cli run --inherit-env ...); this run's cap is %llu MiB\n",
                    static_cast<unsigned long long>(cap >> 20));
        return;
    }
    const uint64_t heap_bytes = 300ull << 20;
    const auto heap_desc = heap_of(D3D12_HEAP_TYPE_UPLOAD, heap_bytes, D3D12_HEAP_FLAG_NONE);
    ID3D12Heap* heap = nullptr;
    HRESULT hr = device->CreateHeap(&heap_desc, __uuidof(ID3D12Heap), reinterpret_cast<void**>(&heap));
    if (!check(hr == S_OK && heap, "pairing: 300 MiB graphics heap hr=0x%08lx", ul(hr))) {
        return;
    }

    bx_xmem_stats_t stats{};
    check(XMemGetWorkingSetStatistics(&stats, sizeof(stats)) != 0, "pairing: xmem stats readable");

    /* 300 MiB heap + 300 MiB xmem = 600 MiB > 512 MiB cap. */
    void* too_much = XMemAlloc(300ull << 20, 0);
    check(too_much == nullptr, "pairing: 300 MiB XMemAlloc on top of 300 MiB heap exceeds the 512 MiB cap");
    if (too_much) XMemFree(too_much, 0);

    /* 300 MiB heap + 200 MiB xmem = 500 MiB <= 512 MiB cap. */
    void* fits = XMemAlloc(200ull << 20, 0);
    check(fits != nullptr, "pairing: 200 MiB XMemAlloc fits beside the 300 MiB heap");
    if (fits) XMemFree(fits, 0);

    heap->Release();

    /* After the heap charge is released the 300 MiB allocation must fit. */
    void* after = XMemAlloc(300ull << 20, 0);
    check(after != nullptr, "pairing: 300 MiB XMemAlloc fits after releasing the graphics heap");
    if (after) XMemFree(after, 0);
}

/* CreatePlacedResource1 must accept a heap from CreateHeap. CreatePlacedResource2
   is checked only when the host device implements ID3D12Device10. */
void check_newer_placement(ID3D12Device* device) {
    D3D12_HEAP_DESC heap_desc = heap_of(D3D12_HEAP_TYPE_DEFAULT, kAlign, D3D12_HEAP_FLAG_NONE);
    ID3D12Heap* heap = nullptr;
    const HRESULT heap_hr = device->CreateHeap(&heap_desc, __uuidof(ID3D12Heap), reinterpret_cast<void**>(&heap));
    if (!check(heap_hr == S_OK && heap, "wrapped heap for newer placement calls hr=0x%08lx", ul(heap_hr))) {
        return;
    }
    D3D12_RESOURCE_DESC1 desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = 256;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ID3D12Device8* dev8 = nullptr;
    const HRESULT q8 = device->QueryInterface(__uuidof(ID3D12Device8), reinterpret_cast<void**>(&dev8));
    check(q8 == S_OK && dev8, "ID3D12Device8 for CreatePlacedResource1 hr=0x%08lx", ul(q8));
    if (dev8) {
        ID3D12Resource* resource = nullptr;
        const HRESULT placed = dev8->CreatePlacedResource1(heap, 0, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                           __uuidof(ID3D12Resource), reinterpret_cast<void**>(&resource));
        check(placed == S_OK && resource, "CreatePlacedResource1 accepts a wrapped CreateHeap hr=0x%08lx", ul(placed));
        if (resource) resource->Release();
        dev8->Release();
    }
    ID3D12Device10* dev10 = nullptr;
    const HRESULT q10 = device->QueryInterface(__uuidof(ID3D12Device10), reinterpret_cast<void**>(&dev10));
    if (q10 == S_OK && dev10) {
        ID3D12Resource* resource = nullptr;
        const HRESULT placed =
            dev10->CreatePlacedResource2(heap, 0, &desc, D3D12_BARRIER_LAYOUT_COMMON, nullptr, 0, nullptr,
                                         __uuidof(ID3D12Resource), reinterpret_cast<void**>(&resource));
        check(placed == S_OK && resource, "CreatePlacedResource2 accepts a wrapped CreateHeap hr=0x%08lx", ul(placed));
        if (resource) resource->Release();
        dev10->Release();
    } else {
        std::printf("CreatePlacedResource2 not checked: ID3D12Device10 hr=0x%08lx\n", ul(q10));
        std::fflush(stdout);
    }
    heap->Release();
}

void check_heap_get_device(ID3D12Heap* heap, ID3D12Device* proxy, ID3D12Device* host_device, const char* what) {
    if (!check(heap != nullptr, "%s exists for GetDevice", what)) {
        return;
    }
    ID3D12Device* got = nullptr;
    const HRESULT hr = heap->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&got));
    check(hr == S_OK && got == proxy, "%s GetDevice returns the device proxy pointer (hr=0x%08lx got=%p proxy=%p)", what,
          ul(hr), static_cast<void*>(got), static_cast<void*>(proxy));
    check(got != host_device && !same_object(got, host_device), "%s GetDevice is not the raw host device", what);
    if (got) {
        got->Release();
    }
}

bool same_object(IUnknown* a, IUnknown* b) {
    if (!a || !b) {
        return false;
    }
    IUnknown* ua = nullptr;
    IUnknown* ub = nullptr;
    const HRESULT ha = a->QueryInterface(__uuidof(IUnknown), reinterpret_cast<void**>(&ua));
    const HRESULT hb = b->QueryInterface(__uuidof(IUnknown), reinterpret_cast<void**>(&ub));
    const bool same = ha == S_OK && hb == S_OK && ua && ua == ub;
    if (ua) ua->Release();
    if (ub) ub->Release();
    return same;
}

ID3D12Device* open_host_device() {
    using CreateFn = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
    HMODULE mod = LoadLibraryA("d3d12.dll");
    if (!mod) {
        return nullptr;
    }
    auto create = reinterpret_cast<CreateFn>(reinterpret_cast<void*>(GetProcAddress(mod, "D3D12CreateDevice")));
    if (!create) {
        return nullptr;
    }
    ID3D12Device* device = nullptr;
    if (create(nullptr, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), reinterpret_cast<void**>(&device)) != S_OK) {
        return nullptr;
    }
    return device;
}

bool wait_fence(ID3D12CommandQueue* queue, ID3D12Fence* fence, UINT64 value) {
    HRESULT hr = queue->Signal(fence, value);
    if (hr != S_OK) {
        return check(false, "queue Signal hr=0x%08lx", ul(hr));
    }
    if (fence->GetCompletedValue() >= value) {
        return true;
    }
    HANDLE done = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    hr = fence->SetEventOnCompletion(value, done);
    if (hr != S_OK || !done) {
        if (done) CloseHandle(done);
        return check(false, "fence SetEventOnCompletion hr=0x%08lx", ul(hr));
    }
    DWORD waited = WaitForSingleObject(done, 10000);
    CloseHandle(done);
    return check(waited == WAIT_OBJECT_0 && fence->GetCompletedValue() >= value,
                 "fence must reach %llu (wait=%lu completed=%llu)", static_cast<unsigned long long>(value), waited,
                 static_cast<unsigned long long>(fence->GetCompletedValue()));
}

struct StackSync {
    uint64_t base;
    uint64_t size;
    HANDLE ready;
    HANDLE done;
    HRESULT hr;
    int intact;
};

DWORD WINAPI stack_config_thread(LPVOID param) {
    auto* sync = static_cast<StackSync*>(param);
    BxD3d12VirtualMemoryConfig block = make_block(sync->base, sync->size, kFlagBacking);
    sync->hr = D3D12ConfigureVirtualMemoryX(&block);
    volatile uint8_t* raw = reinterpret_cast<volatile uint8_t*>(&block);
    for (size_t i = 0; i < sizeof(block); ++i) {
        raw[i] = 0xA5;
    }
    SetEvent(sync->ready);
    WaitForSingleObject(sync->done, INFINITE);
    int intact = 1;
    for (size_t i = 0; i < sizeof(block); ++i) {
        if (raw[i] != 0xA5) {
            intact = 0;
        }
    }
    sync->intact = intact;
    return 0;
}

void expect_bytes_unchanged(void* block, const void* before, size_t bytes, const char* what) {
    check(std::memcmp(block, before, bytes) == 0, "%s leaves every byte unchanged", what);
}

} // namespace

int main() {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);

    const bx_runtime_config_t* cfg = bx_get_runtime_config();
    if (!check(cfg && cfg->title_memory_limit_bytes >= kAlign, "runtime config must publish a title memory limit")) {
        std::fprintf(stderr, "FAIL: test_d3d12_placed (no runtime config)\n");
        return 1;
    }
    const char* profile = cfg->profile == BX_PROFILE_ANACONDA ? "anaconda" : "lockhart";
    char log_path[MAX_PATH];
    DWORD log_len = GetEnvironmentVariableA("BX_LOG_FILE", log_path, sizeof(log_path));
    const bool have_log = log_len > 0 && log_len < sizeof(log_path);

    check(D3D12ConfigureVirtualMemoryX(nullptr) == S_OK, "null configuration is a logged stub that returns S_OK");

    uint8_t wrong_magic[64];
    std::memset(wrong_magic, 0xA5, sizeof(wrong_magic));
    uint8_t wrong_magic_before[64];
    std::memcpy(wrong_magic_before, wrong_magic, sizeof(wrong_magic));
    check(D3D12ConfigureVirtualMemoryX(wrong_magic) == S_OK, "wrong magic returns S_OK");
    expect_bytes_unchanged(wrong_magic, wrong_magic_before, sizeof(wrong_magic), "wrong magic");

    uint8_t wrong_size[64];
    std::memset(wrong_size, 0xA5, sizeof(wrong_size));
    const uint32_t magic = kMagic;
    const uint32_t short_size = 24;
    std::memcpy(wrong_size, &magic, sizeof(magic));
    std::memcpy(wrong_size + 4, &short_size, sizeof(short_size));
    uint8_t wrong_size_before[64];
    std::memcpy(wrong_size_before, wrong_size, sizeof(wrong_size));
    check(D3D12ConfigureVirtualMemoryX(wrong_size) == S_OK, "right magic with the wrong struct size returns S_OK");
    expect_bytes_unchanged(wrong_size, wrong_size_before, sizeof(wrong_size), "wrong struct size");

    SYSTEM_INFO sys{};
    GetSystemInfo(&sys);
    const SIZE_T page_bytes = sys.dwPageSize ? sys.dwPageSize : 4096;
    void* edge_mem = VirtualAlloc(nullptr, page_bytes * 2, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    check(edge_mem != nullptr, "VirtualAlloc for the 8-byte header probe");
    if (edge_mem) {
        DWORD edge_old = 0;
        auto* edge_base = static_cast<uint8_t*>(edge_mem);
        check(VirtualProtect(edge_base + page_bytes, page_bytes, PAGE_NOACCESS, &edge_old) != 0,
              "page after the header probe is NOACCESS");
        uint8_t* edge = edge_base + page_bytes - 8;
        uint32_t bad_magic = 0x11223344u;
        uint32_t bad_size = 0x55667788u;
        std::memcpy(edge, &bad_magic, 4);
        std::memcpy(edge + 4, &bad_size, 4);
        HRESULT edge_hr = D3D12ConfigureVirtualMemoryX(edge);
        check(edge_hr == S_OK, "wrong magic at a page edge returns S_OK without reading past 8 bytes hr=0x%08lx",
              ul(edge_hr));
        uint32_t good_magic = kMagic;
        uint32_t eight = 8;
        std::memcpy(edge, &good_magic, 4);
        std::memcpy(edge + 4, &eight, 4);
        edge_hr = D3D12ConfigureVirtualMemoryX(edge);
        check(edge_hr == S_OK, "right magic and wrong size at a page edge returns S_OK hr=0x%08lx", ul(edge_hr));
        VirtualProtect(edge_base + page_bytes, page_bytes, PAGE_READWRITE, &edge_old);
        VirtualFree(edge_mem, 0, MEM_RELEASE);
    }

    BxD3d12VirtualMemoryConfig bad = make_block(0, 0, 0);
    bad.version = 2;
    bad.cpu_pointer = 0xA5A5A5A5A5A5A5A5ull;
    BxD3d12VirtualMemoryConfig bad_before = bad;
    check(D3D12ConfigureVirtualMemoryX(&bad) == S_OK, "version 2 is an unrecognised block and returns S_OK");
    expect_bytes_unchanged(&bad, &bad_before, sizeof(bad), "version 2");

    bad = make_block(0, 0, 0x2u);
    bad.cpu_pointer = 0xA5A5A5A5A5A5A5A5ull;
    bad_before = bad;
    check(D3D12ConfigureVirtualMemoryX(&bad) == S_OK, "unknown flag bit 0x2 is an unrecognised block and returns S_OK");
    expect_bytes_unchanged(&bad, &bad_before, sizeof(bad), "unknown flag bit");

    bad = make_block(0, 0, 0);
    bad.reserved = 1;
    bad.cpu_pointer = 0xA5A5A5A5A5A5A5A5ull;
    bad_before = bad;
    check(D3D12ConfigureVirtualMemoryX(&bad) == S_OK, "a non-zero reserved word is an unrecognised block and returns S_OK");
    expect_bytes_unchanged(&bad, &bad_before, sizeof(bad), "non-zero reserved");

    bad = make_block(0, 1000, 0);
    bad.cpu_pointer = 0xA5A5A5A5A5A5A5A5ull;
    bad_before = bad;
    check(D3D12ConfigureVirtualMemoryX(&bad) == E_INVALIDARG, "size that is not a multiple of 64 KiB must be rejected");
    expect_bytes_unchanged(&bad, &bad_before, sizeof(bad), "rejected size");

    bad = make_block(0, cfg->title_memory_limit_bytes + kAlign, 0);
    bad.cpu_pointer = 0xA5A5A5A5A5A5A5A5ull;
    bad_before = bad;
    check(D3D12ConfigureVirtualMemoryX(&bad) == E_INVALIDARG, "size above the title limit must be rejected");
    expect_bytes_unchanged(&bad, &bad_before, sizeof(bad), "rejected title limit");

    bad = make_block(0x1000, kAlign, 0);
    check(D3D12ConfigureVirtualMemoryX(&bad) == E_INVALIDARG, "a VA that is not an xmem region must be rejected");

    BxD3d12VirtualMemoryConfig opt_only = make_block(0, 0, kFlagBacking);
    const BxD3d12VirtualMemoryConfig opt_sent = opt_only;
    check(D3D12ConfigureVirtualMemoryX(&opt_only) == S_OK, "BACKING_HEAP alone is a recognised block");
    check(inputs_unchanged(opt_only, opt_sent), "BACKING_HEAP input fields stay unchanged");
    check(opt_only.cpu_pointer == 0, "BACKING_HEAP with no heap yet writes cpu_pointer 0");

    SIZE_T page_count = 2;
    ULONG_PTR pages[2] = {};
    uint8_t* view = nullptr;
    bool mapped = XMemAllocatePhysicalPages(&page_count, pages, 0) && page_count == 2 && pages[0] != 0 && pages[1] != 0;
    if (mapped) {
        view = static_cast<uint8_t*>(XMemMapPhysicalPages(nullptr, page_count, 0, pages));
        mapped = view != nullptr && (reinterpret_cast<uintptr_t>(view) % kAlign) == 0;
        if (mapped) {
            view[kAlign] = 0x5A;
            mapped = view[kAlign] == 0x5A;
        }
    }
    check(mapped, "xmem must map two contiguous 64 KiB pages");
    const uint64_t region_base = reinterpret_cast<uint64_t>(view);

    if (cfg->title_memory_limit_bytes == (512ull << 20)) {
        check_concurrent_device_startup();
    }
    ID3D12Device* device = nullptr;
    HRESULT hr = D3D12XboxCreateDevice(nullptr, __uuidof(ID3D12Device), reinterpret_cast<void**>(&device));
    if (!check(hr == S_OK && device, "D3D12XboxCreateDevice hr=0x%08lx", ul(hr))) {
        std::fprintf(stderr, "FAIL: test_d3d12_placed (no device. %d of %d checks failed)\n", g_failures, g_checks);
        return 1;
    }
    ID3D12Device* host = open_host_device();
    check(host != nullptr, "host d3d12.dll device opened for ordinary-heap comparison");

    BxD3d12VirtualMemoryConfig plain_region = make_block(region_base, kRegionBytes, 0);
    hr = mapped ? D3D12ConfigureVirtualMemoryX(&plain_region) : E_FAIL;
    check(hr == S_OK, "region without the opt-in flag hr=0x%08lx", ul(hr));
    check(plain_region.cpu_pointer == 0, "cpu_pointer stays 0 until the backing heap exists");

    D3D12_HEAP_DESC custom_desc = backing_desc();
    ID3D12Heap* custom_a = nullptr;
    ID3D12Heap* custom_b = nullptr;
    hr = device->CreateHeap(&custom_desc, __uuidof(ID3D12Heap), reinterpret_cast<void**>(&custom_a));
    HRESULT hr_b = device->CreateHeap(&custom_desc, __uuidof(ID3D12Heap), reinterpret_cast<void**>(&custom_b));
    check(hr == S_OK && hr_b == S_OK && custom_a && custom_b, "CUSTOM heaps without the opt-in hr=0x%08lx/0x%08lx",
          ul(hr), ul(hr_b));
    check(!same_object(custom_a, custom_b), "without the opt-in, two CUSTOM WRITE_BACK L0 heaps are distinct");
    if (custom_a && custom_b) {
        D3D12_HEAP_DESC got_a = custom_a->GetDesc();
        D3D12_HEAP_DESC got_b = custom_b->GetDesc();
        check(got_a.Properties.Type == D3D12_HEAP_TYPE_CUSTOM && got_b.Properties.Type == D3D12_HEAP_TYPE_CUSTOM,
              "opt-in-off CUSTOM heaps keep type CUSTOM");
        check(got_a.Properties.CPUPageProperty == D3D12_CPU_PAGE_PROPERTY_WRITE_BACK &&
                  got_b.Properties.CPUPageProperty == D3D12_CPU_PAGE_PROPERTY_WRITE_BACK,
              "opt-in-off CUSTOM heaps keep WRITE_BACK");
    }

    StackSync sync{};
    sync.base = region_base;
    sync.size = kRegionBytes;
    sync.hr = E_FAIL;
    sync.intact = 0;
    sync.ready = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    sync.done = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    HANDLE stack_thread = nullptr;
    if (mapped && sync.ready && sync.done) {
        stack_thread = CreateThread(nullptr, 0, stack_config_thread, &sync, 0, nullptr);
    }
    check(stack_thread != nullptr, "stack config thread started");
    if (stack_thread) {
        const DWORD ready = WaitForSingleObject(sync.ready, 10000);
        check(ready == WAIT_OBJECT_0 && sync.hr == S_OK, "stack block configure returned S_OK before CreateHeap (wait=%lu hr=0x%08lx)",
              ready, ul(sync.hr));
    }

    void* dead_page = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    check(dead_page != nullptr, "VirtualAlloc for the config lifetime page");
    if (dead_page && mapped) {
        auto* dead_block = static_cast<BxD3d12VirtualMemoryConfig*>(dead_page);
        *dead_block = make_block(region_base, kRegionBytes, kFlagBacking);
        hr = D3D12ConfigureVirtualMemoryX(dead_block);
        check(hr == S_OK, "page config before it is made inaccessible hr=0x%08lx", ul(hr));
        DWORD dead_old = 0;
        check(VirtualProtect(dead_page, 4096, PAGE_NOACCESS, &dead_old) != 0,
              "config page is PAGE_NOACCESS before CreateHeap");
    }

    BxD3d12VirtualMemoryConfig live = make_block(region_base, kRegionBytes, kFlagBacking);
    const BxD3d12VirtualMemoryConfig live_sent = live;
    hr = mapped ? D3D12ConfigureVirtualMemoryX(&live) : E_FAIL;
    check(hr == S_OK, "live configure before the backing heap hr=0x%08lx", ul(hr));
    check(inputs_unchanged(live, live_sent), "live configure leaves input fields unchanged");
    check(live.cpu_pointer == 0, "live cpu_pointer is 0 before the backing heap exists");

    ID3D12Heap* backing = nullptr;
    hr = device->CreateHeap(&custom_desc, __uuidof(ID3D12Heap), reinterpret_cast<void**>(&backing));
    check(hr == S_OK && backing, "explicit backing CreateHeap hr=0x%08lx", ul(hr));
    if (backing) {
        D3D12_HEAP_DESC got = backing->GetDesc();
        check(got.Properties.Type == D3D12_HEAP_TYPE_CUSTOM, "backing heap type is CUSTOM");
        check(got.Properties.CPUPageProperty == D3D12_CPU_PAGE_PROPERTY_WRITE_BACK, "backing heap is WRITE_BACK");
        check(got.Properties.MemoryPoolPreference == D3D12_MEMORY_POOL_L0, "backing heap is L0");
        check(got.Flags == D3D12_HEAP_FLAG_NONE, "backing heap flags are NONE");
        check(got.SizeInBytes == kRegionBytes && (got.Alignment == 0 || got.Alignment == kAlign),
              "backing heap is the 128 KiB region on a 64 KiB alignment");
    }
    check(!same_object(custom_a, backing) && !same_object(custom_b, backing),
          "CUSTOM heaps created without the opt-in are not the backing heap");
    check_heap_get_device(backing, device, host, "backing heap");
    if (custom_a) {
        check_heap_get_device(custom_a, device, host, "ordinary CUSTOM heap");
    }

    live.cpu_pointer = 0;
    hr = D3D12ConfigureVirtualMemoryX(&live);
    check(hr == S_OK && live.cpu_pointer != 0, "second configure writes the non-zero map hr=0x%08lx ptr=0x%llx", ul(hr),
          static_cast<unsigned long long>(live.cpu_pointer));
    const uint64_t map_pointer = live.cpu_pointer;
    live.cpu_pointer = 0;
    hr = D3D12ConfigureVirtualMemoryX(&live);
    check(hr == S_OK && live.cpu_pointer == map_pointer, "another configure of the same region writes the same map");

    ID3D12Heap* again = nullptr;
    hr = device->CreateHeap(&custom_desc, __uuidof(ID3D12Heap), reinterpret_cast<void**>(&again));
    check(hr == S_OK && same_object(again, backing), "a later explicit request returns the same backing heap");
    if (again) again->Release();

    D3D12_HEAP_DESC upload_region = heap_of(D3D12_HEAP_TYPE_UPLOAD, kRegionBytes, D3D12_HEAP_FLAG_NONE);
    D3D12_HEAP_DESC deny_region = heap_of(D3D12_HEAP_TYPE_DEFAULT, kRegionBytes, D3D12_HEAP_FLAG_DENY_BUFFERS);
    ID3D12Heap* upload_same = nullptr;
    ID3D12Heap* deny_same = nullptr;
    ID3D12Heap* upload_host = nullptr;
    ID3D12Heap* deny_host = nullptr;
    HRESULT upload_hr = device->CreateHeap(&upload_region, __uuidof(ID3D12Heap), reinterpret_cast<void**>(&upload_same));
    HRESULT deny_hr = device->CreateHeap(&deny_region, __uuidof(ID3D12Heap), reinterpret_cast<void**>(&deny_same));
    HRESULT upload_host_hr = host ? host->CreateHeap(&upload_region, __uuidof(ID3D12Heap), reinterpret_cast<void**>(&upload_host))
                                  : E_FAIL;
    HRESULT deny_host_hr = host ? host->CreateHeap(&deny_region, __uuidof(ID3D12Heap), reinterpret_cast<void**>(&deny_host))
                                : E_FAIL;
    check(upload_hr == S_OK && upload_same, "128 KiB UPLOAD heap hr=0x%08lx", ul(upload_hr));
    check(deny_hr == S_OK && deny_same, "128 KiB DEFAULT|DENY_BUFFERS heap hr=0x%08lx", ul(deny_hr));
    check(upload_hr == upload_host_hr && deny_hr == deny_host_hr,
          "same-size ordinary heaps match the host (upload 0x%08lx/0x%08lx deny 0x%08lx/0x%08lx)", ul(upload_hr),
          ul(upload_host_hr), ul(deny_hr), ul(deny_host_hr));
    check(!same_object(upload_same, backing) && !same_object(deny_same, backing) && !same_object(upload_same, deny_same),
          "UPLOAD and DEFAULT|DENY_BUFFERS heaps are distinct from the backing heap and from each other");
    if (upload_same && upload_host && deny_same && deny_host) {
        D3D12_HEAP_DESC up = upload_same->GetDesc();
        D3D12_HEAP_DESC up_h = upload_host->GetDesc();
        D3D12_HEAP_DESC deny = deny_same->GetDesc();
        D3D12_HEAP_DESC deny_h = deny_host->GetDesc();
        check(up.Properties.Type == D3D12_HEAP_TYPE_UPLOAD && up.Properties.Type == up_h.Properties.Type,
              "128 KiB UPLOAD GetDesc type stays UPLOAD");
        check(deny.Properties.Type == D3D12_HEAP_TYPE_DEFAULT && deny.Properties.Type == deny_h.Properties.Type,
              "128 KiB DEFAULT GetDesc type stays DEFAULT");
        check(up.Flags == up_h.Flags && deny.Flags == deny_h.Flags, "ordinary heap flags match the host heap");
        check(up.Properties.Type != D3D12_HEAP_TYPE_CUSTOM && deny.Properties.Type != D3D12_HEAP_TYPE_CUSTOM,
              "ordinary heaps do not report CUSTOM");
    }

    D3D12_HEAP_DESC upload_odd = heap_of(D3D12_HEAP_TYPE_UPLOAD, 100000, D3D12_HEAP_FLAG_NONE);
    ID3D12Heap* odd = nullptr;
    ID3D12Heap* odd_host = nullptr;
    HRESULT odd_hr = device->CreateHeap(&upload_odd, __uuidof(ID3D12Heap), reinterpret_cast<void**>(&odd));
    HRESULT odd_host_hr = host ? host->CreateHeap(&upload_odd, __uuidof(ID3D12Heap), reinterpret_cast<void**>(&odd_host))
                               : E_FAIL;
    uint64_t odd_size = 0;
    uint64_t odd_host_size = 0;
    check(odd_hr == S_OK && odd, "100000-byte UPLOAD heap hr=0x%08lx", ul(odd_hr));
    check(odd_hr == odd_host_hr, "100000-byte UPLOAD HRESULT matches the host (hooked=0x%08lx host=0x%08lx)", ul(odd_hr),
          ul(odd_host_hr));
    if (odd && odd_host) {
        D3D12_HEAP_DESC og = odd->GetDesc();
        D3D12_HEAP_DESC oh = odd_host->GetDesc();
        odd_size = og.SizeInBytes;
        odd_host_size = oh.SizeInBytes;
        check(og.SizeInBytes == oh.SizeInBytes && og.Properties.Type == D3D12_HEAP_TYPE_UPLOAD &&
                  og.Properties.Type == oh.Properties.Type && og.Flags == oh.Flags,
              "100000-byte UPLOAD GetDesc matches the host (size %llu type %d flags 0x%x)",
              static_cast<unsigned long long>(og.SizeInBytes), static_cast<int>(og.Properties.Type),
              static_cast<unsigned>(og.Flags));
    }
    std::printf("upload_100000 hooked=0x%08lx host=0x%08lx size=%llu host_size=%llu\n", ul(odd_hr), ul(odd_host_hr),
                static_cast<unsigned long long>(odd_size), static_cast<unsigned long long>(odd_host_size));
    std::fflush(stdout);

    D3D12_HEAP_DESC align_probe = heap_of(D3D12_HEAP_TYPE_DEFAULT, kAlign, D3D12_HEAP_FLAG_NONE);
    align_probe.Alignment = 4096;
    ID3D12Heap* align_hook = nullptr;
    ID3D12Heap* align_host = nullptr;
    HRESULT align_hr = device->CreateHeap(&align_probe, __uuidof(ID3D12Heap), reinterpret_cast<void**>(&align_hook));
    HRESULT align_host_hr =
        host ? host->CreateHeap(&align_probe, __uuidof(ID3D12Heap), reinterpret_cast<void**>(&align_host)) : E_FAIL;
    check(align_hr == align_host_hr, "alignment 4096 CreateHeap matches the host (hooked=0x%08lx host=0x%08lx)",
          ul(align_hr), ul(align_host_hr));
    if (FAILED(align_hr)) {
        check(align_hook == nullptr, "failed alignment-4096 CreateHeap does not return a heap");
    }

    if (stack_thread && sync.done) {
        SetEvent(sync.done);
        const DWORD joined = WaitForSingleObject(stack_thread, 10000);
        check(joined == WAIT_OBJECT_0 && sync.intact == 1,
              "stack config block was not written after the call returned (wait=%lu intact=%d)", joined, sync.intact);
        CloseHandle(stack_thread);
        stack_thread = nullptr;
    }
    if (dead_page) {
        DWORD dead_old = 0;
        VirtualProtect(dead_page, 4096, PAGE_READWRITE, &dead_old);
        VirtualFree(dead_page, 0, MEM_RELEASE);
        dead_page = nullptr;
        std::printf("config_page_noaccess_survived=1\n");
        std::fflush(stdout);
    }

    BxD3d12VirtualMemoryConfig opted_out = make_block(region_base, kRegionBytes, 0);
    hr = D3D12ConfigureVirtualMemoryX(&opted_out);
    check(hr == S_OK && opted_out.cpu_pointer == map_pointer,
          "same region without the flag keeps the backing heap and writes the same map hr=0x%08lx", ul(hr));
    ID3D12Heap* custom_later = nullptr;
    hr = device->CreateHeap(&custom_desc, __uuidof(ID3D12Heap), reinterpret_cast<void**>(&custom_later));
    check(hr == S_OK && custom_later && !same_object(custom_later, backing),
          "CUSTOM WRITE_BACK L0 without the opt-in is a separate ordinary heap hr=0x%08lx", ul(hr));
    if (custom_later) {
        D3D12_HEAP_DESC later = custom_later->GetDesc();
        check(later.Properties.Type == D3D12_HEAP_TYPE_CUSTOM &&
                  later.Properties.CPUPageProperty == D3D12_CPU_PAGE_PROPERTY_WRITE_BACK &&
                  later.Properties.MemoryPoolPreference == D3D12_MEMORY_POOL_L0,
              "opt-in-off heap keeps the CUSTOM WRITE_BACK L0 desc it requested");
    }

    live.flags = kFlagBacking;
    live.cpu_pointer = 0;
    hr = D3D12ConfigureVirtualMemoryX(&live);
    check(hr == S_OK && live.cpu_pointer == map_pointer, "restoring the opt-in does not recreate the backing heap");

    uint64_t small_align = 0;
    uint64_t small_bytes = 0;
    HRESULT small_hr = E_FAIL;
    HRESULT small_host_hr = E_FAIL;
    ID3D12Resource* small_res = nullptr;
    ID3D12Resource* small_host_res = nullptr;
    ID3D12Heap* small_heap = nullptr;
    ID3D12Heap* small_host_heap = nullptr;
    {
        struct Candidate {
            const char* name;
            UINT width;
            UINT height;
            DXGI_FORMAT format;
        };
        const Candidate candidates[] = {
            {"rgba8_1x1", 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM}, {"rgba8_4x4", 4, 4, DXGI_FORMAT_R8G8B8A8_UNORM},
            {"rgba8_8x8", 8, 8, DXGI_FORMAT_R8G8B8A8_UNORM}, {"r8_8x8", 8, 8, DXGI_FORMAT_R8_UNORM},
            {"r8_16x16", 16, 16, DXGI_FORMAT_R8_UNORM},     {"rgba8_16x16", 16, 16, DXGI_FORMAT_R8G8B8A8_UNORM},
        };
        ID3D12Device* probe = host ? host : device;
        D3D12_RESOURCE_DESC chosen{};
        bool have_chosen = false;
        bool chose_4k = false;
        for (const Candidate& candidate : candidates) {
            D3D12_RESOURCE_DESC desc = texture_desc();
            desc.Width = candidate.width;
            desc.Height = candidate.height;
            desc.Format = candidate.format;
            desc.Alignment = D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT;
            D3D12_RESOURCE_ALLOCATION_INFO info = probe->GetResourceAllocationInfo(0, 1, &desc);
            std::printf("small_resource %s alloc_align=%llu alloc_bytes=%llu desc_align=%llu\n", candidate.name,
                        static_cast<unsigned long long>(info.Alignment), static_cast<unsigned long long>(info.SizeInBytes),
                        static_cast<unsigned long long>(desc.Alignment));
            const bool usable = info.Alignment != 0 && info.Alignment != ~0ull && info.SizeInBytes != 0 &&
                                info.SizeInBytes != ~0ull && info.Alignment < kAlign;
            if (!usable) {
                continue;
            }
            if (info.Alignment == D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT) {
                chosen = desc;
                small_align = info.Alignment;
                small_bytes = info.SizeInBytes;
                have_chosen = true;
                chose_4k = true;
            } else if (!chose_4k && (!have_chosen || info.Alignment < small_align)) {
                chosen = desc;
                small_align = info.Alignment;
                small_bytes = info.SizeInBytes;
                have_chosen = true;
            }
        }
        std::fflush(stdout);
        const uint64_t place_at = have_chosen ? small_align : 4096ull;
        D3D12_RESOURCE_DESC placed_desc = have_chosen ? chosen : texture_desc();
        if (!have_chosen) {
            placed_desc.Alignment = D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT;
            D3D12_RESOURCE_ALLOCATION_INFO info = probe->GetResourceAllocationInfo(0, 1, &placed_desc);
            small_align = info.Alignment;
            small_bytes = info.SizeInBytes;
            std::printf("small_placement: no alignment below 64 KiB. Comparing offset 4096 with the host "
                        "(reported align=%llu bytes=%llu)\n",
                        static_cast<unsigned long long>(info.Alignment), static_cast<unsigned long long>(info.SizeInBytes));
        }
        const uint64_t need = place_at + (small_bytes == 0 || small_bytes == ~0ull ? kAlign : small_bytes);
        uint64_t heap_bytes = (need + (kAlign - 1)) & ~(kAlign - 1);
        if (heap_bytes < kAlign) {
            heap_bytes = kAlign;
        }
        D3D12_HEAP_DESC small_heap_desc = heap_of(D3D12_HEAP_TYPE_DEFAULT, heap_bytes, D3D12_HEAP_FLAG_NONE);
        hr = device->CreateHeap(&small_heap_desc, __uuidof(ID3D12Heap), reinterpret_cast<void**>(&small_heap));
        HRESULT host_heap_hr =
            host ? host->CreateHeap(&small_heap_desc, __uuidof(ID3D12Heap), reinterpret_cast<void**>(&small_host_heap))
                 : E_FAIL;
        check(hr == host_heap_hr && (FAILED(hr) || small_heap),
              "ordinary heap for small placement matches the host (hooked=0x%08lx host=0x%08lx bytes=%llu)", ul(hr),
              ul(host_heap_hr), static_cast<unsigned long long>(heap_bytes));
        if (small_heap) {
            small_hr = device->CreatePlacedResource(small_heap, place_at, &placed_desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                                    nullptr, __uuidof(ID3D12Resource), reinterpret_cast<void**>(&small_res));
        }
        if (small_host_heap) {
            small_host_hr = host->CreatePlacedResource(small_host_heap, place_at, &placed_desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                                       nullptr, __uuidof(ID3D12Resource),
                                                       reinterpret_cast<void**>(&small_host_res));
        }
        std::printf("small_placement align=%llu offset=%llu bytes=%llu hooked=0x%08lx host=0x%08lx chose_4k=%d\n",
                    static_cast<unsigned long long>(small_align), static_cast<unsigned long long>(place_at),
                    static_cast<unsigned long long>(small_bytes), ul(small_hr), ul(small_host_hr), chose_4k ? 1 : 0);
        std::fflush(stdout);
        check(small_hr == small_host_hr, "ordinary small-resource placement matches the host (hooked=0x%08lx host=0x%08lx)",
              ul(small_hr), ul(small_host_hr));
        if (have_chosen) {
            check(small_hr == S_OK && small_res, "small resource placed at runtime alignment %llu hr=0x%08lx",
                  static_cast<unsigned long long>(place_at), ul(small_hr));
        }
    }

    HRESULT heap1_hr = E_FAIL;
    HRESULT heap1_host_hr = E_FAIL;
    ID3D12Heap* heap1_base = nullptr;
    ID3D12Heap* heap1_host_base = nullptr;
    ID3D12Heap1* heap1 = nullptr;
    ID3D12Heap1* heap1_host = nullptr;
    {
        D3D12_HEAP_DESC plain = heap_of(D3D12_HEAP_TYPE_DEFAULT, kAlign, D3D12_HEAP_FLAG_NONE);
        hr = device->CreateHeap(&plain, __uuidof(ID3D12Heap), reinterpret_cast<void**>(&heap1_base));
        HRESULT host_base_hr =
            host ? host->CreateHeap(&plain, __uuidof(ID3D12Heap), reinterpret_cast<void**>(&heap1_host_base)) : E_FAIL;
        check(hr == S_OK && heap1_base, "heap used for ID3D12Heap1 hr=0x%08lx", ul(hr));
        heap1_hr = heap1_base ? heap1_base->QueryInterface(__uuidof(ID3D12Heap1), reinterpret_cast<void**>(&heap1))
                              : E_FAIL;
        heap1_host_hr = heap1_host_base
                            ? heap1_host_base->QueryInterface(__uuidof(ID3D12Heap1), reinterpret_cast<void**>(&heap1_host))
                            : E_FAIL;
        std::printf("ID3D12Heap1 hooked=0x%08lx host=0x%08lx base_host=0x%08lx\n", ul(heap1_hr), ul(heap1_host_hr),
                    ul(host_base_hr));
        std::fflush(stdout);
        check(heap1_hr == heap1_host_hr, "ID3D12Heap1 QueryInterface matches the host (hooked=0x%08lx host=0x%08lx)",
              ul(heap1_hr), ul(heap1_host_hr));
        if (heap1_host_hr == S_OK) {
            check(heap1_hr == S_OK && heap1, "hooked CreateHeap heap answers ID3D12Heap1");
            IUnknown* session = nullptr;
            IUnknown* session_host = nullptr;
            const HRESULT session_hr =
                heap1->GetProtectedResourceSession(__uuidof(IUnknown), reinterpret_cast<void**>(&session));
            const HRESULT session_host_hr =
                heap1_host->GetProtectedResourceSession(__uuidof(IUnknown), reinterpret_cast<void**>(&session_host));
            std::printf("GetProtectedResourceSession hooked=0x%08lx host=0x%08lx\n", ul(session_hr), ul(session_host_hr));
            check(session_hr == session_host_hr, "GetProtectedResourceSession matches the host (hooked=0x%08lx host=0x%08lx)",
                  ul(session_hr), ul(session_host_hr));
            if (session) session->Release();
            if (session_host) session_host->Release();
            ID3D12Heap1* created = nullptr;
            ID3D12Heap1* created_host = nullptr;
            const HRESULT created_hr =
                device->CreateHeap(&plain, __uuidof(ID3D12Heap1), reinterpret_cast<void**>(&created));
            const HRESULT created_host_hr =
                host->CreateHeap(&plain, __uuidof(ID3D12Heap1), reinterpret_cast<void**>(&created_host));
            std::printf("CreateHeap ID3D12Heap1 hooked=0x%08lx host=0x%08lx\n", ul(created_hr), ul(created_host_hr));
            check(created_hr == S_OK && created && created_hr == created_host_hr,
                  "CreateHeap riid ID3D12Heap1 returns a heap (hooked=0x%08lx host=0x%08lx)", ul(created_hr),
                  ul(created_host_hr));
            if (created) created->Release();
            if (created_host) created_host->Release();
        } else {
            std::printf("ID3D12Heap1: host cannot answer it (hr=0x%08lx). Hooked result matches.\n", ul(heap1_host_hr));
            ID3D12Heap1* created = nullptr;
            ID3D12Heap1* created_host = nullptr;
            const HRESULT created_hr =
                device->CreateHeap(&plain, __uuidof(ID3D12Heap1), reinterpret_cast<void**>(&created));
            const HRESULT created_host_hr =
                host ? host->CreateHeap(&plain, __uuidof(ID3D12Heap1), reinterpret_cast<void**>(&created_host)) : E_FAIL;
            std::printf("CreateHeap ID3D12Heap1 hooked=0x%08lx host=0x%08lx\n", ul(created_hr), ul(created_host_hr));
            std::fflush(stdout);
            check(created_hr == created_host_hr,
                  "CreateHeap riid ID3D12Heap1 matches the host (hooked=0x%08lx host=0x%08lx)", ul(created_hr),
                  ul(created_host_hr));
            if (created) created->Release();
            if (created_host) created_host->Release();
        }
    }

    ID3D12Resource* placed_buf = nullptr;
    ID3D12Resource* placed_tex = nullptr;
    if (backing) {
        D3D12_RESOURCE_DESC tiny = buffer_desc(kBufBytes);
        D3D12_RESOURCE_ALLOCATION_INFO tiny_info = device->GetResourceAllocationInfo(0, 1, &tiny);
        std::printf("backing_buffer_alloc align=%llu bytes=%llu\n", static_cast<unsigned long long>(tiny_info.Alignment),
                    static_cast<unsigned long long>(tiny_info.SizeInBytes));
        std::fflush(stdout);
        if (tiny_info.Alignment != 0 && tiny_info.SizeInBytes != 0 && tiny_info.SizeInBytes != ~0ull &&
            (4096ull % tiny_info.Alignment) != 0) {
            ID3D12Resource* misaligned = nullptr;
            hr = device->CreatePlacedResource(backing, 4096, &tiny, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                              __uuidof(ID3D12Resource), reinterpret_cast<void**>(&misaligned));
            check(hr == E_INVALIDARG && misaligned == nullptr,
                  "backing heap rejects offset 4096 because allocation alignment is %llu hr=0x%08lx",
                  static_cast<unsigned long long>(tiny_info.Alignment), ul(hr));
            if (misaligned) misaligned->Release();
        } else {
            check(false, "backing buffer alignment %llu does not make offset 4096 a real rejection",
                  static_cast<unsigned long long>(tiny_info.Alignment));
        }

        D3D12_RESOURCE_DESC buf_desc = buffer_desc(kUploadBytes);
        hr = device->CreatePlacedResource(backing, 0, &buf_desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                          __uuidof(ID3D12Resource), reinterpret_cast<void**>(&placed_buf));
        check(hr == S_OK && placed_buf, "placed buffer at offset 0 hr=0x%08lx", ul(hr));

        D3D12_RESOURCE_DESC tex_desc = texture_desc();
        hr = device->CreatePlacedResource(backing, kAlign, &tex_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                          __uuidof(ID3D12Resource), reinterpret_cast<void**>(&placed_tex));
        check(hr == S_OK && placed_tex, "placed RGBA8 texture at offset 64 KiB hr=0x%08lx", ul(hr));
    }

    ID3D12Resource* readback = nullptr;
    D3D12_HEAP_PROPERTIES readback_props{};
    readback_props.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC staging_desc = buffer_desc(kUploadBytes);
    if (placed_buf && placed_tex) {
        hr = device->CreateCommittedResource(&readback_props, D3D12_HEAP_FLAG_NONE, &staging_desc,
                                             D3D12_RESOURCE_STATE_COPY_DEST, nullptr, __uuidof(ID3D12Resource),
                                             reinterpret_cast<void**>(&readback));
        check(hr == S_OK && readback, "readback buffer hr=0x%08lx", ul(hr));
    }

    uint32_t buf_match = 0;
    uint32_t tex_match = 0;
    uint32_t reverse_match = 0;
    UINT64 buf_gpu = 0;
    int flush_ok = 0;
    const char* texture_path = "not_run";
    HRESULT texture_hr = E_FAIL;
    const UINT64 cpu_va = region_base;
    uint8_t* cpu = map_pointer != 0 ? reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(map_pointer)) : nullptr;
    if (readback && placed_buf && placed_tex && cpu) {
        std::memset(cpu, 0, kUploadBytes);
        write_buffer_pattern(cpu);
        write_texture(cpu + kPlacement);
        uint8_t tex_src[kFootprint];
        write_texture(tex_src);
        texture_hr = placed_tex->WriteToSubresource(0, nullptr, tex_src, kRowPitch, kFootprint);
        texture_path = texture_hr == S_OK ? "WriteToSubresource" : "copy_from_buffer";
        std::printf("texture_path=%s hr=0x%08lx\n", texture_path, ul(texture_hr));
        std::fflush(stdout);

        D3D12_COMMAND_QUEUE_DESC queue_desc{};
        queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        ID3D12CommandQueue* queue = nullptr;
        ID3D12CommandAllocator* allocator = nullptr;
        ID3D12GraphicsCommandList* list = nullptr;
        ID3D12Fence* fence = nullptr;
        hr = device->CreateCommandQueue(&queue_desc, __uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(&queue));
        check(hr == S_OK && queue, "CreateCommandQueue hr=0x%08lx", ul(hr));
        hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator),
                                           reinterpret_cast<void**>(&allocator));
        check(hr == S_OK && allocator, "CreateCommandAllocator hr=0x%08lx", ul(hr));
        if (allocator) {
            hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr,
                                           __uuidof(ID3D12GraphicsCommandList), reinterpret_cast<void**>(&list));
            check(hr == S_OK && list, "CreateCommandList hr=0x%08lx", ul(hr));
        }
        hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), reinterpret_cast<void**>(&fence));
        check(hr == S_OK && fence, "CreateFence hr=0x%08lx", ul(hr));

        ID3D12Resource* upload = nullptr;
        if (list && queue && fence) {
            D3D12_TEXTURE_COPY_LOCATION src{};
            src.pResource = placed_buf;
            src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src.PlacedFootprint.Offset = kPlacement;
            src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            src.PlacedFootprint.Footprint.Width = kTexW;
            src.PlacedFootprint.Footprint.Height = kTexH;
            src.PlacedFootprint.Footprint.Depth = 1;
            src.PlacedFootprint.Footprint.RowPitch = kRowPitch;
            if (texture_hr != S_OK) {
                D3D12_TEXTURE_COPY_LOCATION dst{};
                dst.pResource = placed_tex;
                dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            }
            list->CopyBufferRegion(readback, 0, placed_buf, 0, kBufBytes);

            D3D12_RESOURCE_BARRIER to_src{};
            to_src.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            to_src.Transition.pResource = placed_tex;
            to_src.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            to_src.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            to_src.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            list->ResourceBarrier(1, &to_src);

            D3D12_TEXTURE_COPY_LOCATION back_src{};
            back_src.pResource = placed_tex;
            back_src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            D3D12_TEXTURE_COPY_LOCATION back_dst{};
            back_dst.pResource = readback;
            back_dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            back_dst.PlacedFootprint = src.PlacedFootprint;
            list->CopyTextureRegion(&back_dst, 0, 0, 0, &back_src, nullptr);

            hr = list->Close();
            check(hr == S_OK, "command list Close hr=0x%08lx", ul(hr));
            ID3D12CommandList* lists[] = {list};
            queue->ExecuteCommandLists(1, lists);
            check(wait_fence(queue, fence, 1), "GPU queue must retire the placed-resource copy");

            D3D12_HEAP_PROPERTIES upload_props{};
            upload_props.Type = D3D12_HEAP_TYPE_UPLOAD;
            D3D12_RESOURCE_DESC upload_desc = buffer_desc(kBufBytes);
            hr = device->CreateCommittedResource(&upload_props, D3D12_HEAP_FLAG_NONE, &upload_desc,
                                                 D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, __uuidof(ID3D12Resource),
                                                 reinterpret_cast<void**>(&upload));
            check(hr == S_OK && upload, "reverse upload buffer hr=0x%08lx", ul(hr));
            void* upload_cpu = nullptr;
            if (upload && upload->Map(0, nullptr, &upload_cpu) == S_OK && upload_cpu) {
                write_reverse_pattern(static_cast<uint8_t*>(upload_cpu));
                upload->Unmap(0, nullptr);
            }
            if (upload && allocator->Reset() == S_OK && list->Reset(allocator, nullptr) == S_OK) {
                D3D12_RESOURCE_BARRIER to_dest{};
                to_dest.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                to_dest.Transition.pResource = placed_buf;
                to_dest.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                to_dest.Transition.StateBefore = D3D12_RESOURCE_STATE_GENERIC_READ;
                to_dest.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
                list->ResourceBarrier(1, &to_dest);
                list->CopyBufferRegion(placed_buf, 0, upload, 0, kBufBytes);
                hr = list->Close();
                check(hr == S_OK, "reverse command list Close hr=0x%08lx", ul(hr));
                ID3D12CommandList* reverse_lists[] = {list};
                queue->ExecuteCommandLists(1, reverse_lists);
                check(wait_fence(queue, fence, 2), "GPU queue must retire the reverse copy");
                reverse_match = count_reverse(cpu);
            }
        }
        if (upload) upload->Release();
        if (queue) queue->Release();
        if (allocator) allocator->Release();
        if (list) list->Release();
        if (fence) fence->Release();

        void* back = nullptr;
        D3D12_RANGE read_range = {0, kUploadBytes};
        hr = readback->Map(0, &read_range, &back);
        check(hr == S_OK && back, "Map readback hr=0x%08lx", ul(hr));
        if (back) {
            auto* bytes = static_cast<const uint8_t*>(back);
            buf_match = count_buffer(bytes);
            tex_match = count_texture(bytes + kPlacement);
            D3D12_RANGE empty = {0, 0};
            readback->Unmap(0, &empty);
        }
        check(buf_match == kBufBytes, "readback buffer bytes %u/%u", buf_match, kBufBytes);
        check(tex_match == kFootprint, "readback texture bytes %u/%u", tex_match, kFootprint);
        check(reverse_match == kBufBytes, "CPU readback through the persistent pointer %u/%u", reverse_match, kBufBytes);

        buf_gpu = placed_buf->GetGPUVirtualAddress();
        if (view) {
            const BOOL first = FlushViewOfFile(view, static_cast<SIZE_T>(kAlign));
            const BOOL second = FlushViewOfFile(view + kAlign, static_cast<SIZE_T>(kAlign));
            flush_ok = first && second ? 1 : 0;
            check(flush_ok == 1, "FlushViewOfFile on each xmem page must succeed (err=%lu)",
                  static_cast<unsigned long>(GetLastError()));
        }
    }

    std::printf("RESULT: cpu_va=0x%llx map=0x%llx buffer_gpu_va=0x%llx flush=%d copy_buffer=%u/%u copy_texture=%u/%u "
                "texture_path=%s cpu_after_gpu=%u/%u small_align=%llu upload_100000=%llu\n",
                static_cast<unsigned long long>(cpu_va), static_cast<unsigned long long>(map_pointer),
                static_cast<unsigned long long>(buf_gpu), flush_ok, buf_match, kBufBytes, tex_match, kFootprint,
                texture_path, reverse_match, kBufBytes, static_cast<unsigned long long>(small_align),
                static_cast<unsigned long long>(odd_size));
    std::fflush(stdout);

    if (have_log) {
        char base_needle[64];
        char limit_needle[80];
        std::snprintf(base_needle, sizeof(base_needle), "base=0x%llx", static_cast<unsigned long long>(cpu_va));
        std::snprintf(limit_needle, sizeof(limit_needle), "title_limit=%llu",
                      static_cast<unsigned long long>(cfg->title_memory_limit_bytes));
        check(file_contains(log_path, "\"event\":\"D3D12ConfigureVirtualMemoryX\""),
              "log must record D3D12ConfigureVirtualMemoryX");
        check(file_contains(log_path, profile), "log must name the %s profile", profile);
        check(file_contains(log_path, limit_needle), "log must record the active title limit %s", limit_needle);
        check(mapped && file_contains(log_path, base_needle), "log must record the queried xmem base %s", base_needle);
        check(file_contains(log_path, "d3d12_xs.dll!D3D12ConfigureVirtualMemoryX"),
              "an unrecognised configuration must log the unimplemented-API stub");
    } else {
        check(false, "BX_LOG_FILE must be set");
    }

    if (small_res) small_res->Release();
    if (small_host_res) small_host_res->Release();
    if (small_heap) small_heap->Release();
    if (small_host_heap) small_host_heap->Release();
    if (heap1) heap1->Release();
    if (heap1_host) heap1_host->Release();
    if (heap1_base) heap1_base->Release();
    if (heap1_host_base) heap1_host_base->Release();
    if (align_hook) align_hook->Release();
    if (align_host) align_host->Release();
    if (odd) odd->Release();
    if (odd_host) odd_host->Release();
    if (upload_same) upload_same->Release();
    if (deny_same) deny_same->Release();
    if (upload_host) upload_host->Release();
    if (deny_host) deny_host->Release();
    if (custom_later) custom_later->Release();
    if (custom_a) custom_a->Release();
    if (custom_b) custom_b->Release();
    if (readback) readback->Release();
    if (placed_tex) placed_tex->Release();
    if (placed_buf) placed_buf->Release();

    if (mapped && backing && have_log) {
        check_placed_heap_charge(device, log_path, region_base, cfg->title_memory_limit_bytes);
    }

    if (device) {
        check_graphics_xmem_pairing(device, cfg->title_memory_limit_bytes);
    }

    /*
     * An ordinary heap AddRefs its device, matching BxChildProxy. Releasing the
     * caller's device reference must leave the proxy alive for GetDevice.
     */
    ID3D12Device* side = nullptr;
    hr = D3D12XboxCreateDevice(nullptr, __uuidof(ID3D12Device), reinterpret_cast<void**>(&side));
    check(hr == S_OK && side, "side device for the ordinary-heap lifetime check hr=0x%08lx", ul(hr));
    if (side) {
        D3D12_HEAP_DESC side_desc = heap_of(D3D12_HEAP_TYPE_UPLOAD, kAlign, D3D12_HEAP_FLAG_NONE);
        ID3D12Heap* side_heap = nullptr;
        hr = side->CreateHeap(&side_desc, __uuidof(ID3D12Heap), reinterpret_cast<void**>(&side_heap));
        check(hr == S_OK && side_heap, "ordinary side heap hr=0x%08lx", ul(hr));
        ID3D12Device* side_got = nullptr;
        if (side_heap) {
            const HRESULT gd = side_heap->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&side_got));
            check(gd == S_OK && side_got == side, "ordinary heap GetDevice is its proxy (hr=0x%08lx)", ul(gd));
            const ULONG left = side->Release();
            side = nullptr;
            check(left != 0, "ordinary heap keeps the device alive after the caller releases it (refs=%lu)", left);
            ID3D12Device* still = nullptr;
            const HRESULT again = side_heap->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&still));
            check(again == S_OK && still != nullptr && still == side_got,
                  "ordinary heap GetDevice still returns the live proxy after the caller released it");
            if (still) still->Release();
            side_heap->Release();
            if (side_got) side_got->Release();
        }
        if (side) side->Release();
    }

    /*
     * Caller-held heaps other than the backing heap are released above. g_record
     * still holds that heap, so the charge must be exactly one region. The caller's
     * extra reference does not add a second charge. Releasing device A must drop
     * the proxy to zero. GetDevice on the heap we still hold must then fail closed.
     * Device B is forced onto the same host device, which is still alive because
     * the cached host heap owns it, and must receive that same backing heap.
     *
     * The zero-reference race and the cache hand-out race are closed by g_section.
     * A weak GetDevice never increments a proxy whose count is already zero, and
     * the host check, retarget, and returned reference happen in one critical
     * section. There is no timing stress test for those windows.
     */
    check_newer_placement(device);

    uint64_t host_before = 0;
    uint64_t host_ignored = 0;
    int host_before_count = 0;
    if (backing) {
        BxD3d12VirtualMemoryConfig detached = make_block(0, 0, kFlagBacking);
        const HRESULT away = D3D12ConfigureVirtualMemoryX(&detached);
        check(away == S_OK, "reconfigure drops the cached backing heap hr=0x%08lx", ul(away));
        check(get_device_closed(backing), "GetDevice on a heap detached by reconfigure fails closed");
        const ULONG left = device->Release();
        check(left == 0, "proxy released to zero while the detached heap is still held (refs=%lu)", left);
        check(get_device_closed(backing), "GetDevice on the retained heap stays closed after the proxy is gone");
        /* The device was released. Recreate it and the backing heap for the checks below. */
        device = nullptr;
        backing->Release();
        backing = nullptr;
        hr = D3D12XboxCreateDevice(nullptr, __uuidof(ID3D12Device), reinterpret_cast<void**>(&device));
        check(hr == S_OK && device, "device recreated after the detach check hr=0x%08lx", ul(hr));
        if (device && mapped) {
            BxD3d12VirtualMemoryConfig again = make_block(region_base, kRegionBytes, kFlagBacking);
            check(D3D12ConfigureVirtualMemoryX(&again) == S_OK, "region restored after the detach check");
            D3D12_HEAP_DESC again_desc = backing_desc();
            hr = device->CreateHeap(&again_desc, __uuidof(ID3D12Heap), reinterpret_cast<void**>(&backing));
            check(hr == S_OK && backing, "backing heap restored hr=0x%08lx", ul(hr));
        }
    }
    if (device && backing) {
        check(SetEnvironmentVariableA("BX_D3D12_FORCE_BACKING_REPLACE", "1") != 0,
              "test hook BX_D3D12_FORCE_BACKING_REPLACE=1 is set");
        ID3D12Heap* replaced = nullptr;
        D3D12_HEAP_DESC replace_desc = backing_desc();
        hr = device->CreateHeap(&replace_desc, __uuidof(ID3D12Heap), reinterpret_cast<void**>(&replaced));
        SetEnvironmentVariableA("BX_D3D12_FORCE_BACKING_REPLACE", nullptr);
        check(hr == S_OK && replaced && !same_object(replaced, backing),
              "forced host replacement publishes a different backing heap hr=0x%08lx", ul(hr));
        if (have_log) {
            /* Capture the device whose forced replacement heap remains in the cache. */
            log_u64_ends(log_path, "host_device=", &host_ignored, &host_before, &host_before_count);
        }
        check(get_device_closed(backing), "GetDevice on the heap replaced by another host device fails closed");
        const ULONG left = device->Release();
        check(left == 0, "proxy released to zero while the replaced heap is still held (refs=%lu)", left);
        check(get_device_closed(backing), "GetDevice on the replaced heap stays closed after the proxy is gone");
        backing->Release();
        backing = replaced;
        device = nullptr;
        hr = D3D12XboxCreateDevice(nullptr, __uuidof(ID3D12Device), reinterpret_cast<void**>(&device));
        check(hr == S_OK && device, "device recreated after the replacement check hr=0x%08lx", ul(hr));
        if (device && mapped && backing) {
            /* The replacement already published a heap for this region. A same-host
               create on the new proxy must retarget that heap, which the checks below cover. */
            BxD3d12VirtualMemoryConfig again = make_block(region_base, kRegionBytes, kFlagBacking);
            check(D3D12ConfigureVirtualMemoryX(&again) == S_OK, "region still configured after replacement");
        }
    }
    BxD3d12VirtualMemoryConfig charge_before_block = make_block(region_base, kRegionBytes, kFlagBacking);
    hr = mapped ? D3D12ConfigureVirtualMemoryX(&charge_before_block) : E_FAIL;
    const uint64_t charged_before = have_log ? last_log_u64(log_path, "gpu_heap_bytes=") : ~0ull;
    check(hr == S_OK && charged_before == kRegionBytes,
          "one backing heap remains charged before device recreation (gpu_heap_bytes=%llu)",
          static_cast<unsigned long long>(charged_before));
    if (device) {
        check(device->Release() == 0, "device A proxy released while the backing cache still holds the heap");
        device = nullptr;
    } else {
        check(false, "device A was already gone before the cache-lifetime check");
    }
    if (backing) {
        ID3D12Device* dead = nullptr;
        const HRESULT gd = backing->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&dead));
        check(gd == E_FAIL && dead == nullptr, "GetDevice after proxy A is destroyed returns E_FAIL (hr=0x%08lx)", ul(gd));
        if (dead) dead->Release();
    }

    uint64_t latest_host_before_b = 0;
    if (have_log) {
        log_u64_ends(log_path, "host_device=", &host_ignored, &latest_host_before_b, &host_before_count);
    }
    check(SetEnvironmentVariableA("BX_D3D12_REUSE_BACKING_HOST", "1") != 0,
          "test hook BX_D3D12_REUSE_BACKING_HOST=1 is set");
    ID3D12Device* device_b = nullptr;
    hr = D3D12XboxCreateDevice(nullptr, __uuidof(ID3D12Device), reinterpret_cast<void**>(&device_b));
    SetEnvironmentVariableA("BX_D3D12_REUSE_BACKING_HOST", nullptr);
    check(hr == S_OK && device_b, "device B is a new proxy on the cached host device hr=0x%08lx", ul(hr));
    uint64_t host_after = 0;
    int host_after_count = 0;
    if (have_log) {
        log_u64_ends(log_path, "host_device=", &host_ignored, &host_after, &host_after_count);
    }
    check(have_log && host_after_count == host_before_count + 1 && host_before != 0 && host_before == host_after,
          "device B wraps the host ID3D12Device the cache is holding (before=0x%llx after=0x%llx)",
          static_cast<unsigned long long>(host_before), static_cast<unsigned long long>(host_after));
    ID3D12Heap* backing_b = nullptr;
    if (device_b) {
        D3D12_HEAP_DESC again_desc = backing_desc();
        hr = device_b->CreateHeap(&again_desc, __uuidof(ID3D12Heap), reinterpret_cast<void**>(&backing_b));
        check(hr == S_OK && backing_b, "device B backing CreateHeap hr=0x%08lx", ul(hr));
        check(same_object(backing_b, backing), "device B received the cached backing heap");
    }
    if (device_b && backing_b) {
        check_heap_get_device(backing_b, device_b, host, "device B backing heap");
        ID3D12Resource* placed_b = nullptr;
        D3D12_RESOURCE_DESC placed_desc = buffer_desc(kBufBytes);
        hr = device_b->CreatePlacedResource(backing_b, 0, &placed_desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                            __uuidof(ID3D12Resource), reinterpret_cast<void**>(&placed_b));
        check(hr == S_OK && placed_b, "device B CreatePlacedResource in the configured region hr=0x%08lx", ul(hr));
        if (placed_b) placed_b->Release();
    }
    BxD3d12VirtualMemoryConfig charge_after_block = make_block(region_base, kRegionBytes, kFlagBacking);
    hr = mapped ? D3D12ConfigureVirtualMemoryX(&charge_after_block) : E_FAIL;
    const uint64_t charged_after = have_log ? last_log_u64(log_path, "gpu_heap_bytes=") : ~0ull;
    check(hr == S_OK && charged_after == kRegionBytes && charged_after != kRegionBytes * 2,
          "budget charge after device B is one region, not two (gpu_heap_bytes=%llu)",
          static_cast<unsigned long long>(charged_after));
    if (backing_b) backing_b->Release();
    if (backing) backing->Release();
    if (host) host->Release();
    if (device_b) {
        check(device_b->Release() == 0, "device B proxy released");
    }

    if (view) {
        XMemMapPhysicalPages(view, page_count, 0, nullptr);
    }
    if (pages[0] || pages[1]) {
        SIZE_T free_count = 2;
        XMemFreePhysicalPages(&free_count, pages, 0);
    }
    if (sync.ready) CloseHandle(sync.ready);
    if (sync.done) CloseHandle(sync.done);

    if (g_failures > 0) {
        std::fprintf(stderr, "FAIL: test_d3d12_placed (%d of %d checks failed)\n", g_failures, g_checks);
        return 1;
    }
    std::printf("PASS: test_d3d12_placed (%d checks passed, %d skipped, buffer %u/%u bytes, texture %u/%u bytes, "
                "texture_path=%s, cpu_after_gpu %u/%u, small_align=%llu, upload_100000=%llu)\n",
                g_checks, g_skipped, buf_match, kBufBytes, tex_match, kFootprint, texture_path, reverse_match, kBufBytes,
                static_cast<unsigned long long>(small_align), static_cast<unsigned long long>(odd_size));
    return 0;
}
