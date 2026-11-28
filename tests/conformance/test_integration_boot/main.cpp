/*
 * M1-F1 (#420): end-to-end synthetic mini-engine boot fixture.
 *
 * A clean-room, purpose-built x86-64 PE32+ program that statically imports all
 * 16 BetterXen shim DLLs and walks a console-title-shaped startup path:
 *
 *   loader imports -> runtime config/VEH -> AVX2+FMA -> clwb -> xmem ->
 *   xgameruntime -> PSM app notifications -> ntuser window + message pump ->
 *   XCRDAPI -> AcpHal/xapu/chad -> GameInput (@1) -> xg_xs -> d3d12_xs ->
 *   reverse-order shutdown.
 *
 * Every stage emits one JSON checkpoint line on stdout and one bx_log_emit
 * record (sub="integration_boot") so `bx-cli trace` sees the same sequence.
 *
 * Stage status:
 *   pass    - the contract was exercised and held.
 *   pending - the shim answered with its documented not-implemented signal
 *             (E_NOTIMPL / null object, logged through BX_STUB_LOG), the
 *             fixture cannot yet supply a real descriptor, or the host
 *             answered an import name instead of the shim. Never counted as a
 *             pass; reported separately and turned into a failure by --strict
 *             (or BX_INTEGRATION_STRICT=1) once M1-A..E land.
 *   fail    - anything else, including an unexpected HRESULT, a wrong value,
 *             or an error-severity exception.
 *
 * Function prototypes below mirror the in-tree shim sources and abi/defs; this
 * fixture deliberately invents no console structure layouts. The xg_xs
 * descriptor and vtable are BetterXen's own, from shims/xg_xs/xg_texture.h.
 */
#include <windows.h>
#include <unknwn.h>
#include <d3d12.h>
#include <immintrin.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "bx_config.h"
#include "bx_log.h"
#include "bx_xmem_internal.h"
#include "xg_texture.h"

#define BX_IMPORT __declspec(dllimport)

extern "C" {
/* bx_runtime_veh.dll */
BX_IMPORT int bx_veh_install(void);
BX_IMPORT uint64_t bx_veh_get_clwb_trap_count(void);

/* xmem.dll */
BX_IMPORT PVOID WINAPI XMemAlloc(SIZE_T size, uint64_t attributes);
BX_IMPORT VOID WINAPI XMemFree(PVOID address, uint64_t attributes);
BX_IMPORT BOOL WINAPI XMemTransferMemory(PVOID dst, LPCVOID src, SIZE_T bytes, uint64_t flags);
BX_IMPORT PVOID WINAPI XMemVirtualAlloc(PVOID address, SIZE_T size, DWORD alloc_type, uint64_t xmem_flags,
                                         DWORD protect);
BX_IMPORT SIZE_T WINAPI XMemVirtualQuery(LPCVOID address, PMEMORY_BASIC_INFORMATION buf, SIZE_T len);

/* xgameruntime.dll */
BX_IMPORT HRESULT WINAPI XGameRuntimeInitialize(void);
BX_IMPORT void WINAPI XGameRuntimeUninitialize(void);
BX_IMPORT uint32_t WINAPI XSystemGetDeviceType(void);
BX_IMPORT HRESULT WINAPI XTaskQueueCreate(uint32_t work_mode, uint32_t completion_mode, void** out_queue);
BX_IMPORT void WINAPI XTaskQueueCloseHandle(void* queue);

/* api-ms-win-core-psm-appnotify-l1-1-{0,1}.dll */
BX_IMPORT ULONG WINAPI RegisterAppStateChangeNotification(PVOID callback, PVOID context, PVOID* out_reg);
BX_IMPORT VOID WINAPI UnregisterAppStateChangeNotification(PVOID reg);
BX_IMPORT ULONG WINAPI RegisterAppConstrainedChangeNotification(PVOID callback, PVOID context, PVOID* out_reg);
BX_IMPORT VOID WINAPI UnregisterAppConstrainedChangeNotification(PVOID reg);

/* XCRDAPI.dll */
BX_IMPORT HRESULT WINAPI DStorageGetFactory(REFIID riid, void** ppv);

/* AcpHal.dll */
BX_IMPORT HRESULT WINAPI AcpHalCreate(void** out_hal);
BX_IMPORT HRESULT WINAPI ApuCreateHeap(uint32_t cached_size, uint32_t non_cached_size);
BX_IMPORT HRESULT WINAPI ApuAlloc(void** out_va, uint32_t* out_apu_addr, uint32_t size, uint32_t alignment);
BX_IMPORT HRESULT WINAPI ApuFree(void* va);
BX_IMPORT BOOL WINAPI ApuIsVirtualAddressValid(const void* va, uint32_t size);
BX_IMPORT void* WINAPI ApuMapApuAddress(uint32_t apu_addr);
BX_IMPORT uint32_t WINAPI ApuMapVirtualAddress(const void* va);

/* xapu.dll */
BX_IMPORT HRESULT WINAPI XApuConnect(void** out_conn);
BX_IMPORT HRESULT WINAPI XApuDisconnect(void* conn);
BX_IMPORT uint32_t WINAPI XApuGetResultCount(void* conn);

/* chad.dll */
BX_IMPORT HRESULT WINAPI XDspConnectWithMaximumStreamLimit(uint32_t max_streams, void** out_handle);
BX_IMPORT HRESULT WINAPI XDspActivate(void* handle);
BX_IMPORT HRESULT WINAPI XDspDeactivate(void* handle);
BX_IMPORT HRESULT WINAPI XDspDisconnect(void* handle);

/* GameInput.dll (ordinal 1) */
BX_IMPORT HRESULT WINAPI GameInputCreate(void** out_game_input);

/* xg_xs.dll is declared by "xg_texture.h". */

/* d3d12_xs.dll (D3D12SerializeVersionedRootSignature is declared by <d3d12.h>) */
BX_IMPORT HRESULT WINAPI D3D12XboxCreateDevice(PVOID params, REFIID riid, void** ppv);
} // extern "C"

namespace {

enum class Status { Pass, Pending, Fail };

int g_seq = 0;
int g_pass = 0;
int g_pending = 0;
int g_fail = 0;
const char* volatile g_current_stage = "process_start";
volatile LONG g_error_exceptions = 0;
LPTOP_LEVEL_EXCEPTION_FILTER g_prev_unhandled_filter = nullptr;

const char* status_name(Status s) {
    switch (s) {
        case Status::Pass: return "pass";
        case Status::Pending: return "pending";
        default: return "fail";
    }
}

/* Details are fixture-authored ASCII without quotes or backslashes, so they are safe inside JSON. */
__attribute__((format(gnu_printf, 3, 4)))
void checkpoint(const char* stage, Status status, const char* fmt, ...) {
    char detail[640];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(detail, sizeof(detail), fmt, ap);
    va_end(ap);

    switch (status) {
        case Status::Pass: g_pass++; break;
        case Status::Pending: g_pending++; break;
        default: g_fail++; break;
    }
    g_seq++;
    std::printf("{\"bx_checkpoint\":\"test_integration_boot\",\"seq\":%d,\"stage\":\"%s\",\"status\":\"%s\",\"detail\":\"%s\"}\n",
                g_seq, stage, status_name(status), detail);
    std::fflush(stdout);

    char line[704];
    std::snprintf(line, sizeof(line), "%s: %s", stage, detail);
    const char* event = status == Status::Pass ? "stage_pass"
                      : status == Status::Pending ? "stage_pending" : "stage_fail";
    bx_log_emit(status == Status::Fail ? BX_LOG_ERROR : BX_LOG_INFO, "integration_boot", event, line);
}

void begin_stage(const char* stage) { g_current_stage = stage; }

/* Counts error-severity exceptions that no earlier vectored handler resolved (clwb traps never reach here). */
LONG CALLBACK fault_counter(PEXCEPTION_POINTERS ep) {
    if (ep && ep->ExceptionRecord && (ep->ExceptionRecord->ExceptionCode & 0xC0000000u) == 0xC0000000u) {
        InterlockedIncrement(&g_error_exceptions);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

LONG WINAPI unhandled_fault(PEXCEPTION_POINTERS ep) {
    unsigned long code = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0ul;
    unsigned long long addr = ep && ep->ExceptionRecord
        ? reinterpret_cast<uintptr_t>(ep->ExceptionRecord->ExceptionAddress) : 0ull;
    g_seq++;
    std::printf("{\"bx_checkpoint\":\"test_integration_boot\",\"seq\":%d,\"stage\":\"%s\",\"status\":\"fail\",\"detail\":\"unhandled exception code=0x%08lx address=0x%016llx\"}\n",
                g_seq, g_current_stage, code, addr);
    std::printf("FAIL: test_integration_boot (unhandled fault in stage %s)\n", g_current_stage);
    std::fflush(stdout);
    if (g_prev_unhandled_filter) {
        g_prev_unhandled_filter(ep);
    }
    ExitProcess(3);
    return EXCEPTION_EXECUTE_HANDLER;
}

/* S_OK + object => pass, E_NOTIMPL + null => pending (logged M0 stub), anything else => fail. */
void report_create(const char* stage, const char* api, HRESULT hr, void* obj, bool null_arg_probe) {
    if (hr == S_OK && obj) {
        checkpoint(stage, Status::Pass, "%s returned S_OK with object", api);
    } else if (hr == E_NOTIMPL && !obj) {
        checkpoint(stage, Status::Pending, "%s is an M0 stub (E_NOTIMPL, null object)", api);
    } else if (null_arg_probe && (hr == E_INVALIDARG || hr == E_POINTER) && !obj) {
        checkpoint(stage, Status::Pending,
                   "%s rejected the null descriptor (hr=0x%08lx); fixture needs the real descriptor contract",
                   api, static_cast<unsigned long>(hr));
    } else {
        checkpoint(stage, Status::Fail, "%s returned hr=0x%08lx object=%s", api,
                   static_cast<unsigned long>(hr), obj ? "non-null" : "null");
    }
}

void release_com(void*& obj) {
    if (obj) {
        static_cast<IUnknown*>(obj)->Release();
        obj = nullptr;
    }
}

/* ---- Stage 1: loader imports ------------------------------------------------------------ */

const char* const kShimDlls[] = {
    "bx_runtime_veh.dll",
    "xmem.dll",
    "xgameruntime.dll",
    "XCRDAPI.dll",
    "AcpHal.dll",
    "xapu.dll",
    "chad.dll",
    "GameInput.dll",
    "xg_xs.dll",
    "d3d12_xs.dll",
    "api-ms-win-core-psm-appnotify-l1-1-0.dll",
    "api-ms-win-core-psm-appnotify-l1-1-1.dll",
    "ext-ms-win-rtcore-ntuser-message-l1-1-0.dll",
    "ext-ms-win-rtcore-ntuser-message-ansi-l1-1-0.dll",
    "ext-ms-win-rtcore-ntuser-window-l1-1-0.dll",
    "ext-ms-win-rtcore-ntuser-window-ansi-l1-1-0.dll",
};
constexpr size_t kShimDllCount = sizeof(kShimDlls) / sizeof(kShimDlls[0]);

bool image_statically_imports(const char* dll) {
    auto* base = reinterpret_cast<const uint8_t*>(GetModuleHandleA(nullptr));
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (dir.VirtualAddress == 0) {
        return false;
    }
    auto* desc = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
    for (; desc->Name != 0; ++desc) {
        if (_stricmp(reinterpret_cast<const char*>(base + desc->Name), dll) == 0) {
            return true;
        }
    }
    return false;
}

/* Lower-cased full path of the file backing a loaded module, or false when the module is not loaded. */
bool module_path(const char* dll, char* out, DWORD out_len) {
    HMODULE mod = GetModuleHandleA(dll);
    if (!mod) {
        return false;
    }
    DWORD n = GetModuleFileNameA(mod, out, out_len);
    if (n == 0 || n >= out_len) {
        return false;
    }
    CharLowerBuffA(out, n);
    return true;
}

/*
 * True when `dll` is served by the BetterXen shim file of that name sitting next to bx_runtime_veh.dll,
 * rather than by a host module (for example a Wine built-in reached through API-set redirection).
 */
bool backed_by_shim(const char* dll) {
    char anchor[MAX_PATH];
    char path[MAX_PATH];
    if (!module_path("bx_runtime_veh.dll", anchor, sizeof(anchor)) || !module_path(dll, path, sizeof(path))) {
        return false;
    }
    char* anchor_slash = std::strrchr(anchor, '\\');
    char* path_slash = std::strrchr(path, '\\');
    if (!anchor_slash || !path_slash) {
        return false;
    }
    size_t anchor_dir = static_cast<size_t>(anchor_slash - anchor);
    size_t path_dir = static_cast<size_t>(path_slash - path);
    return anchor_dir == path_dir && std::memcmp(anchor, path, anchor_dir) == 0 && _stricmp(path_slash + 1, dll) == 0;
}

void stage_imports() {
    begin_stage("imports_resolved");
    size_t imported = 0;
    size_t loaded = 0;
    const char* first_missing = nullptr;
    for (const char* dll : kShimDlls) {
        bool in_table = image_statically_imports(dll);
        bool is_loaded = GetModuleHandleA(dll) != nullptr;
        imported += in_table ? 1 : 0;
        loaded += is_loaded ? 1 : 0;
        if ((!in_table || !is_loaded) && !first_missing) {
            first_missing = dll;
        }
    }
    if (imported == kShimDllCount && loaded == kShimDllCount) {
        checkpoint("imports_resolved", Status::Pass, "%zu/%zu shim DLL names in static import directory and resolved by the loader",
                   imported, kShimDllCount);
    } else {
        checkpoint("imports_resolved", Status::Fail, "static=%zu loaded=%zu of %zu first_missing=%s",
                   imported, loaded, kShimDllCount, first_missing ? first_missing : "none");
    }

    /* The loader resolving a name does not prove the BetterXen shim answered it: report who did. */
    begin_stage("shim_backing");
    size_t backed = 0;
    char redirected[512] = "";
    for (const char* dll : kShimDlls) {
        if (backed_by_shim(dll)) {
            backed++;
        } else if (std::strlen(redirected) + std::strlen(dll) + 2 < sizeof(redirected)) {
            if (redirected[0]) std::strcat(redirected, " ");
            std::strcat(redirected, dll);
        }
    }
    if (backed == kShimDllCount) {
        checkpoint("shim_backing", Status::Pass, "%zu/%zu import names are served by BetterXen shim files", backed, kShimDllCount);
    } else {
        checkpoint("shim_backing", Status::Pending, "%zu/%zu served by BetterXen shim files; host answers for: %s",
                   backed, kShimDllCount, redirected);
    }
}

/* ---- Stage 2: runtime config + VEH ------------------------------------------------------ */

const bx_runtime_config_t* stage_runtime_config() {
    begin_stage("runtime_config");
    const bx_runtime_config_t* cfg = bx_get_runtime_config();
    if (!cfg) {
        checkpoint("runtime_config", Status::Fail, "bx_get_runtime_config returned null");
        return nullptr;
    }
    bool profile_ok = cfg->profile == BX_PROFILE_LOCKHART || cfg->profile == BX_PROFILE_ANACONDA;
    int veh = bx_veh_install();
    if (cfg->abi_version == BX_ABI_VERSION && profile_ok && cfg->title_memory_limit_bytes > 0 && veh == 1) {
        checkpoint("runtime_config", Status::Pass, "abi=%u profile=%s title_limit_mb=%llu veh_installed=1",
                   cfg->abi_version, cfg->profile == BX_PROFILE_ANACONDA ? "anaconda" : "lockhart",
                   static_cast<unsigned long long>(cfg->title_memory_limit_bytes >> 20));
    } else {
        checkpoint("runtime_config", Status::Fail, "abi=%u (want %u) profile=%d title_limit=%llu veh_installed=%d",
                   cfg->abi_version, BX_ABI_VERSION, static_cast<int>(cfg->profile),
                   static_cast<unsigned long long>(cfg->title_memory_limit_bytes), veh);
    }
    return cfg;
}

/* ---- Stage 3: AVX2 + FMA ---------------------------------------------------------------- */

/* Only this function is compiled with AVX2/FMA so nothing VEX-encoded runs before the CPUID gate. */
__attribute__((target("avx2,fma"), noinline))
void run_avx2_fma(const float* a, const float* b, float* fma_out, const int32_t* x, const int32_t* y, int32_t* add_out) {
    __m256 va = _mm256_loadu_ps(a);
    __m256 vb = _mm256_loadu_ps(b);
    _mm256_storeu_ps(fma_out, _mm256_fmadd_ps(va, vb, va)); /* a*b + a */
    __m256i vx = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(x));
    __m256i vy = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(y));
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(add_out), _mm256_add_epi32(vx, vy)); /* 256-bit integer = AVX2 */
    _mm256_zeroupper();
}

void stage_avx2_fma() {
    begin_stage("cpu_avx2_fma");
    __builtin_cpu_init();
    if (!__builtin_cpu_supports("avx2") || !__builtin_cpu_supports("fma")) {
        checkpoint("cpu_avx2_fma", Status::Fail,
                   "CPUID does not advertise AVX2+FMA (bx-cli run sets ROSETTA_ADVERTISE_AVX=1)");
        return;
    }
    volatile float seed_a = 3.0f;
    volatile float seed_b = 4.0f;
    volatile int32_t seed_x = 40;
    float a[8], b[8], fma_out[8] = {};
    int32_t x[8], y[8], add_out[8] = {};
    for (int i = 0; i < 8; ++i) {
        a[i] = seed_a + static_cast<float>(i);
        b[i] = seed_b;
        x[i] = seed_x + i;
        y[i] = 2;
    }
    run_avx2_fma(a, b, fma_out, x, y, add_out);
    for (int i = 0; i < 8; ++i) {
        /* (3+i)*4 + (3+i) = 15 + 5i: exactly representable, so FMA and mul+add agree bit-for-bit. */
        float want_f = 15.0f + 5.0f * static_cast<float>(i);
        int32_t want_i = 42 + i;
        if (fma_out[i] != want_f || add_out[i] != want_i) {
            checkpoint("cpu_avx2_fma", Status::Fail, "lane %d fma=%f (want %f) add=%d (want %d)", i,
                       static_cast<double>(fma_out[i]), static_cast<double>(want_f), add_out[i], want_i);
            return;
        }
    }
    checkpoint("cpu_avx2_fma", Status::Pass, "8 lanes of 256-bit fmadd_ps and add_epi32 match expected values");
}

/* ---- Stage 4: clwb ---------------------------------------------------------------------- */

alignas(64) uint8_t g_clwb_buf[8192];

void stage_clwb(const bx_runtime_config_t* cfg) {
    begin_stage("clwb_veh");
    constexpr uint64_t kForms = 3;
    std::memset(g_clwb_buf, 0x42, sizeof(g_clwb_buf));
    uint8_t* p = g_clwb_buf;
    uint64_t before = bx_veh_get_clwb_trap_count();
    __asm__ volatile("clwb (%0)" :: "r"(p) : "memory");     /* [reg]        */
    __asm__ volatile("clwb 64(%0)" :: "r"(p) : "memory");   /* [reg+disp8]  */
    __asm__ volatile("clwb 4096(%0)" :: "r"(p) : "memory"); /* [reg+disp32] */
    uint64_t delta = bx_veh_get_clwb_trap_count() - before;
    bool data_intact = g_clwb_buf[0] == 0x42 && g_clwb_buf[64] == 0x42 && g_clwb_buf[4096] == 0x42;
    unsigned veh_enabled = cfg ? cfg->enable_clwb_veh : 0u;
    if (!data_intact) {
        checkpoint("clwb_veh", Status::Fail, "buffer contents changed across clwb");
    } else if (delta == kForms) {
        checkpoint("clwb_veh", Status::Pass, "%llu/%llu (3 of 6) clwb forms trapped and skipped by bx_runtime_veh (veh_enabled=%u)",
                   static_cast<unsigned long long>(delta), static_cast<unsigned long long>(kForms), veh_enabled);
    } else if (delta == 0) {
        checkpoint("clwb_veh", Status::Pass,
                   "%llu (3 of 6) clwb forms executed without raising an illegal-instruction trap (trap delta 0, veh_enabled=%u)",
                   static_cast<unsigned long long>(kForms), veh_enabled);
    } else {
        checkpoint("clwb_veh", Status::Fail, "trap delta %llu is neither 0 nor %llu",
                   static_cast<unsigned long long>(delta), static_cast<unsigned long long>(kForms));
    }
}

/* ---- Stage 5: xmem ---------------------------------------------------------------------- */

void stage_xmem(const bx_runtime_config_t* cfg) {
    begin_stage("xmem_init");
    constexpr SIZE_T kHeapBytes = 64 * 1024;
    constexpr SIZE_T kVirtualBytes = 1024 * 1024;

    bx_xmem_stats_t before = {};
    bx_xmem_get_internal_stats(&before);

    void* src = XMemAlloc(kHeapBytes, 0);
    void* dst = XMemAlloc(kHeapBytes, 0);
    if (!src || !dst) {
        checkpoint("xmem_init", Status::Fail, "XMemAlloc(%zu) returned null", static_cast<size_t>(kHeapBytes));
        if (src) XMemFree(src, 0);
        if (dst) XMemFree(dst, 0);
        return;
    }
    for (SIZE_T i = 0; i < kHeapBytes; ++i) {
        static_cast<uint8_t*>(src)[i] = static_cast<uint8_t>((i * 31u + 7u) & 0xFFu);
    }
    std::memset(dst, 0, kHeapBytes);
    bool transfer_ok = XMemTransferMemory(dst, src, kHeapBytes, 0) && std::memcmp(src, dst, kHeapBytes) == 0;

    auto* vmem = static_cast<uint8_t*>(XMemVirtualAlloc(nullptr, kVirtualBytes, MEM_COMMIT | MEM_RESERVE, 0x1 /* XMEM_CPU */, PAGE_READWRITE));
    bool virtual_ok = false;
    if (vmem) {
        vmem[0] = 0xA5;
        vmem[kVirtualBytes - 1] = 0x5A;
        MEMORY_BASIC_INFORMATION mbi = {};
        SIZE_T got = XMemVirtualQuery(vmem, &mbi, sizeof(mbi));
        virtual_ok = got == sizeof(mbi) && mbi.State == MEM_COMMIT && mbi.RegionSize >= kVirtualBytes
                  && vmem[0] == 0xA5 && vmem[kVirtualBytes - 1] == 0x5A;
    }

    bx_xmem_stats_t after = {};
    bx_xmem_get_internal_stats(&after);
    uint64_t want_growth = 2ull * kHeapBytes + kVirtualBytes;
    bool limit_ok = cfg && after.title_limit_bytes == cfg->title_memory_limit_bytes && after.title_limit_bytes > 0;
    bool growth_ok = after.committed_virtual_bytes >= before.committed_virtual_bytes + want_growth;
    bool peak_ok = after.peak_committed_bytes >= after.committed_virtual_bytes;

    if (vmem) VirtualFree(vmem, 0, MEM_RELEASE);
    XMemFree(src, 0);
    XMemFree(dst, 0);

    if (transfer_ok && virtual_ok && limit_ok && growth_ok && peak_ok) {
        checkpoint("xmem_init", Status::Pass, "heap transfer, 1 MiB virtual commit and stats ok (limit_mb=%llu committed_delta=%llu)",
                   static_cast<unsigned long long>(after.title_limit_bytes >> 20),
                   static_cast<unsigned long long>(after.committed_virtual_bytes - before.committed_virtual_bytes));
    } else {
        checkpoint("xmem_init", Status::Fail, "transfer=%d virtual=%d limit=%d growth=%d peak=%d",
                   transfer_ok, virtual_ok, limit_ok, growth_ok, peak_ok);
    }
}

/* ---- Stage 6: xgameruntime -------------------------------------------------------------- */

void* stage_xgameruntime(const bx_runtime_config_t* cfg, bool* runtime_initialized) {
    begin_stage("xgameruntime_init");
    HRESULT hr = XGameRuntimeInitialize();
    *runtime_initialized = hr == S_OK;
    if (hr != S_OK) {
        checkpoint("xgameruntime_init", Status::Fail, "XGameRuntimeInitialize hr=0x%08lx", static_cast<unsigned long>(hr));
        return nullptr;
    }
    /* Public GDK XSystemDeviceType: XboxScarlettLockhart = 6, XboxScarlettAnaconda = 7. */
    uint32_t want_device = (cfg && cfg->profile == BX_PROFILE_ANACONDA) ? 7u : 6u;
    uint32_t device = XSystemGetDeviceType();
    void* queue = nullptr;
    /* Public GDK XTaskQueueDispatchMode::Manual = 0 for both ports: no worker threads are requested. */
    HRESULT qhr = XTaskQueueCreate(0u, 0u, &queue);
    if (device == want_device && qhr == S_OK && queue) {
        checkpoint("xgameruntime_init", Status::Pass, "initialized, device_type=%u matches profile, task queue created", device);
    } else {
        checkpoint("xgameruntime_init", Status::Fail, "device_type=%u (want %u) XTaskQueueCreate hr=0x%08lx queue=%s",
                   device, want_device, static_cast<unsigned long>(qhr), queue ? "non-null" : "null");
    }
    return qhr == S_OK ? queue : nullptr;
}

/* ---- Stage 7: PSM app notifications ----------------------------------------------------- */

volatile LONG g_psm_callbacks = 0;
VOID CALLBACK on_app_state_change(BOOLEAN /*quiesced*/, PVOID /*context*/) { InterlockedIncrement(&g_psm_callbacks); }
VOID CALLBACK on_app_constrained_change(BOOLEAN /*constrained*/, PVOID /*context*/) { InterlockedIncrement(&g_psm_callbacks); }

void stage_psm(PVOID* state_reg, PVOID* constrained_reg) {
    begin_stage("psm_appnotify");
    ULONG rc_state = RegisterAppStateChangeNotification(
        reinterpret_cast<PVOID>(reinterpret_cast<uintptr_t>(&on_app_state_change)), nullptr, state_reg);
    ULONG rc_constrained = RegisterAppConstrainedChangeNotification(
        reinterpret_cast<PVOID>(reinterpret_cast<uintptr_t>(&on_app_constrained_change)), nullptr, constrained_reg);
    bool shim_served = backed_by_shim("api-ms-win-core-psm-appnotify-l1-1-0.dll")
                    && backed_by_shim("api-ms-win-core-psm-appnotify-l1-1-1.dll");
    if (rc_state == ERROR_SUCCESS && rc_constrained == ERROR_SUCCESS && *state_reg && *constrained_reg) {
        checkpoint("psm_appnotify", Status::Pass, "state and constrained notifications registered with non-null tokens (shim_served=%d)",
                   shim_served);
    } else if (!shim_served && rc_state == ERROR_CALL_NOT_IMPLEMENTED && rc_constrained == ERROR_CALL_NOT_IMPLEMENTED
               && !*state_reg && !*constrained_reg) {
        /* The PSM shim was never reached: the host resolved the API-set name itself and reports not-implemented. */
        checkpoint("psm_appnotify", Status::Pending,
                   "host API-set implementation answered instead of the BetterXen PSM shim: ERROR_CALL_NOT_IMPLEMENTED, null tokens");
    } else {
        checkpoint("psm_appnotify", Status::Fail, "state rc=%lu token=%s constrained rc=%lu token=%s shim_served=%d",
                   rc_state, *state_reg ? "non-null" : "null", rc_constrained, *constrained_reg ? "non-null" : "null", shim_served);
    }
}

/* ---- Stage 8: ntuser forwarders (hidden window + message pump) -------------------------- */

constexpr UINT kPingMessage = WM_APP + 0x42;
constexpr WPARAM kPingCookie = 0xB0071;
int g_ping_seen = 0;
WPARAM g_ping_cookie = 0;

LRESULT CALLBACK boot_wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == kPingMessage) {
        g_ping_seen++;
        g_ping_cookie = wparam;
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wparam, lparam);
}

struct ForwarderCheck {
    const char* shim;
    const char* symbol;
};
const ForwarderCheck kForwarders[] = {
    {"ext-ms-win-rtcore-ntuser-window-ansi-l1-1-0.dll", "RegisterClassExA"},
    {"ext-ms-win-rtcore-ntuser-window-ansi-l1-1-0.dll", "CreateWindowExA"},
    {"ext-ms-win-rtcore-ntuser-window-ansi-l1-1-0.dll", "DefWindowProcA"},
    {"ext-ms-win-rtcore-ntuser-window-l1-1-0.dll", "ShowWindow"},
    {"ext-ms-win-rtcore-ntuser-message-ansi-l1-1-0.dll", "PeekMessageA"},
    {"ext-ms-win-rtcore-ntuser-message-ansi-l1-1-0.dll", "DispatchMessageA"},
    {"ext-ms-win-rtcore-ntuser-message-l1-1-0.dll", "TranslateMessage"},
};

void stage_ntuser_window() {
    begin_stage("ntuser_window");
    HMODULE user32 = GetModuleHandleA("user32.dll");
    for (const auto& fwd : kForwarders) {
        HMODULE shim = GetModuleHandleA(fwd.shim);
        FARPROC via_shim = shim ? GetProcAddress(shim, fwd.symbol) : nullptr;
        FARPROC direct = user32 ? GetProcAddress(user32, fwd.symbol) : nullptr;
        if (!via_shim || via_shim != direct) {
            checkpoint("ntuser_window", Status::Fail, "%s!%s does not resolve to user32 (shim=%s user32=%s)",
                       fwd.shim, fwd.symbol, via_shim ? "resolved" : "null", direct ? "resolved" : "null");
            return;
        }
    }

    HINSTANCE inst = GetModuleHandleA(nullptr);
    static const char kClass[] = "BxIntegrationBootWindow";
    WNDCLASSEXA wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = boot_wnd_proc;
    wc.hInstance = inst;
    wc.lpszClassName = kClass;
    if (!RegisterClassExA(&wc)) {
        checkpoint("ntuser_window", Status::Fail, "RegisterClassExA failed err=%lu", GetLastError());
        return;
    }
    HWND hwnd = CreateWindowExA(0, kClass, "bx-integration-boot", WS_OVERLAPPED, 0, 0, 64, 64,
                                nullptr, nullptr, inst, nullptr);
    if (!hwnd) {
        checkpoint("ntuser_window", Status::Fail, "CreateWindowExA failed err=%lu", GetLastError());
        UnregisterClassA(kClass, inst);
        return;
    }
    BOOL was_visible = ShowWindow(hwnd, SW_HIDE);
    BOOL posted = PostMessageA(hwnd, kPingMessage, kPingCookie, 0);
    int dispatched = 0;
    for (int spin = 0; spin < 400 && g_ping_seen == 0; ++spin) {
        MSG msg;
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
            dispatched++;
        }
        if (g_ping_seen == 0) {
            Sleep(5);
        }
    }
    BOOL destroyed = DestroyWindow(hwnd);
    BOOL unregistered = UnregisterClassA(kClass, inst);
    if (!was_visible && posted && g_ping_seen == 1 && g_ping_cookie == kPingCookie && destroyed && unregistered) {
        checkpoint("ntuser_window", Status::Pass,
                   "7 forwarders resolve to user32; hidden window pumped %d message(s) and received the ping", dispatched);
    } else {
        checkpoint("ntuser_window", Status::Fail, "was_visible=%d posted=%d ping_seen=%d cookie_ok=%d destroyed=%d unregistered=%d",
                   was_visible, posted, g_ping_seen, g_ping_cookie == kPingCookie, destroyed, unregistered);
    }
}

/* ---- Stage 10: AcpHal heap ---------------------------------------------------------------- */

void stage_acphal_heap() {
    begin_stage("acphal_heap");
    constexpr uint32_t kSize = 4096;
    constexpr uint32_t kAlign = 64;
    HRESULT heap_hr = ApuCreateHeap(1u << 20, 1u << 20);
    void* va = nullptr;
    uint32_t apu = 0;
    HRESULT alloc_hr = ApuAlloc(&va, &apu, kSize, kAlign);
    if (heap_hr != S_OK || alloc_hr != S_OK || !va) {
        checkpoint("acphal_heap", Status::Fail, "ApuCreateHeap hr=0x%08lx ApuAlloc hr=0x%08lx va=%s",
                   static_cast<unsigned long>(heap_hr), static_cast<unsigned long>(alloc_hr), va ? "non-null" : "null");
        return;
    }
    std::memset(va, 0x3C, kSize);
    bool aligned = (reinterpret_cast<uintptr_t>(va) % kAlign) == 0;
    bool valid = ApuIsVirtualAddressValid(va, kSize) != FALSE;
    bool va_to_apu = ApuMapVirtualAddress(va) == apu;
    void* back = ApuMapApuAddress(apu);
    HRESULT free_hr = ApuFree(va);
    if (aligned && valid && va_to_apu && free_hr == S_OK) {
        checkpoint("acphal_heap", Status::Pass, "ApuCreateHeap/ApuAlloc/ApuMapVirtualAddress/ApuFree round-trip ok");
    } else {
        checkpoint("acphal_heap", Status::Fail, "aligned=%d valid=%d va_to_apu=%d free_hr=0x%08lx",
                   aligned, valid, va_to_apu, static_cast<unsigned long>(free_hr));
    }

    begin_stage("acphal_apu_to_va");
    if (back == va) {
        checkpoint("acphal_apu_to_va", Status::Pass, "ApuMapApuAddress maps the APU address back to its virtual address");
    } else if (back == nullptr) {
        checkpoint("acphal_apu_to_va", Status::Pending, "ApuMapApuAddress is an M0 stub (returned null)");
    } else {
        checkpoint("acphal_apu_to_va", Status::Fail, "ApuMapApuAddress returned a different non-null address");
    }
}

/* ---- Stage 13: xg_xs ---------------------------------------------------------------------- */

/*
 * 100x50 R8G8B8A8 with two mips, so both public D3D12 alignments matter. By hand:
 *   mip 0: 100 * 4 = 400 bytes per row, pitch 512 (256-byte pitch alignment);
 *          ends at 49 * 512 + 400 = 25488.
 *   mip 1: starts at the next 512 boundary, 25600; 50 * 4 = 200 bytes per row,
 *          pitch 256; ends at 25600 + 24 * 256 + 200 = 31944.
 *   total: 31944 rounded up to 512 = 32256.
 */
void* stage_xg_texture_computer() {
    begin_stage("xg_xs_texture_computer");
    bx_xg_resource_desc desc = {};
    desc.dimension = BX_XG_DIMENSION_TEXTURE2D;
    desc.width = 100;
    desc.height = 50;
    desc.depth_or_array_size = 1;
    desc.mip_levels = 2;
    desc.format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.sample_desc.count = 1;
    desc.layout = BX_XG_LAYOUT_LINEAR;

    void* object = reinterpret_cast<void*>(static_cast<uintptr_t>(0xDEAD));
    HRESULT hr = XGCreateTextureComputer(&desc, &object);
    if (hr != S_OK || !object || object == reinterpret_cast<void*>(static_cast<uintptr_t>(0xDEAD))) {
        checkpoint("xg_xs_texture_computer", Status::Fail, "XGCreateTextureComputer hr=0x%08lx object=%p",
                   static_cast<unsigned long>(hr), object);
        return nullptr;
    }

    auto* computer = static_cast<bx_xg_texture_computer*>(object);
    bx_xg_placed_footprint mips[2] = {};
    uint32_t rows[2] = {};
    uint64_t row_bytes[2] = {};
    uint64_t total = 0;
    hr = computer->lpVtbl->GetResourceLayout(computer, 0, 2, 0, mips, rows, row_bytes, &total);
    auto shape_is = [](const bx_xg_subresource_footprint& f, uint32_t width, uint32_t height, uint32_t pitch) {
        return f.format == DXGI_FORMAT_R8G8B8A8_UNORM && f.width == width && f.height == height && f.depth == 1 &&
               f.row_pitch == pitch;
    };
    const bool mip0 = mips[0].offset == 0 && shape_is(mips[0].footprint, 100, 50, 512) && mips[0].copy_end == 25488 &&
                      rows[0] == 50 && row_bytes[0] == 400;
    const bool mip1 = mips[1].offset == 25600 && shape_is(mips[1].footprint, 50, 25, 256) && mips[1].copy_end == 31944 &&
                      rows[1] == 25 && row_bytes[1] == 200;
    if (hr == S_OK && mip0 && mip1 && total == 32256) {
        checkpoint("xg_xs_texture_computer", Status::Pass,
                   "100x50 RGBA8 two-mip linear footprints and 32256 total bytes match hand-computed values");
    } else {
        checkpoint("xg_xs_texture_computer", Status::Fail,
                   "GetResourceLayout hr=0x%08lx mip0_ok=%d mip1_ok=%d total=%llu",
                   static_cast<unsigned long>(hr), mip0, mip1, static_cast<unsigned long long>(total));
    }
    return object;
}

void* stage_xg_xs() {
    begin_stage("xg_xs_swizzle_mode");
    /* D3D12_RESOURCE_DIMENSION_TEXTURE2D / DXGI_FORMAT_R8G8B8A8_UNORM, 256x256, 1 mip, 1 sample. */
    uint32_t mode_a = XGSuggestSwizzleMode(D3D12_RESOURCE_DIMENSION_TEXTURE2D, DXGI_FORMAT_R8G8B8A8_UNORM, 256, 256, 1, 1, 1, 0);
    uint32_t mode_b = XGSuggestSwizzleMode(D3D12_RESOURCE_DIMENSION_TEXTURE2D, DXGI_FORMAT_R8G8B8A8_UNORM, 256, 256, 1, 1, 1, 0);
    if (mode_a == BX_XG_LAYOUT_LINEAR && mode_b == mode_a) {
        checkpoint("xg_xs_swizzle_mode", Status::Pass, "XGSuggestSwizzleMode suggests linear and is deterministic (mode=%u)", mode_a);
    } else {
        checkpoint("xg_xs_swizzle_mode", Status::Fail, "XGSuggestSwizzleMode returned %u then %u, expected linear", mode_a, mode_b);
    }

    return stage_xg_texture_computer();
}

/* ---- Stage 14: d3d12_xs ------------------------------------------------------------------- */

void stage_d3d12_root_signature() {
    begin_stage("d3d12_root_signature");
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc = {};
    desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_0; /* empty v1.0 root signature: no parameters, no samplers */
    ID3DBlob* blob = nullptr;
    ID3DBlob* error = nullptr;
    HRESULT hr = D3D12SerializeVersionedRootSignature(&desc, &blob, &error);
    if (hr == S_OK && blob) {
        const void* bytes = blob->GetBufferPointer();
        SIZE_T size = blob->GetBufferSize();
        bool dxbc = bytes && size >= 4 && std::memcmp(bytes, "DXBC", 4) == 0;
        if (dxbc) {
            checkpoint("d3d12_root_signature", Status::Pass,
                       "empty v1.0 root signature serialized through host d3d12.dll (%llu bytes, DXBC container)",
                       static_cast<unsigned long long>(size));
        } else {
            checkpoint("d3d12_root_signature", Status::Fail, "serialized blob is not a DXBC container (%llu bytes)",
                       static_cast<unsigned long long>(size));
        }
    } else if (hr == E_NOTIMPL && !blob) {
        checkpoint("d3d12_root_signature", Status::Pending,
                   "host d3d12.dll serializer unavailable; shim logged the stub and returned E_NOTIMPL");
    } else {
        checkpoint("d3d12_root_signature", Status::Fail, "D3D12SerializeVersionedRootSignature hr=0x%08lx blob=%s",
                   static_cast<unsigned long>(hr), blob ? "non-null" : "null");
    }
    if (blob) blob->Release();
    if (error) error->Release();
}

void* stage_d3d12_create_device() {
    begin_stage("d3d12xbox_create_device");
    void* device = reinterpret_cast<void*>(static_cast<uintptr_t>(0xDEAD));
    HRESULT hr = D3D12XboxCreateDevice(nullptr, __uuidof(ID3D12Device), &device);
    if (device == reinterpret_cast<void*>(static_cast<uintptr_t>(0xDEAD))) {
        checkpoint("d3d12xbox_create_device", Status::Fail, "D3D12XboxCreateDevice left the out pointer unwritten (hr=0x%08lx)",
                   static_cast<unsigned long>(hr));
        return nullptr;
    }
    report_create("d3d12xbox_create_device", "D3D12XboxCreateDevice", hr, device, true);
    return hr == S_OK ? device : nullptr;
}

bool strict_requested(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--strict") == 0) {
            return true;
        }
    }
    char buf[8];
    DWORD n = GetEnvironmentVariableA("BX_INTEGRATION_STRICT", buf, sizeof(buf));
    return n == 1 && buf[0] == '1';
}

} // namespace

int main(int argc, char** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    g_prev_unhandled_filter = SetUnhandledExceptionFilter(unhandled_fault);
    PVOID fault_handle = AddVectoredExceptionHandler(0, fault_counter);
    const bool strict = strict_requested(argc, argv);

    bx_log_emit(BX_LOG_INFO, "integration_boot", "boot_begin", strict ? "strict=1" : "strict=0");

    /* --- Startup ---------------------------------------------------------- */
    stage_imports();
    const bx_runtime_config_t* cfg = stage_runtime_config();
    stage_avx2_fma();
    stage_clwb(cfg);
    stage_xmem(cfg);

    bool runtime_initialized = false;
    void* task_queue = stage_xgameruntime(cfg, &runtime_initialized);

    PVOID psm_state_reg = nullptr;
    PVOID psm_constrained_reg = nullptr;
    stage_psm(&psm_state_reg, &psm_constrained_reg);

    stage_ntuser_window();

    begin_stage("xcrdapi_factory");
    void* dstorage_factory = reinterpret_cast<void*>(static_cast<uintptr_t>(0xDEAD));
    HRESULT factory_hr = DStorageGetFactory(__uuidof(IUnknown), &dstorage_factory);
    if (dstorage_factory == reinterpret_cast<void*>(static_cast<uintptr_t>(0xDEAD))) {
        checkpoint("xcrdapi_factory", Status::Fail, "DStorageGetFactory left the out pointer unwritten");
        dstorage_factory = nullptr;
    } else {
        report_create("xcrdapi_factory", "DStorageGetFactory", factory_hr, dstorage_factory, false);
        if (factory_hr != S_OK) dstorage_factory = nullptr;
    }

    begin_stage("acphal_create");
    void* acp_hal = reinterpret_cast<void*>(static_cast<uintptr_t>(0xDEAD));
    HRESULT hal_hr = AcpHalCreate(&acp_hal);
    if (acp_hal == reinterpret_cast<void*>(static_cast<uintptr_t>(0xDEAD))) {
        checkpoint("acphal_create", Status::Fail, "AcpHalCreate left the out pointer unwritten");
        acp_hal = nullptr;
    } else {
        report_create("acphal_create", "AcpHalCreate", hal_hr, acp_hal, false);
        if (hal_hr != S_OK) acp_hal = nullptr;
    }
    stage_acphal_heap();

    begin_stage("xapu_connect");
    void* apu_conn = nullptr;
    HRESULT apu_hr = XApuConnect(&apu_conn);
    uint32_t apu_results = (apu_hr == S_OK && apu_conn) ? XApuGetResultCount(apu_conn) : 0u;
    if (apu_hr == S_OK && apu_conn && apu_results == 0u) {
        checkpoint("xapu_connect", Status::Pass, "connected; fresh connection reports 0 queued results");
    } else {
        checkpoint("xapu_connect", Status::Fail, "XApuConnect hr=0x%08lx conn=%s result_count=%u",
                   static_cast<unsigned long>(apu_hr), apu_conn ? "non-null" : "null", apu_results);
    }

    begin_stage("chad_connect");
    void* dsp = nullptr;
    HRESULT dsp_hr = XDspConnectWithMaximumStreamLimit(8u, &dsp);
    HRESULT dsp_activate_hr = (dsp_hr == S_OK && dsp) ? XDspActivate(dsp) : E_FAIL;
    bool dsp_active = dsp_activate_hr == S_OK;
    if (dsp_hr == S_OK && dsp && dsp_active) {
        checkpoint("chad_connect", Status::Pass, "DSP connection opened (max 8 streams) and activated");
    } else {
        checkpoint("chad_connect", Status::Fail, "XDspConnectWithMaximumStreamLimit hr=0x%08lx handle=%s XDspActivate hr=0x%08lx",
                   static_cast<unsigned long>(dsp_hr), dsp ? "non-null" : "null", static_cast<unsigned long>(dsp_activate_hr));
    }

    begin_stage("gameinput_create");
    HMODULE gameinput_dll = GetModuleHandleA("GameInput.dll");
    FARPROC by_ordinal = gameinput_dll ? GetProcAddress(gameinput_dll, MAKEINTRESOURCEA(1)) : nullptr;
    void* game_input = reinterpret_cast<void*>(static_cast<uintptr_t>(0xDEAD));
    HRESULT gi_hr = GameInputCreate(&game_input);
    if (!by_ordinal || reinterpret_cast<void*>(by_ordinal) != reinterpret_cast<void*>(&GameInputCreate)) {
        checkpoint("gameinput_create", Status::Fail, "GameInput.dll ordinal 1 does not match the imported GameInputCreate");
        if (gi_hr != S_OK) game_input = nullptr;
    } else if (game_input == reinterpret_cast<void*>(static_cast<uintptr_t>(0xDEAD))) {
        checkpoint("gameinput_create", Status::Fail, "GameInputCreate left the out pointer unwritten");
        game_input = nullptr;
    } else {
        report_create("gameinput_create", "GameInputCreate (ordinal 1 verified)", gi_hr, game_input, false);
        if (gi_hr != S_OK) game_input = nullptr;
    }

    void* texture_computer = stage_xg_xs();
    stage_d3d12_root_signature();
    void* device = stage_d3d12_create_device();

    /* --- Shutdown (reverse order) ----------------------------------------- */
    begin_stage("shutdown");
    release_com(device);
    /* The fixture holds the only reference, so this Release must return 0. */
    ULONG texture_refs = 0;
    if (texture_computer) {
        auto* computer = static_cast<bx_xg_texture_computer*>(texture_computer);
        texture_refs = computer->lpVtbl->Release(computer);
    }
    release_com(game_input);
    bool dsp_closed = true;
    if (dsp) {
        HRESULT deact_hr = dsp_active ? XDspDeactivate(dsp) : S_OK;
        dsp_closed = deact_hr == S_OK && XDspDisconnect(dsp) == S_OK;
    }
    bool apu_closed = apu_conn ? XApuDisconnect(apu_conn) == S_OK : true;
    release_com(acp_hal);
    release_com(dstorage_factory);
    if (psm_constrained_reg) UnregisterAppConstrainedChangeNotification(psm_constrained_reg);
    if (psm_state_reg) UnregisterAppStateChangeNotification(psm_state_reg);
    if (task_queue) XTaskQueueCloseHandle(task_queue);
    if (runtime_initialized) XGameRuntimeUninitialize();

    LONG faults = InterlockedCompareExchange(&g_error_exceptions, 0, 0);
    if (dsp_closed && apu_closed && texture_refs == 0 && faults == 0) {
        checkpoint("shutdown", Status::Pass, "reverse-order teardown completed with 0 error-severity exceptions");
    } else {
        checkpoint("shutdown", Status::Fail, "dsp_closed=%d apu_closed=%d texture_refs=%lu error_exceptions=%ld",
                   dsp_closed, apu_closed, texture_refs, faults);
    }
    if (fault_handle) RemoveVectoredExceptionHandler(fault_handle);
    SetUnhandledExceptionFilter(g_prev_unhandled_filter);

    /* --- Summary ---------------------------------------------------------- */
    const bool ok = g_fail == 0 && !(strict && g_pending > 0);
    char summary[160];
    std::snprintf(summary, sizeof(summary), "stages=%d pass=%d pending=%d fail=%d strict=%d result=%s",
                  g_pass + g_pending + g_fail, g_pass, g_pending, g_fail, strict ? 1 : 0, ok ? "pass" : "fail");
    bx_log_emit(ok ? BX_LOG_INFO : BX_LOG_ERROR, "integration_boot", "boot_summary", summary);
    std::printf("{\"bx_summary\":\"test_integration_boot\",\"stages\":%d,\"pass\":%d,\"pending\":%d,\"fail\":%d,\"strict\":%d,\"result\":\"%s\"}\n",
                g_pass + g_pending + g_fail, g_pass, g_pending, g_fail, strict ? 1 : 0, ok ? "pass" : "fail");
    if (ok) {
        std::printf("PASS: test_integration_boot (%d passed, %d pending, 0 failed)\n", g_pass, g_pending);
    } else {
        std::printf("FAIL: test_integration_boot (%d passed, %d pending, %d failed, strict=%d)\n",
                    g_pass, g_pending, g_fail, strict ? 1 : 0);
    }
    std::fflush(stdout);
    return ok ? 0 : 1;
}
