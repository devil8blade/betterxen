/*
 * M2-C2 (#465): clean-room synthetic conformance test for d3d12_xs.dll's presentation bridge.
 *
 * Renders FRAMES frames into the bridge's back buffers (a clear, then a shader draw limited by a
 * scissor rectangle), presents each one through the bridge, and compares every pixel of every
 * frame, read back from the back buffer before it was presented, with values computed on the CPU
 * from the pixel coordinates and the frame number alone.
 *
 * It also injects three faults from the test side: a host Present that cannot finish until a
 * second caller returns, a fence signal that fails after a successful host Present, and a fence
 * SetEventOnCompletion failure on both sides of host Present.
 *
 * What this shows: the content handed to Present. It does not capture the window, so it does not
 * show what reached the screen. The bridge interfaces are BetterXen's own (presentx.h); nothing
 * here is evidence about the console's PresentX or about commercial software.
 *
 * Exit codes: 0 pass; otherwise the code of the first stage that failed (see Stage).
 * Negative controls, each of which must fail in the named stage. Pass --inject <name>, or, because
 * `bx-cli run` forwards no guest arguments, set BX_PRESENTX_INJECT=<name> and add --inherit-env:
 *   --inject bad-window            the main swapchain gets a destroyed window      -> create  (12)
 *   --inject skip-draw             the shader draw is left out of one frame        -> pixels  (15)
 *   --inject wrong-expectation     the CPU expects a blue value that is off by 1   -> pixels  (15)
 *   --inject skip-wait-fault       omits the post-present SetEventOnCompletion fault -> sync  (16)
 *   --inject wrong-removed-hr      post-present device removal injects E_OUTOFMEMORY -> sync  (16)
 *   --inject zero-timeout-overrun  WaitForIdle(0) budget test passes 5000 ms       -> sync  (16)
 * run_controls.sh runs all six and checks the failing stage and log records of each.
 */
#ifndef WIDL_EXPLICIT_AGGREGATE_RETURNS
#define WIDL_EXPLICIT_AGGREGATE_RETURNS
#endif
#include <windows.h>
#include <unknwn.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "bx_log.h"
#include "presentx.h"

extern "C" __declspec(dllimport) HRESULT WINAPI D3D12XboxCreateDevice(PVOID params, REFIID riid, void** ppv);

namespace {

constexpr UINT kWidth = 100;  /* 400-byte rows, so the 256-byte copy pitch has padding */
constexpr UINT kHeight = 60;
constexpr UINT kBuffers = 3;
constexpr UINT kFrames = 12;  /* every back buffer is reused several times */
constexpr UINT kSkippedFrame = 5;
constexpr D3D12_RECT kRect = {7, 5, 71, 43}; /* asymmetric, so flips and transposes show */
constexpr UINT kRectPixels = (71 - 7) * (43 - 5);
constexpr HRESULT kTimedOut = static_cast<HRESULT>(0x800705B4u);

enum Stage { kSetup = 10, kContract = 11, kCreate = 12, kPipeline = 13, kRender = 14, kPixels = 15, kSync = 16, kLifetime = 17 };

const char* stage_name(Stage s) {
    switch (s) {
    case kSetup: return "setup";
    case kContract: return "contract";
    case kCreate: return "create";
    case kPipeline: return "pipeline";
    case kRender: return "render";
    case kPixels: return "pixels";
    case kSync: return "sync";
    case kLifetime: return "lifetime";
    }
    return "?";
}

Stage g_stage = kSetup;
int g_first_failed_stage = 0;
int g_checks = 0;
int g_failures = 0;
int g_skipped = 0;
LONGLONG g_log_start = 0; /* log records before this offset belong to earlier runs */

__attribute__((format(gnu_printf, 2, 3)))
bool check(bool ok, const char* fmt, ...) {
    g_checks++;
    if (!ok) {
        g_failures++;
        if (!g_first_failed_stage) g_first_failed_stage = g_stage;
        char msg[320];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(msg, sizeof(msg), fmt, ap);
        va_end(ap);
        std::printf("FAIL [%s]: %s\n", stage_name(g_stage), msg);
    }
    return ok;
}

/* For failures that make later stages meaningless. The process exits; nothing is unwound. */
#define REQUIRE(...)                  \
    do {                              \
        if (!check(__VA_ARGS__)) {    \
            finish();                 \
        }                             \
    } while (0)

[[noreturn]] void finish() {
    if (g_failures) {
        std::printf("FAIL: test_d3d12_presentx (%d of %d checks failed; first failing stage: %s)\n", g_failures, g_checks,
                    stage_name(static_cast<Stage>(g_first_failed_stage)));
    } else {
        std::printf("PASS: test_d3d12_presentx (%d checks passed, %d skipped)\n", g_checks, g_skipped);
    }
    std::fflush(stdout);
    ExitProcess(static_cast<UINT>(g_first_failed_stage));
}

unsigned long ul(HRESULT hr) { return static_cast<unsigned long>(hr); }

template <typename T>
void release(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

/* The reference count an object reports to its holder, without changing it. */
ULONG ref_count(IUnknown* obj) {
    obj->AddRef();
    return obj->Release();
}

bool vtable_in_shim(IUnknown* obj) {
    HMODULE owner = nullptr;
    void* vtable = *reinterpret_cast<void**>(obj);
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCSTR>(vtable), &owner)) {
        return false;
    }
    return owner != nullptr && owner == GetModuleHandleA("d3d12_xs.dll");
}

void pump(DWORD ms) {
    const ULONGLONG start = GetTickCount64();
    do {
        MSG msg;
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        Sleep(5);
    } while (GetTickCount64() - start < ms);
}

HWND make_window() {
    static bool registered = false;
    if (!registered) {
        WNDCLASSA wc = {};
        wc.lpfnWndProc = DefWindowProcA;
        wc.hInstance = GetModuleHandleA(nullptr);
        wc.lpszClassName = "BxPresentXTest";
        registered = RegisterClassA(&wc) != 0;
    }
    HWND w = CreateWindowA("BxPresentXTest", "BetterXen PresentX test", WS_POPUP, 40, 40, kWidth * 2, kHeight * 2, nullptr,
                           nullptr, GetModuleHandleA(nullptr), nullptr);
    if (w) {
        ShowWindow(w, SW_SHOWNOACTIVATE);
        pump(10);
    }
    return w;
}

template <typename Fn>
Fn proc(const char* dll, const char* name) {
    HMODULE m = LoadLibraryA(dll);
    return m ? reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(m, name))) : nullptr;
}

bool wait_fence(ID3D12Fence* fence, UINT64 value, HANDLE event) {
    if (fence->GetCompletedValue() < value) {
        if (FAILED(fence->SetEventOnCompletion(value, event))) return false;
        if (WaitForSingleObject(event, 5000) != WAIT_OBJECT_0) return false;
    }
    const UINT64 done = fence->GetCompletedValue();
    return done >= value && done != UINT64_MAX;
}

const BxPresentDesc kDesc = {kWidth, kHeight, DXGI_FORMAT_R8G8B8A8_UNORM, kBuffers};

/* ---- Expected image: from the coordinates and the frame number only ------------------------- */

struct Rgba {
    uint8_t r, g, b, a;
};

Rgba clear_colour(UINT frame) { return {static_cast<uint8_t>(10 + frame), 200, 30, 255}; }
uint32_t pattern_blue(UINT frame) { return 100 + 8 * frame; }

Rgba expected_pixel(UINT x, UINT y, UINT frame, int blue_bias) {
    const bool inside = static_cast<LONG>(x) >= kRect.left && static_cast<LONG>(x) < kRect.right &&
                        static_cast<LONG>(y) >= kRect.top && static_cast<LONG>(y) < kRect.bottom;
    if (!inside) return clear_colour(frame);
    return {static_cast<uint8_t>(x), static_cast<uint8_t>(y),
            static_cast<uint8_t>(static_cast<int>(pattern_blue(frame)) + blue_bias), 255};
}

UINT count_mismatches(const uint8_t* data, UINT row_pitch, UINT frame, int blue_bias, bool report) {
    UINT bad = 0;
    for (UINT y = 0; y < kHeight; ++y) {
        for (UINT x = 0; x < kWidth; ++x) {
            const uint8_t* p = data + static_cast<size_t>(y) * row_pitch + x * 4;
            const Rgba e = expected_pixel(x, y, frame, blue_bias);
            if (p[0] != e.r || p[1] != e.g || p[2] != e.b || p[3] != e.a) {
                if (report && bad == 0) {
                    std::printf("  frame %u first mismatch at (%u,%u): got %u,%u,%u,%u expected %u,%u,%u,%u\n", frame, x, y,
                                p[0], p[1], p[2], p[3], e.r, e.g, e.b, e.a);
                }
                ++bad;
            }
        }
    }
    return bad;
}

const char kShader[] =
    "cbuffer C : register(b0) { uint blue; };\n"
    "float4 vs(uint id : SV_VertexID) : SV_Position {\n"
    "  float2 p = float2((id << 1) & 2, id & 2);\n"
    "  return float4(p * float2(2, -2) + float2(-1, 1), 0, 1);\n"
    "}\n"
    "float4 ps(float4 pos : SV_Position) : SV_Target {\n"
    "  uint2 xy = (uint2)pos.xy;\n"
    "  return float4((xy.x & 255) / 255.0, (xy.y & 255) / 255.0, blue / 255.0, 1.0);\n"
    "}\n";

ID3DBlob* compile(const char* entry, const char* target) {
    auto fn = proc<pD3DCompile>("d3dcompiler_47.dll", "D3DCompile");
    if (!check(fn != nullptr, "d3dcompiler_47.dll!D3DCompile not available")) return nullptr;
    ID3DBlob* code = nullptr;
    ID3DBlob* errors = nullptr;
    HRESULT hr = fn(kShader, sizeof(kShader) - 1, "presentx", nullptr, nullptr, entry, target, 0, 0, &code, &errors);
    check(SUCCEEDED(hr) && code, "D3DCompile(%s) hr=0x%08lx %s", entry, ul(hr),
          errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
    release(errors);
    return code;
}

/* ---- Log evidence --------------------------------------------------------------------------- */

bool log_path(char (&path)[MAX_PATH]) {
    DWORD len = GetEnvironmentVariableA("BX_LOG_FILE", path, sizeof(path));
    return len > 0 && len < sizeof(path);
}

LONGLONG log_size() {
    char path[MAX_PATH];
    WIN32_FILE_ATTRIBUTE_DATA info = {};
    if (!log_path(path) || !GetFileAttributesExA(path, GetFileExInfoStandard, &info)) return 0;
    return (static_cast<LONGLONG>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
}

bool log_has(const char* event, const char* needle) {
    char path[MAX_PATH];
    if (!log_path(path)) return false;
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    LARGE_INTEGER size = {};
    if (GetFileSizeEx(f, &size) && size.QuadPart > g_log_start && size.QuadPart < (64ll << 20)) {
        auto* buf = static_cast<char*>(HeapAlloc(GetProcessHeap(), 0, static_cast<SIZE_T>(size.QuadPart) + 1));
        DWORD got = 0;
        if (buf && ReadFile(f, buf, static_cast<DWORD>(size.QuadPart), &got, nullptr)) {
            buf[got] = '\0';
            char event_key[96];
            std::snprintf(event_key, sizeof(event_key), "\"event\":\"%s\"", event);
            for (char* line = buf + g_log_start; line && *line && !found;) {
                char* end = std::strchr(line, '\n');
                if (end) *end = '\0';
                found = std::strstr(line, event_key) && std::strstr(line, needle);
                line = end ? end + 1 : nullptr;
            }
        }
        if (buf) HeapFree(GetProcessHeap(), 0, buf);
    }
    CloseHandle(f);
    return found;
}

/* Records of `event` containing `needle` written by this run. Zero if there is no log. */
UINT log_count(const char* event, const char* needle) {
    char path[MAX_PATH];
    if (!log_path(path)) return 0;
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return 0;
    UINT count = 0;
    LARGE_INTEGER size = {};
    if (GetFileSizeEx(f, &size) && size.QuadPart > g_log_start && size.QuadPart < (64ll << 20)) {
        auto* buf = static_cast<char*>(HeapAlloc(GetProcessHeap(), 0, static_cast<SIZE_T>(size.QuadPart) + 1));
        DWORD got = 0;
        if (buf && ReadFile(f, buf, static_cast<DWORD>(size.QuadPart), &got, nullptr)) {
            buf[got] = '\0';
            char event_key[96];
            std::snprintf(event_key, sizeof(event_key), "\"event\":\"%s\"", event);
            for (char* line = buf + g_log_start; line && *line;) {
                char* end = std::strchr(line, '\n');
                if (end) *end = '\0';
                count += std::strstr(line, event_key) && std::strstr(line, needle);
                line = end ? end + 1 : nullptr;
            }
        }
        if (buf) HeapFree(GetProcessHeap(), 0, buf);
    }
    CloseHandle(f);
    return count;
}

UINT host_present_count(IBxPresentSwapChain* chain) {
    IDXGISwapChain1* h = nullptr;
    UINT count = 0;
    const bool ok = SUCCEEDED(chain->GetHostSwapChain(__uuidof(IDXGISwapChain1), reinterpret_cast<void**>(&h))) &&
                    SUCCEEDED(h->GetLastPresentCount(&count));
    check(ok, "reading the host present count");
    release(h);
    return count;
}

/* ---- Lifetime: the bridge against a swapchain made with host DXGI alone ---------------------- */

/* Attached to a swapchain as private data; it stays referenced for as long as that data lives. */
struct Witness final : public IUnknown {
    LONG refs = 1;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (!IsEqualGUID(riid, __uuidof(IUnknown))) return E_NOINTERFACE;
        *ppv = this;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&refs)); }
    ULONG STDMETHODCALLTYPE Release() override { return static_cast<ULONG>(InterlockedDecrement(&refs)); }
};
const GUID kWitnessKey = {0xb3b13f0b, 0xe98a, 0x4a4f, {0x82, 0xc2, 0xc1, 0x30, 0xc3, 0x71, 0xcf, 0xc8}};

struct HostDxgi {
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    IDXGIFactory4* factory = nullptr;
};

HRESULT host_swapchain(const HostDxgi& host, HWND window, IDXGISwapChain1** out) {
    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Width = kWidth;
    sd.Height = kHeight;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = kBuffers;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    return host.factory->CreateSwapChainForHwnd(host.queue, window, &sd, nullptr, nullptr, out);
}

bool open_host(HostDxgi& host) {
    using CreateDevice = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
    using CreateFactory = HRESULT(WINAPI*)(REFIID, void**);
    auto create_device = proc<CreateDevice>("d3d12.dll", "D3D12CreateDevice");
    auto create_factory = proc<CreateFactory>("dxgi.dll", "CreateDXGIFactory1");
    if (!create_device || !create_factory) return false;
    D3D12_COMMAND_QUEUE_DESC qd = {};
    return SUCCEEDED(create_device(nullptr, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), reinterpret_cast<void**>(&host.device))) &&
           SUCCEEDED(host.device->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(&host.queue))) &&
           SUCCEEDED(create_factory(__uuidof(IDXGIFactory4), reinterpret_cast<void**>(&host.factory)));
}

struct Retention {
    ULONG holder_refs = 0; /* swapchain references seen while its one owner (app or bridge) holds it */
    LONG retained = 0;     /* witnesses still referenced after every create/release cycle */
    ULONG released[4] = {}; /* host control: what each final swapchain Release returned */
};

constexpr int kProbeCycles = 4;

Retention probe_host(const HostDxgi& host, Witness (&witness)[kProbeCycles]) {
    Retention result;
    for (int i = 0; i < kProbeCycles; ++i) {
        HWND window = make_window();
        IDXGISwapChain1* swap = nullptr;
        HRESULT hr = window ? host_swapchain(host, window, &swap) : E_FAIL;
        if (!check(SUCCEEDED(hr), "control: host swapchain %d hr=0x%08lx", i, ul(hr))) return result;
        result.holder_refs = ref_count(swap);
        check(SUCCEEDED(swap->SetPrivateDataInterface(kWitnessKey, &witness[i])) && witness[i].refs == 2,
              "control: witness %d must be referenced by the swapchain (refs=%ld)", i, witness[i].refs);
        check(SUCCEEDED(swap->Present(0, 0)), "control: host Present %d", i);
        pump(50);
        result.released[i] = swap->Release();
        DestroyWindow(window);
        pump(50);
    }
    pump(300);
    for (const Witness& w : witness) result.retained += w.refs > 1;
    return result;
}

Retention probe_bridge(IBxPresentQueue* bridge, Witness (&witness)[kProbeCycles], const Retention& control) {
    Retention result;
    for (int i = 0; i < kProbeCycles; ++i) {
        HWND window = make_window();
        IBxPresentSwapChain* chain = nullptr;
        HRESULT hr = bridge->CreateSwapChain(window, &kDesc, &chain);
        if (!check(SUCCEEDED(hr) && chain, "bridge swapchain %d hr=0x%08lx", i, ul(hr))) return result;
        IDXGISwapChain1* swap = nullptr;
        hr = chain->GetHostSwapChain(__uuidof(IDXGISwapChain1), reinterpret_cast<void**>(&swap));
        if (!check(SUCCEEDED(hr) && swap, "GetHostSwapChain %d hr=0x%08lx", i, ul(hr))) return result;
        result.holder_refs = ref_count(swap) - 1; /* minus the reference taken just above */
        check(SUCCEEDED(swap->SetPrivateDataInterface(kWitnessKey, &witness[i])) && witness[i].refs == 2,
              "witness %d must be referenced by the bridge's swapchain (refs=%ld)", i, witness[i].refs);
        swap->Release();
        hr = chain->Present(0, 0);
        check(SUCCEEDED(hr), "bridge Present %d hr=0x%08lx", i, ul(hr));
        pump(50);
        char path[MAX_PATH], needle[64];
        std::snprintf(needle, sizeof(needle), "presented=1 swapchain_release_returned=%lu\"", control.released[i]);
        const UINT before = log_count("present_chain_destroyed", needle);
        check(chain->Release() == 0, "bridge swapchain %d still referenced after its final Release", i);
        /* An extra bridge reference would raise this above what the host-only control saw. */
        if (log_path(path)) {
            check(log_count("present_chain_destroyed", needle) == before + 1,
                  "bridge swapchain %d: host Release did not return the control's %lu", i, control.released[i]);
        }
        DestroyWindow(window);
        pump(50);
    }
    pump(300);
    for (const Witness& w : witness) result.retained += w.refs > 1;
    return result;
}

struct PresentCall {
    IBxPresentSwapChain* chain;
    HRESULT result;
};

DWORD WINAPI present_thread(void* arg) {
    auto* call = static_cast<PresentCall*>(arg);
    call->result = call->chain->Present(0, 0);
    return 0;
}

/*
 * Fault injection from the test side. The shipping DLL has no injection switch.
 * The swapchain's vtable is patched in place. D3DMetal's command queue and fence vtables are not
 * memory that VirtualProtect can reach, so those objects get static patched copies instead.
 */
void* patch_slot(void* object, unsigned slot, void* replacement) {
    void** vtable = *reinterpret_cast<void***>(object);
    DWORD old = 0;
    if (!VirtualProtect(&vtable[slot], sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) return nullptr;
    void* previous = vtable[slot];
    vtable[slot] = replacement;
    VirtualProtect(&vtable[slot], sizeof(void*), old, &old);
    return previous;
}

constexpr unsigned kSlotSwapChainPresent = 8; /* IDXGISwapChain::Present */
constexpr unsigned kSlotQueueGetDevice = 7;   /* ID3D12DeviceChild::GetDevice */
constexpr unsigned kSlotQueueSignal = 14;     /* ID3D12CommandQueue::Signal */
constexpr unsigned kQueueSlots = 19;          /* IUnknown through ID3D12CommandQueue::GetDesc */
constexpr unsigned kSlotDeviceCreateFence = 36; /* ID3D12Device::CreateFence */
constexpr unsigned kSlotDeviceRemovedReason = 37; /* ID3D12Device::GetDeviceRemovedReason */
constexpr unsigned kDeviceSlots = 44;         /* IUnknown through ID3D12Device::GetAdapterLuid */
constexpr unsigned kSlotFenceCompleted = 8;   /* ID3D12Fence::GetCompletedValue */
constexpr unsigned kSlotFenceSetEvent = 9;    /* ID3D12Fence::SetEventOnCompletion */
constexpr unsigned kFenceSlots = 11;          /* IUnknown through ID3D12Fence::Signal */

/* d3d12_xs's private "give me the host object" IID (proxy.cpp). No reference is added. */
const GUID kIidProxyHost = {0x6b1d5c0e, 0x2f4a, 0x4e0b, {0x9a, 0x57, 0x3c, 0x0e, 0x5d, 0x7b, 0x1a, 0x42}};

using HostPresentFn = HRESULT(STDMETHODCALLTYPE*)(void*, UINT, UINT);
using QueueGetDeviceFn = HRESULT(STDMETHODCALLTYPE*)(void*, REFIID, void**);
using QueueSignalFn = HRESULT(STDMETHODCALLTYPE*)(void*, ID3D12Fence*, UINT64);
using DeviceCreateFenceFn = HRESULT(STDMETHODCALLTYPE*)(void*, UINT64, D3D12_FENCE_FLAGS, REFIID, void**);
using DeviceRemovedReasonFn = HRESULT(STDMETHODCALLTYPE*)(void*);
using FenceCompletedFn = UINT64(STDMETHODCALLTYPE*)(void*);
using FenceSetEventFn = HRESULT(STDMETHODCALLTYPE*)(void*, UINT64, HANDLE);

struct Faults {
    HostPresentFn present = nullptr;
    QueueGetDeviceFn get_device = nullptr;
    QueueSignalFn signal = nullptr;
    DeviceCreateFenceFn create_fence = nullptr;
    DeviceRemovedReasonFn removed_reason = nullptr;
    FenceCompletedFn fence_completed = nullptr;
    FenceSetEventFn set_event = nullptr;
    void* target = nullptr;             /* the swapchain the Present hook acts on */
    void* captured_device = nullptr;
    void** real_device_vtable = nullptr;
    void* hooked_device_vtable[kDeviceSlots] = {};
    void* target_fence = nullptr;
    void** real_fence_vtable = nullptr;
    void* hooked_fence_vtable[kFenceSlots] = {};
    HANDLE entered = nullptr;           /* set once the hooked host Present is running */
    HANDLE resume = nullptr;            /* the hooked host Present waits for this */
    volatile LONG hold_present = 0;     /* one-shot: wait inside the host Present */
    volatile LONG fail_after_hr = 0;    /* one-shot: fail Signal after host Present with this HRESULT */
    volatile LONG fail_signal_hr = 0;   /* one-shot: HRESULT returned by hooked_signal */
    volatile LONG signal_delay_ms = 0;  /* one-shot: consume the wait budget inside Signal */
    ULONGLONG delayed_signal_finished = 0;
    volatile LONG signals_seen = 0;
    volatile LONG signals_failed = 0;
    volatile LONG capture_fence = 0;
    volatile LONG device_removed_reason = 0; /* one-shot device status override */
    volatile LONG fail_completed_after_signal = 0;
    volatile LONG fail_completed_removed = 0;
    volatile LONG fail_set_event = 0;    /* one-shot: fail SetEventOnCompletion with E_OUTOFMEMORY */
    volatile LONG fail_set_event_hr = 0; /* one-shot: fail SetEventOnCompletion with custom HRESULT */
    volatile LONG set_events_failed = 0;
    volatile LONG hold_timed_out = 0;
} g_faults;

HRESULT STDMETHODCALLTYPE hooked_present(void* self, UINT sync, UINT flags) {
    if (self == g_faults.target && InterlockedExchange(&g_faults.hold_present, 0)) {
        SetEvent(g_faults.entered);
        /* Stands for DXGI waiting on the message-pump thread. Bounded so a failure cannot hang the run. */
        if (WaitForSingleObject(g_faults.resume, 3000) != WAIT_OBJECT_0) InterlockedExchange(&g_faults.hold_timed_out, 1);
    }
    const HRESULT hr = g_faults.present(self, sync, flags);
    const LONG after_hr = InterlockedExchange(&g_faults.fail_after_hr, 0);
    if (self == g_faults.target && SUCCEEDED(hr) && after_hr != 0) {
        InterlockedExchange(&g_faults.fail_signal_hr, after_hr);
    }
    return hr;
}

UINT64 STDMETHODCALLTYPE hooked_fence_completed(void* self) {
    if (self == g_faults.target_fence && InterlockedExchange(&g_faults.fail_completed_removed, 0)) {
        return UINT64_MAX;
    }
    return g_faults.fence_completed(self);
}

HRESULT STDMETHODCALLTYPE hooked_signal(void* self, ID3D12Fence* fence, UINT64 value) {
    InterlockedIncrement(&g_faults.signals_seen);
    if (InterlockedExchange(&g_faults.fail_completed_after_signal, 0)) {
        InterlockedExchange(&g_faults.fail_completed_removed, 1);
    }
    const LONG sig_hr = InterlockedExchange(&g_faults.fail_signal_hr, 0);
    if (sig_hr != 0) {
        InterlockedIncrement(&g_faults.signals_failed);
        return static_cast<HRESULT>(sig_hr); /* nothing is queued */
    }
    const LONG delay_ms = InterlockedExchange(&g_faults.signal_delay_ms, 0);
    if (delay_ms > 0) Sleep(static_cast<DWORD>(delay_ms));
    const HRESULT hr = g_faults.signal(self, fence, value);
    if (delay_ms > 0) g_faults.delayed_signal_finished = GetTickCount64();
    return hr;
}

HRESULT STDMETHODCALLTYPE hooked_create_fence(void* self, UINT64 initial_value, D3D12_FENCE_FLAGS flags,
                                              REFIID riid, void** ppv) {
    const HRESULT hr = g_faults.create_fence(self, initial_value, flags, riid, ppv);
    if (g_faults.capture_fence && SUCCEEDED(hr) && ppv && *ppv && IsEqualGUID(riid, __uuidof(ID3D12Fence))) {
        g_faults.target_fence = *ppv;
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hooked_removed_reason(void* self) {
    const LONG hr = InterlockedExchange(&g_faults.device_removed_reason, 0);
    return hr != 0 ? static_cast<HRESULT>(hr) : g_faults.removed_reason(self);
}

HRESULT STDMETHODCALLTYPE hooked_get_device(void* self, REFIID riid, void** ppv) {
    const HRESULT hr = g_faults.get_device(self, riid, ppv);
    if (g_faults.capture_fence && SUCCEEDED(hr) && ppv && *ppv && IsEqualGUID(riid, __uuidof(ID3D12Device))) {
        if (*reinterpret_cast<void***>(*ppv) != g_faults.hooked_device_vtable) {
            if (g_faults.captured_device) {
                *reinterpret_cast<void***>(g_faults.captured_device) = g_faults.real_device_vtable;
                static_cast<IUnknown*>(g_faults.captured_device)->Release();
                g_faults.captured_device = nullptr;
            }
            static_cast<IUnknown*>(*ppv)->AddRef();
            g_faults.captured_device = *ppv;
            g_faults.real_device_vtable = *reinterpret_cast<void***>(*ppv);
            std::memcpy(g_faults.hooked_device_vtable, g_faults.real_device_vtable,
                        sizeof(g_faults.hooked_device_vtable));
            g_faults.create_fence =
                reinterpret_cast<DeviceCreateFenceFn>(g_faults.real_device_vtable[kSlotDeviceCreateFence]);
            g_faults.removed_reason =
                reinterpret_cast<DeviceRemovedReasonFn>(g_faults.real_device_vtable[kSlotDeviceRemovedReason]);
            g_faults.hooked_device_vtable[kSlotDeviceRemovedReason] = reinterpret_cast<void*>(&hooked_removed_reason);
            g_faults.hooked_device_vtable[kSlotDeviceCreateFence] = reinterpret_cast<void*>(&hooked_create_fence);
            *reinterpret_cast<void***>(*ppv) = g_faults.hooked_device_vtable;
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hooked_set_event(void* self, UINT64 value, HANDLE event) {
    if (self == g_faults.target_fence) {
        const LONG hr = InterlockedExchange(&g_faults.fail_set_event_hr, 0);
        if (hr != 0) {
            InterlockedIncrement(&g_faults.set_events_failed);
            return static_cast<HRESULT>(hr);
        }
        if (InterlockedExchange(&g_faults.fail_set_event, 0)) {
            InterlockedIncrement(&g_faults.set_events_failed);
            return E_OUTOFMEMORY;
        }
    }
    return g_faults.set_event(self, value, event);
}

/* Waits for a thread while still dispatching this thread's window messages. */
bool join_pumping(HANDLE thread, DWORD timeout_ms) {
    const ULONGLONG start = GetTickCount64();
    while (WaitForSingleObject(thread, 5) != WAIT_OBJECT_0) {
        if (GetTickCount64() - start >= timeout_ms) return false;
        pump(1);
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);

    char inject[32] = "";
    if (GetEnvironmentVariableA("BX_PRESENTX_INJECT", inject, sizeof(inject)) >= sizeof(inject)) {
        std::strcpy(inject, "?"); /* too long to be a control name */
    }
    if (argc == 3 && !std::strcmp(argv[1], "--inject")) std::snprintf(inject, sizeof(inject), "%s", argv[2]);
    const bool bad_window = !std::strcmp(inject, "bad-window");
    const bool skip_draw = !std::strcmp(inject, "skip-draw");
    const bool wrong_expectation = !std::strcmp(inject, "wrong-expectation");
    const bool skip_wait_fault = !std::strcmp(inject, "skip-wait-fault");
    const bool wrong_removed_hr = !std::strcmp(inject, "wrong-removed-hr");
    const bool zero_timeout_overrun = !std::strcmp(inject, "zero-timeout-overrun");
    const bool valid_inject = !inject[0] || bad_window || skip_draw || wrong_expectation ||
                              skip_wait_fault || wrong_removed_hr || zero_timeout_overrun;
    if ((argc != 1 && (argc != 3 || std::strcmp(argv[1], "--inject"))) || !valid_inject) {
        std::printf("usage: test_d3d12_presentx [--inject bad-window|skip-draw|wrong-expectation|"
                    "skip-wait-fault|wrong-removed-hr|zero-timeout-overrun]\n");
        return 2;
    }
    if (inject[0]) std::printf("NEGATIVE CONTROL: %s (this run must fail)\n", inject);
    g_log_start = log_size();

    /* --- Device, queue, window ----------------------------------------------------------------- */
    g_stage = kSetup;
    ID3D12Device* device = nullptr;
    HRESULT hr = D3D12XboxCreateDevice(nullptr, __uuidof(ID3D12Device), reinterpret_cast<void**>(&device));
    REQUIRE(hr == S_OK && device, "D3D12XboxCreateDevice hr=0x%08lx", ul(hr));
    D3D12_COMMAND_QUEUE_DESC queue_desc = {};
    ID3D12CommandQueue* queue = nullptr;
    hr = device->CreateCommandQueue(&queue_desc, __uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(&queue));
    REQUIRE(hr == S_OK && queue && vtable_in_shim(queue), "CreateCommandQueue hr=0x%08lx", ul(hr));
    HWND window = make_window();
    REQUIRE(window != nullptr, "CreateWindow failed (%lu)", GetLastError());

    /* --- The bridge and its argument contract --------------------------------------------------- */
    g_stage = kContract;
    IBxPresentQueue* bridge = nullptr;
    hr = queue->QueryInterface(IID_IBxPresentQueue, reinterpret_cast<void**>(&bridge));
    REQUIRE(hr == S_OK && bridge && vtable_in_shim(bridge), "queue QI(IBxPresentQueue) hr=0x%08lx", ul(hr));
    {
        IUnknown* from_queue = nullptr;
        IUnknown* from_bridge = nullptr;
        ID3D12CommandQueue* back = nullptr;
        queue->QueryInterface(__uuidof(IUnknown), reinterpret_cast<void**>(&from_queue));
        bridge->QueryInterface(__uuidof(IUnknown), reinterpret_cast<void**>(&from_bridge));
        check(from_queue && from_queue == from_bridge, "the bridge must report the queue's IUnknown identity");
        check(bridge->QueryInterface(__uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(&back)) == S_OK && back == queue,
              "the bridge must answer ID3D12CommandQueue with the queue proxy");
        release(back);
        release(from_bridge);
        release(from_queue);

        ID3D12CommandAllocator* other = nullptr;
        device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator),
                                       reinterpret_cast<void**>(&other));
        void* none = nullptr;
        check(other && other->QueryInterface(IID_IBxPresentQueue, &none) == E_NOINTERFACE && !none,
              "only command queues may answer IBxPresentQueue");
        release(other);

        IBxPresentSwapChain* out = nullptr;
        auto rejected = [&](const char* what, HWND w, const BxPresentDesc* d, HRESULT want) {
            out = reinterpret_cast<IBxPresentSwapChain*>(static_cast<uintptr_t>(0xDEAD));
            HRESULT got = bridge->CreateSwapChain(w, d, &out);
            check(got == want && out == nullptr, "%s: hr=0x%08lx out=%p, wanted 0x%08lx and null", what, ul(got),
                  static_cast<void*>(out), ul(want));
        };
        check(bridge->CreateSwapChain(window, &kDesc, nullptr) == E_POINTER, "null out pointer must be E_POINTER");
        rejected("null description", window, nullptr, E_POINTER);
        rejected("null window", nullptr, &kDesc, DXGI_ERROR_INVALID_CALL);
        HWND dead = make_window();
        DestroyWindow(dead);
        rejected("destroyed window", dead, &kDesc, DXGI_ERROR_INVALID_CALL);
        BxPresentDesc d = kDesc;
        d.width = 0;
        rejected("zero width", window, &d, DXGI_ERROR_INVALID_CALL);
        d = kDesc;
        d.height = 16385;
        rejected("oversized height", window, &d, DXGI_ERROR_INVALID_CALL);
        d = kDesc;
        d.buffer_count = 1;
        rejected("one buffer", window, &d, DXGI_ERROR_INVALID_CALL);
        d = kDesc;
        d.buffer_count = 17;
        rejected("seventeen buffers", window, &d, DXGI_ERROR_INVALID_CALL);
        d = kDesc;
        d.format = DXGI_FORMAT_D32_FLOAT;
        rejected("depth format", window, &d, DXGI_ERROR_INVALID_CALL);

        D3D12_COMMAND_QUEUE_DESC compute_desc = {};
        compute_desc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
        ID3D12CommandQueue* compute = nullptr;
        hr = device->CreateCommandQueue(&compute_desc, __uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(&compute));
        IBxPresentQueue* compute_bridge = nullptr;
        if (check(SUCCEEDED(hr) && compute &&
                      compute->QueryInterface(IID_IBxPresentQueue, reinterpret_cast<void**>(&compute_bridge)) == S_OK,
                  "compute queue setup hr=0x%08lx", ul(hr))) {
            hr = compute_bridge->CreateSwapChain(window, &kDesc, &out);
            check(hr == DXGI_ERROR_INVALID_CALL && !out, "compute queue: hr=0x%08lx", ul(hr));
        }
        release(compute_bridge);
        release(compute);
        char path[MAX_PATH];
        if (log_path(path)) {
            const UINT args = log_count("present_create_rejected", "invalid window or description");
            check(args == 7, "bridge logged %u argument rejections, expected 7", args);
            check(log_count("present_create_rejected", "not a DIRECT queue") == 1, "compute queue rejection not logged");
        }
    }

    /* --- Swapchain ------------------------------------------------------------------------------ */
    g_stage = kCreate;
    HWND target_window = window;
    if (bad_window) {
        target_window = make_window();
        DestroyWindow(target_window);
    }
    const ULONG queue_refs_before = ref_count(queue);
    IBxPresentSwapChain* chain = nullptr;
    hr = bridge->CreateSwapChain(target_window, &kDesc, &chain);
    REQUIRE(hr == S_OK && chain && vtable_in_shim(chain), "CreateSwapChain hr=0x%08lx", ul(hr));
    check(ref_count(queue) == queue_refs_before + 1, "the swapchain must hold one reference on the queue proxy");

    ID3D12Resource* back_buffers[kBuffers] = {};
    for (UINT i = 0; i < kBuffers; ++i) {
        hr = chain->GetBuffer(i, __uuidof(ID3D12Resource), reinterpret_cast<void**>(&back_buffers[i]));
        REQUIRE(hr == S_OK && back_buffers[i], "GetBuffer(%u) hr=0x%08lx", i, ul(hr));
    }
    {
        void* none = reinterpret_cast<void*>(static_cast<uintptr_t>(0xDEAD));
        hr = chain->GetBuffer(kBuffers, __uuidof(ID3D12Resource), &none);
        check(FAILED(hr) && !none, "GetBuffer past the last index: hr=0x%08lx", ul(hr));
        check(chain->GetBuffer(0, __uuidof(ID3D12Resource), nullptr) == E_POINTER, "GetBuffer(null) must be E_POINTER");
        check(chain->WaitForIdle(INFINITE) == E_INVALIDARG, "an unbounded wait must be rejected");
    }
    D3D12_RESOURCE_DESC bb_desc;
    back_buffers[0]->GetDesc(&bb_desc);
    check(bb_desc.Width == kWidth && bb_desc.Height == kHeight && bb_desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM,
          "back buffer is %llux%u format %d", static_cast<unsigned long long>(bb_desc.Width), bb_desc.Height,
          static_cast<int>(bb_desc.Format));

    /* --- Pipeline ------------------------------------------------------------------------------- */
    g_stage = kPipeline;
    D3D12_ROOT_PARAMETER constant = {};
    constant.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    constant.Constants.Num32BitValues = 1;
    constant.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC rs_desc = {};
    rs_desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_0;
    rs_desc.Desc_1_0.NumParameters = 1;
    rs_desc.Desc_1_0.pParameters = &constant;
    ID3DBlob* rs_blob = nullptr;
    ID3DBlob* rs_error = nullptr;
    hr = D3D12SerializeVersionedRootSignature(&rs_desc, &rs_blob, &rs_error);
    REQUIRE(SUCCEEDED(hr) && rs_blob, "serialize root signature hr=0x%08lx", ul(hr));
    ID3D12RootSignature* root = nullptr;
    hr = device->CreateRootSignature(0, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(), __uuidof(ID3D12RootSignature),
                                     reinterpret_cast<void**>(&root));
    REQUIRE(SUCCEEDED(hr) && root, "CreateRootSignature hr=0x%08lx", ul(hr));
    release(rs_blob);
    release(rs_error);

    ID3DBlob* vs = compile("vs", "vs_5_0");
    ID3DBlob* ps = compile("ps", "ps_5_0");
    REQUIRE(vs && ps, "shader compilation");
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc = {};
    pso_desc.pRootSignature = root;
    pso_desc.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pso_desc.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    pso_desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso_desc.SampleMask = UINT_MAX;
    pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso_desc.RasterizerState.DepthClipEnable = TRUE;
    pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso_desc.NumRenderTargets = 1;
    pso_desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso_desc.SampleDesc.Count = 1;
    ID3D12PipelineState* pso = nullptr;
    hr = device->CreateGraphicsPipelineState(&pso_desc, __uuidof(ID3D12PipelineState), reinterpret_cast<void**>(&pso));
    REQUIRE(SUCCEEDED(hr) && pso, "CreateGraphicsPipelineState hr=0x%08lx", ul(hr));
    release(vs);
    release(ps);

    D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {};
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heap_desc.NumDescriptors = kBuffers;
    ID3D12DescriptorHeap* rtv_heap = nullptr;
    hr = device->CreateDescriptorHeap(&heap_desc, __uuidof(ID3D12DescriptorHeap), reinterpret_cast<void**>(&rtv_heap));
    REQUIRE(SUCCEEDED(hr), "CreateDescriptorHeap hr=0x%08lx", ul(hr));
    D3D12_CPU_DESCRIPTOR_HANDLE rtv[kBuffers];
    rtv_heap->GetCPUDescriptorHandleForHeapStart(&rtv[0]);
    const UINT rtv_step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    for (UINT i = 0; i < kBuffers; ++i) {
        rtv[i].ptr = rtv[0].ptr + static_cast<SIZE_T>(i) * rtv_step;
        device->CreateRenderTargetView(back_buffers[i], nullptr, rtv[i]);
    }

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT64 readback_bytes = 0;
    device->GetCopyableFootprints(&bb_desc, 0, 1, 0, &footprint, nullptr, nullptr, &readback_bytes);
    ID3D12Resource* readback[kFrames] = {};
    D3D12_HEAP_PROPERTIES readback_heap = {};
    readback_heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC buffer_desc = {};
    buffer_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer_desc.Width = readback_bytes;
    buffer_desc.Height = 1;
    buffer_desc.DepthOrArraySize = 1;
    buffer_desc.MipLevels = 1;
    buffer_desc.SampleDesc.Count = 1;
    buffer_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    for (UINT f = 0; f < kFrames; ++f) {
        hr = device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &buffer_desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                             nullptr, __uuidof(ID3D12Resource), reinterpret_cast<void**>(&readback[f]));
        REQUIRE(SUCCEEDED(hr), "readback buffer %u hr=0x%08lx", f, ul(hr));
    }

    ID3D12CommandAllocator* allocators[kBuffers] = {};
    for (UINT i = 0; i < kBuffers; ++i) {
        hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator),
                                            reinterpret_cast<void**>(&allocators[i]));
        REQUIRE(SUCCEEDED(hr), "CreateCommandAllocator hr=0x%08lx", ul(hr));
    }
    ID3D12GraphicsCommandList* list = nullptr;
    hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators[0], nullptr,
                                   __uuidof(ID3D12GraphicsCommandList), reinterpret_cast<void**>(&list));
    REQUIRE(SUCCEEDED(hr) && list && SUCCEEDED(list->Close()), "CreateCommandList hr=0x%08lx", ul(hr));
    ID3D12Fence* fence = nullptr;
    hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), reinterpret_cast<void**>(&fence));
    HANDLE event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    REQUIRE(SUCCEEDED(hr) && fence && event, "fence hr=0x%08lx", ul(hr));

    /* --- Render and present ---------------------------------------------------------------------- */
    g_stage = kRender;
    IDXGISwapChain1* host_chain = nullptr;
    hr = chain->GetHostSwapChain(__uuidof(IDXGISwapChain1), reinterpret_cast<void**>(&host_chain));
    REQUIRE(SUCCEEDED(hr) && host_chain, "GetHostSwapChain hr=0x%08lx", ul(hr));
    UINT presents_before = 0;
    hr = host_chain->GetLastPresentCount(&presents_before);
    check(SUCCEEDED(hr), "GetLastPresentCount hr=0x%08lx", ul(hr));

    IDXGISwapChain3* host3 = nullptr;
    hr = host_chain->QueryInterface(__uuidof(IDXGISwapChain3), reinterpret_cast<void**>(&host3));
    REQUIRE(SUCCEEDED(hr) && host3, "host IDXGISwapChain3 hr=0x%08lx", ul(hr));
    for (UINT i = 0; i < kBuffers; ++i) {
        ID3D12Resource* theirs = nullptr;
        hr = host_chain->GetBuffer(i, __uuidof(ID3D12Resource), reinterpret_cast<void**>(&theirs));
        std::printf("  buffer %u: bridge=%p host=%p\n", i, static_cast<void*>(back_buffers[i]), static_cast<void*>(theirs));
        check(SUCCEEDED(hr) && theirs == back_buffers[i], "bridge buffer %u is not the host swapchain's buffer %u", i, i);
        release(theirs);
    }

    UINT64 slot_value[kBuffers] = {};
    UINT indices_seen = 0;
    UINT frames_presented = 0;
    for (UINT f = 0; f < kFrames; ++f) {
        const UINT slot = chain->GetCurrentBackBufferIndex();
        REQUIRE(slot < kBuffers, "back buffer index %u", slot);
        indices_seen |= 1u << slot;
        /* No wait of our own here: reusing this slot relies on Present() having paced it. */
        check(fence->GetCompletedValue() >= slot_value[slot], "frame %u: Present returned before slot %u was idle", f, slot);
        REQUIRE(SUCCEEDED(allocators[slot]->Reset()) && SUCCEEDED(list->Reset(allocators[slot], pso)), "frame %u: reset", f);

        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = back_buffers[slot];
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        auto transition = [&](D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
            barrier.Transition.StateBefore = from;
            barrier.Transition.StateAfter = to;
            list->ResourceBarrier(1, &barrier);
        };
        transition(D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const Rgba c = clear_colour(f);
        const float clear[4] = {c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, c.a / 255.0f};
        list->ClearRenderTargetView(rtv[slot], clear, 0, nullptr);
        if (!(skip_draw && f == kSkippedFrame)) {
            const D3D12_VIEWPORT viewport = {0.0f, 0.0f, static_cast<float>(kWidth), static_cast<float>(kHeight), 0.0f, 1.0f};
            list->OMSetRenderTargets(1, &rtv[slot], FALSE, nullptr);
            list->RSSetViewports(1, &viewport);
            list->RSSetScissorRects(1, &kRect);
            list->SetGraphicsRootSignature(root);
            list->SetGraphicsRoot32BitConstant(0, pattern_blue(f), 0);
            list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            list->DrawInstanced(3, 1, 0, 0);
        }
        transition(D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION dst = {};
        dst.pResource = readback[f];
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = footprint;
        D3D12_TEXTURE_COPY_LOCATION src = {};
        src.pResource = back_buffers[slot];
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        transition(D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
        REQUIRE(SUCCEEDED(list->Close()), "frame %u: Close", f);

        ID3D12CommandList* const lists[] = {list};
        queue->ExecuteCommandLists(1, lists);
        slot_value[slot] = f + 1;
        REQUIRE(SUCCEEDED(queue->Signal(fence, f + 1)), "frame %u: Signal", f);
        check(host3->GetCurrentBackBufferIndex() == slot, "frame %u: bridge index %u, host index %u", f, slot,
              host3->GetCurrentBackBufferIndex());
        const UINT count_before = host_present_count(chain);
        hr = chain->Present(f % 2, 0); /* alternate immediate and vsync'd presents */
        REQUIRE(SUCCEEDED(hr), "frame %u: Present hr=0x%08lx", f, ul(hr));
        const UINT advanced = host_present_count(chain) - count_before;
        check(advanced == 1, "frame %u: host present count advanced by %u", f, advanced);
        ++frames_presented;
        pump(1);
    }
    hr = chain->WaitForIdle(5000);
    REQUIRE(hr == S_OK && wait_fence(fence, kFrames, event), "GPU did not finish: hr=0x%08lx", ul(hr));
    hr = device->GetDeviceRemovedReason();
    check(hr == S_OK, "device removed: 0x%08lx", ul(hr));
    UINT presents_after = 0;
    hr = host_chain->GetLastPresentCount(&presents_after);
    check(SUCCEEDED(hr) && presents_after - presents_before == kFrames,
          "host swapchain counted %u presents, expected %u (hr=0x%08lx)", presents_after - presents_before, kFrames, ul(hr));
    release(host3);
    release(host_chain);
    check(indices_seen == (1u << kBuffers) - 1, "back buffer indices used: mask 0x%x", indices_seen);
    std::printf("frames_presented=%u host_present_count=%u\n", frames_presented, presents_after - presents_before);

    /* Invalid presents are refused by the bridge and never reach the host swapchain. */
    {
        auto refused = [&](const char* what, UINT sync, UINT flags) {
            UINT before = 0, after = 0;
            IDXGISwapChain1* h = nullptr;
            if (FAILED(chain->GetHostSwapChain(__uuidof(IDXGISwapChain1), reinterpret_cast<void**>(&h)))) {
                check(false, "%s: GetHostSwapChain", what);
                return;
            }
            h->GetLastPresentCount(&before);
            const UINT index = chain->GetCurrentBackBufferIndex();
            HRESULT got = chain->Present(sync, flags);
            h->GetLastPresentCount(&after);
            h->Release();
            check(got == DXGI_ERROR_INVALID_CALL && before == after && index == chain->GetCurrentBackBufferIndex(),
                  "%s: hr=0x%08lx host presents %u -> %u", what, ul(got), before, after);
        };
        refused("sync interval 5", 5, 0);
        refused("DXGI_PRESENT_TEST", 0, DXGI_PRESENT_TEST);
        refused("tearing flag", 0, DXGI_PRESENT_ALLOW_TEARING);

        HWND doomed = make_window();
        IBxPresentSwapChain* orphan = nullptr;
        hr = bridge->CreateSwapChain(doomed, &kDesc, &orphan);
        if (check(SUCCEEDED(hr) && orphan, "swapchain for the destroyed-window case hr=0x%08lx", ul(hr))) {
            DestroyWindow(doomed);
            pump(20);
            hr = orphan->Present(0, 0);
            check(hr == DXGI_ERROR_INVALID_CALL, "present after the window is destroyed: hr=0x%08lx", ul(hr));
            check(orphan->Release() == 0, "orphaned swapchain still referenced");
        }
        char path[MAX_PATH];
        if (log_path(path)) {
            check(log_count("present_rejected", "window_alive=1") == 3 && log_count("present_rejected", "window_alive=0") == 1,
                  "expected 3 argument and 1 destroyed-window present rejections in the log");
        }
    }

    /* --- Pixels ---------------------------------------------------------------------------------- */
    g_stage = kPixels;
    REQUIRE(footprint.Footprint.RowPitch >= kWidth * 4, "row pitch %u is too small", footprint.Footprint.RowPitch);
    UINT64 pixels_checked = 0;
    UINT64 pixels_wrong = 0;
    for (UINT f = 0; f < kFrames; ++f) {
        void* mapped = nullptr;
        const D3D12_RANGE all = {0, static_cast<SIZE_T>(readback_bytes)};
        hr = readback[f]->Map(0, &all, &mapped);
        REQUIRE(SUCCEEDED(hr) && mapped, "Map frame %u hr=0x%08lx", f, ul(hr));
        const auto* data = static_cast<const uint8_t*>(mapped) + footprint.Offset;
        const UINT pitch = footprint.Footprint.RowPitch;
        const UINT bad = count_mismatches(data, pitch, f, wrong_expectation ? 1 : 0, true);
        check(bad == 0, "frame %u: %u of %u pixels differ", f, bad, kWidth * kHeight);
        pixels_checked += kWidth * kHeight;
        pixels_wrong += bad;
        if (f == 0 && !skip_draw && !wrong_expectation) {
            /* The comparison itself must notice: exactly the drawn rectangle differs in each case. */
            check(count_mismatches(data, pitch, f, 1, false) == kRectPixels, "a blue value off by one went unnoticed");
            check(count_mismatches(data, pitch, f + 1, 0, false) == kWidth * kHeight, "a stale frame went unnoticed");
        }
        const D3D12_RANGE none = {0, 0};
        readback[f]->Unmap(0, &none);
    }
    std::printf("pixels_checked=%llu pixels_wrong=%llu row_pitch=%u\n", static_cast<unsigned long long>(pixels_checked),
                static_cast<unsigned long long>(pixels_wrong), footprint.Footprint.RowPitch);
    if (g_failures) finish();

    /* --- Bounded waiting --------------------------------------------------------------------------- */
    g_stage = kSync;
    {
        ID3D12Fence* gate = nullptr;
        hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), reinterpret_cast<void**>(&gate));
        REQUIRE(SUCCEEDED(hr) && gate && SUCCEEDED(queue->Wait(gate, 1)), "gate fence hr=0x%08lx", ul(hr));
        const ULONGLONG start = GetTickCount64();
        hr = chain->WaitForIdle(300);
        const ULONGLONG waited = GetTickCount64() - start;
        check(hr == kTimedOut && waited >= 250 && waited < 3000, "blocked queue: hr=0x%08lx after %llu ms", ul(hr),
              static_cast<unsigned long long>(waited));
        REQUIRE(SUCCEEDED(gate->Signal(1)), "release the gate");
        hr = chain->WaitForIdle(5000);
        check(hr == S_OK, "after the gate opened: hr=0x%08lx", ul(hr));
        release(gate);
    }
    /* The host queue behind the proxy: hooked so the sync stage can capture swapchain fences and inject faults. */
    void* host_queue = nullptr;
    REQUIRE(queue->QueryInterface(kIidProxyHost, &host_queue) == S_OK && host_queue, "host queue behind the proxy");
    void** const real_queue_vtable = *reinterpret_cast<void***>(host_queue);
    static void* hooked_queue_vtable[kQueueSlots];
    std::memcpy(hooked_queue_vtable, real_queue_vtable, sizeof(hooked_queue_vtable));
    g_faults.get_device = reinterpret_cast<QueueGetDeviceFn>(real_queue_vtable[kSlotQueueGetDevice]);
    g_faults.signal = reinterpret_cast<QueueSignalFn>(real_queue_vtable[kSlotQueueSignal]);
    hooked_queue_vtable[kSlotQueueGetDevice] = reinterpret_cast<void*>(&hooked_get_device);
    hooked_queue_vtable[kSlotQueueSignal] = reinterpret_cast<void*>(&hooked_signal);
    *reinterpret_cast<void***>(host_queue) = hooked_queue_vtable;

    auto finish_fence_capture = [&]() {
        InterlockedExchange(&g_faults.capture_fence, 0);
        REQUIRE(g_faults.target_fence != nullptr, "swapchain fence was not captured");
        g_faults.real_fence_vtable = *reinterpret_cast<void***>(g_faults.target_fence);
        std::memcpy(g_faults.hooked_fence_vtable, g_faults.real_fence_vtable, sizeof(g_faults.hooked_fence_vtable));
        g_faults.fence_completed = reinterpret_cast<FenceCompletedFn>(g_faults.real_fence_vtable[kSlotFenceCompleted]);
        g_faults.set_event = reinterpret_cast<FenceSetEventFn>(g_faults.real_fence_vtable[kSlotFenceSetEvent]);
        g_faults.hooked_fence_vtable[kSlotFenceCompleted] = reinterpret_cast<void*>(&hooked_fence_completed);
        g_faults.hooked_fence_vtable[kSlotFenceSetEvent] = reinterpret_cast<void*>(&hooked_set_event);
        *reinterpret_cast<void***>(g_faults.target_fence) = g_faults.hooked_fence_vtable;
    };
    auto detach_fence_hook = [&]() {
        if (g_faults.captured_device) {
            *reinterpret_cast<void***>(g_faults.captured_device) = g_faults.real_device_vtable;
            static_cast<IUnknown*>(g_faults.captured_device)->Release();
            g_faults.captured_device = nullptr;
        }
        if (g_faults.target_fence && g_faults.real_fence_vtable) {
            *reinterpret_cast<void***>(g_faults.target_fence) = g_faults.real_fence_vtable;
        }
        g_faults.target_fence = nullptr;
        g_faults.real_fence_vtable = nullptr;
    };

    {
        /*
         * Frame pacing. With the queue blocked, no frame can finish. Presents 1 and 2 move on to
         * buffers with no earlier use; present 3 wraps to the first buffer and has to wait for
         * frame 1, so it must give up after BX_PRESENT_WAIT_MS instead of handing the buffer back.
         */
        HWND paced_window = make_window();
        IBxPresentSwapChain* paced = nullptr;
        ID3D12Fence* gate = nullptr;
        g_faults.target_fence = nullptr;
        InterlockedExchange(&g_faults.capture_fence, 1);
        REQUIRE(SUCCEEDED(bridge->CreateSwapChain(paced_window, &kDesc, &paced)), "pacing swapchain");
        finish_fence_capture();
        hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), reinterpret_cast<void**>(&gate));
        REQUIRE(SUCCEEDED(hr) && gate && SUCCEEDED(queue->Wait(gate, 1)), "gate fence hr=0x%08lx", ul(hr));
        for (UINT i = 0; i + 1 < kBuffers; ++i) {
            hr = paced->Present(0, 0);
            check(hr == S_OK, "blocked queue, present %u (unused buffer next): hr=0x%08lx", i + 1, ul(hr));
        }
        UINT count = host_present_count(paced);
        ULONGLONG start = GetTickCount64();
        hr = paced->Present(0, 0);
        ULONGLONG waited = GetTickCount64() - start;
        /* No tight upper bound: this time includes the host's own Present. */
        check(hr == BX_PRESENT_E_NEXT_BUFFER_BUSY && waited >= BX_PRESENT_WAIT_MS - 100,
              "present onto a busy buffer: hr=0x%08lx after %llu ms", ul(hr), static_cast<unsigned long long>(waited));
        std::printf("observed: present_pacing_wait_ms=%llu\n", static_cast<unsigned long long>(waited));
        check(host_present_count(paced) == count + 1, "a busy-next-buffer result must mean the frame was presented");
        /*
         * A retry while still blocked presents nothing. It runs on another thread and holds the
         * swapchain for the whole wait, so WaitForIdle here must honour its own, shorter timeout.
         */
        PresentCall retry = {paced, E_FAIL};
        HANDLE thread = CreateThread(nullptr, 0, present_thread, &retry, 0, nullptr);
        REQUIRE(thread != nullptr, "CreateThread");
        Sleep(500);
        const LONG signals_before_contended_idle = g_faults.signals_seen;
        start = GetTickCount64();
        hr = paced->WaitForIdle(300);
        waited = GetTickCount64() - start;
        check(hr == kTimedOut && waited < 2000 && g_faults.signals_seen == signals_before_contended_idle,
              "WaitForIdle behind a Present: hr=0x%08lx after %llu ms (signals +%ld)", ul(hr),
              static_cast<unsigned long long>(waited), g_faults.signals_seen - signals_before_contended_idle);
        REQUIRE(WaitForSingleObject(thread, 30000) == WAIT_OBJECT_0, "retry Present did not return");
        CloseHandle(thread);
        check(retry.result == kTimedOut && host_present_count(paced) == count + 1, "retry while blocked: hr=0x%08lx",
              ul(retry.result));

        /*
         * Pre-Present fence wait failure: the current back buffer is still blocked, so wait_locked
         * before host Present calls SetEventOnCompletion and fails with E_OUTOFMEMORY. Nothing must
         * reach the host swapchain.
         */
        const UINT pre_count = host_present_count(paced);
        InterlockedExchange(&g_faults.fail_set_event, 1);
        hr = paced->Present(0, 0);
        REQUIRE(g_faults.set_events_failed == 1, "pre-Present SetEventOnCompletion fault was not injected (%ld)",
                g_faults.set_events_failed);
        check(hr == E_OUTOFMEMORY && host_present_count(paced) == pre_count,
              "pre-Present wait failure: hr=0x%08lx, host count advanced by %u", ul(hr),
              host_present_count(paced) - pre_count);
        std::printf("observed: pre_present_wait_failed hr=0x%08lx host_presents=%u\n", ul(hr),
                    host_present_count(paced) - pre_count);

        REQUIRE(SUCCEEDED(gate->Signal(1)), "release the gate");
        check(paced->WaitForIdle(5000) == S_OK, "queue did not drain after the gate opened");
        hr = paced->Present(0, 0);
        check(hr == S_OK, "present after recovery: hr=0x%08lx", ul(hr));

        /* Post-present GetCompletedValue()==UINT64_MAX must pass DXGI_ERROR_DEVICE_REMOVED through. */
        check(paced->WaitForIdle(5000) == S_OK, "idle before post-present completed=MAX check");
        count = host_present_count(paced);
        InterlockedExchange(&g_faults.fail_completed_after_signal, 1);
        hr = paced->Present(0, 0);
        std::printf("observed: post_present_completed_max hr=0x%08lx host_presents=%u\n",
                    ul(hr), host_present_count(paced) - count);
        check(hr == DXGI_ERROR_DEVICE_REMOVED && host_present_count(paced) == count + 1,
              "post-present completed=UINT64_MAX must return DXGI_ERROR_DEVICE_REMOVED: hr=0x%08lx", ul(hr));
        char log_file[MAX_PATH];
        if (log_path(log_file)) {
            check(log_has("present_device_removed", "fence"),
                  "post-present completed=UINT64_MAX must log present_device_removed");
        } else {
            std::puts("SKIP: post-present fence removal log (BX_LOG_FILE unset)");
            ++g_skipped;
        }
        check(paced->Present(0, 0) == BX_PRESENT_E_NEEDS_IDLE && host_present_count(paced) == count + 1,
              "post-present removal result on a live device must block reuse");
        check(paced->WaitForIdle(5000) == S_OK, "idle after the completed-value failure");
        check(paced->Present(0, 0) == S_OK && host_present_count(paced) == count + 2,
              "present after completed-value recovery");
        pump(1);

        detach_fence_hook();
        check(paced->Release() == 0, "pacing swapchain still referenced");
        release(gate);
        DestroyWindow(paced_window);
    }
    {
        /*
         * A second Present must not wait for the first. Here the host Present of thread A cannot
         * finish until this thread (the window's owner) has returned from its own Present.
         */
        HWND w = make_window();
        IBxPresentSwapChain* cycle = nullptr;
        IDXGISwapChain3* host_cycle = nullptr;
        g_faults.target_fence = nullptr;
        InterlockedExchange(&g_faults.capture_fence, 1);
        REQUIRE(SUCCEEDED(bridge->CreateSwapChain(w, &kDesc, &cycle)) &&
                    SUCCEEDED(cycle->GetHostSwapChain(__uuidof(IDXGISwapChain3), reinterpret_cast<void**>(&host_cycle))),
                "concurrent-Present swapchain");
        finish_fence_capture();
        g_faults.target = host_cycle;
        g_faults.entered = CreateEventA(nullptr, TRUE, FALSE, nullptr);
        g_faults.resume = CreateEventA(nullptr, TRUE, FALSE, nullptr);
        g_faults.present = reinterpret_cast<HostPresentFn>(
            patch_slot(host_cycle, kSlotSwapChainPresent, reinterpret_cast<void*>(&hooked_present)));
        REQUIRE(g_faults.present && g_faults.entered && g_faults.resume, "hook the host Present");
        const UINT count = host_present_count(cycle);
        InterlockedExchange(&g_faults.hold_present, 1);
        PresentCall first = {cycle, E_FAIL};
        HANDLE thread = CreateThread(nullptr, 0, present_thread, &first, 0, nullptr);
        REQUIRE(thread && WaitForSingleObject(g_faults.entered, 5000) == WAIT_OBJECT_0, "first Present did not reach the host");
        const ULONGLONG start = GetTickCount64();
        hr = cycle->Present(0, 0);
        const ULONGLONG waited = GetTickCount64() - start;
        SetEvent(g_faults.resume);
        REQUIRE(join_pumping(thread, 15000), "first Present did not return");
        CloseHandle(thread);
        std::printf("observed: concurrent_present hr=0x%08lx returned_after_ms=%llu first_hr=0x%08lx host_wait_timed_out=%ld\n",
                    ul(hr), static_cast<unsigned long long>(waited), ul(first.result), g_faults.hold_timed_out);
        check(hr == BX_PRESENT_E_BUSY && waited < 1000, "concurrent Present: hr=0x%08lx after %llu ms", ul(hr),
              static_cast<unsigned long long>(waited));
        check(g_faults.hold_timed_out == 0, "the host Present was stuck behind the second caller");
        check(SUCCEEDED(first.result) && host_present_count(cycle) == count + 1,
              "first Present hr=0x%08lx, host count advanced by %u", ul(first.result), host_present_count(cycle) - count);
        check(cycle->WaitForIdle(5000) == S_OK && SUCCEEDED(cycle->Present(0, 0)), "present after the contention");

        /*
         * The fence signal fails right after a successful host Present, with the queue blocked so the
         * frame really is unfinished. The swapchain must not report any buffer reusable until the
         * queue's completion has been confirmed.
         */
        check(cycle->WaitForIdle(5000) == S_OK, "idle before the signal fault");
        ID3D12Fence* gate = nullptr;
        hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), reinterpret_cast<void**>(&gate));
        REQUIRE(SUCCEEDED(hr) && gate && SUCCEEDED(queue->Wait(gate, 1)), "gate fence hr=0x%08lx", ul(hr));
        UINT before = host_present_count(cycle);
        InterlockedExchange(&g_faults.fail_after_hr, E_FAIL);
        hr = cycle->Present(0, 0);
        REQUIRE(g_faults.signals_failed == 1, "the fault was not injected (%ld signals failed)", g_faults.signals_failed);
        std::printf("observed: present_with_failed_signal hr=0x%08lx host_presents=%u\n", ul(hr), host_present_count(cycle) - before);
        check(hr == BX_PRESENT_E_COMPLETION_UNCONFIRMED && host_present_count(cycle) == before + 1,
              "failed signal after Present: hr=0x%08lx, host count advanced by %u", ul(hr), host_present_count(cycle) - before);
        before = host_present_count(cycle);
        for (UINT i = 0; i < kBuffers; ++i) {
            /* Without recovery these wrap back onto the unfinished buffer and report success. */
            hr = cycle->Present(0, 0);
            check(hr == BX_PRESENT_E_NEEDS_IDLE, "present %u after the failed signal: hr=0x%08lx", i + 1, ul(hr));
        }
        check(host_present_count(cycle) == before, "%u frames reached the host before completion was confirmed",
              host_present_count(cycle) - before);
        hr = cycle->WaitForIdle(300);
        check(hr == kTimedOut, "WaitForIdle with the queue blocked: hr=0x%08lx", ul(hr));
        InterlockedExchange(&g_faults.signal_delay_ms, 1300);
        hr = cycle->WaitForIdle(1000);
        const ULONGLONG after_signal = GetTickCount64() - g_faults.delayed_signal_finished;
        check(hr == kTimedOut && g_faults.delayed_signal_finished != 0 && after_signal < 650,
              "WaitForIdle after its signal used the budget: hr=0x%08lx, extra wait=%llu ms",
              ul(hr), static_cast<unsigned long long>(after_signal));
        check(cycle->Present(0, 0) == BX_PRESENT_E_NEEDS_IDLE, "an unsuccessful WaitForIdle must not recover the swapchain");
        REQUIRE(SUCCEEDED(gate->Signal(1)), "release the gate");
        check(cycle->WaitForIdle(5000) == S_OK, "queue did not drain after the gate opened");
        before = host_present_count(cycle);
        hr = cycle->Present(0, 0);
        check(SUCCEEDED(hr) && host_present_count(cycle) == before + 1, "present after recovery: hr=0x%08lx", ul(hr));
        check(cycle->WaitForIdle(5000) == S_OK, "idle after recovery");
        release(gate);

        /*
         * Post-Present fence wait failure: host Present and Signal succeed, then SetEventOnCompletion
         * fails with E_OUTOFMEMORY while waiting for the next back buffer. Present must return a
         * distinct post-present result (not raw E_OUTOFMEMORY), log the wait HRESULT, and require
         * WaitForIdle to succeed before any further Present / back-buffer reuse.
         */
        ID3D12Fence* wait_gate = nullptr;
        hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), reinterpret_cast<void**>(&wait_gate));
        REQUIRE(SUCCEEDED(hr) && wait_gate && SUCCEEDED(queue->Wait(wait_gate, 1)), "wait gate fence hr=0x%08lx", ul(hr));
        for (UINT i = 0; i + 1 < kBuffers; ++i) {
            hr = cycle->Present(0, 0);
            check(hr == S_OK, "wait-fault setup present %u: hr=0x%08lx", i + 1, ul(hr));
        }
        before = host_present_count(cycle);
        if (!skip_wait_fault) {
            InterlockedExchange(&g_faults.fail_set_event, 1);
        }
        hr = cycle->Present(0, 0);
        REQUIRE(g_faults.set_events_failed == 2, "the SetEventOnCompletion fault was not injected (%ld)",
                g_faults.set_events_failed);
        std::printf("observed: post_present_wait_failed hr=0x%08lx host_presents=%u\n", ul(hr),
                    host_present_count(cycle) - before);
        check(hr == BX_PRESENT_E_NEXT_BUFFER_WAIT_FAILED && hr != E_OUTOFMEMORY && host_present_count(cycle) == before + 1,
              "post-Present wait failure: hr=0x%08lx, host count advanced by %u", ul(hr),
              host_present_count(cycle) - before);
        char path[MAX_PATH];
        if (log_path(path)) {
            check(log_count("present_wait_failed", "SetEventOnCompletion hr=0x8007000e") == 2,
                  "expected 2 logged SetEventOnCompletion E_OUTOFMEMORY failures");
            check(log_count("present_next_buffer_wait_failed", "hr=0x8007000e") == 1,
                  "expected 1 logged present_next_buffer_wait_failed record");
        } else {
            std::puts("SKIP: post-present wait failure logs (BX_LOG_FILE unset)");
            ++g_skipped;
        }
        before = host_present_count(cycle);
        for (UINT i = 0; i < kBuffers; ++i) {
            hr = cycle->Present(0, 0);
            check(hr == BX_PRESENT_E_NEEDS_IDLE, "present %u after post-present wait failure: hr=0x%08lx", i + 1, ul(hr));
        }
        check(host_present_count(cycle) == before, "%u frames reached host before WaitForIdle recovery",
              host_present_count(cycle) - before);
        hr = cycle->WaitForIdle(300);
        check(hr == kTimedOut, "WaitForIdle blocked after wait failure: hr=0x%08lx", ul(hr));
        check(cycle->Present(0, 0) == BX_PRESENT_E_NEEDS_IDLE,
              "unsuccessful WaitForIdle must not clear wait-failure recovery");
        REQUIRE(SUCCEEDED(wait_gate->Signal(1)), "release the wait gate");
        pump(20);
        check(cycle->Present(0, 0) == BX_PRESENT_E_NEEDS_IDLE && host_present_count(cycle) == before,
              "back-buffer reuse must still require WaitForIdle after gate opens");
        check(cycle->WaitForIdle(5000) == S_OK, "WaitForIdle did not succeed after wait gate opened");
        before = host_present_count(cycle);
        hr = cycle->Present(0, 0);
        check(SUCCEEDED(hr) && host_present_count(cycle) == before + 1, "present after wait-failure recovery: hr=0x%08lx",
              ul(hr));
        check(cycle->WaitForIdle(5000) == S_OK, "idle after wait-failure recovery");

        /* Zero is an invalid timeout and must be rejected before calling the host. */
        const LONG signals_before_zero = g_faults.signals_seen;
        InterlockedExchange(&g_faults.signal_delay_ms, 1300);
        const ULONGLONG zero_start = GetTickCount64();
        hr = cycle->WaitForIdle(zero_timeout_overrun ? 5000 : 0);
        const ULONGLONG zero_elapsed = GetTickCount64() - zero_start;
        InterlockedExchange(&g_faults.signal_delay_ms, 0);
        check(hr == E_INVALIDARG && zero_elapsed < 500 && g_faults.signals_seen == signals_before_zero,
              "WaitForIdle(0) must reject zero without signaling: hr=0x%08lx elapsed=%llu ms signals +%ld",
              ul(hr), static_cast<unsigned long long>(zero_elapsed), g_faults.signals_seen - signals_before_zero);

        /* Post-present SetEventOnCompletion returning DXGI_ERROR_DEVICE_RESET must pass through unchanged. */
        REQUIRE(SUCCEEDED(queue->Wait(wait_gate, 2)), "block the queue for device-reset wait check");
        for (UINT i = 0; i + 1 < kBuffers; ++i) {
            check(cycle->Present(0, 0) == S_OK, "device-reset setup, present %u", i + 1);
        }
        before = host_present_count(cycle);
        InterlockedExchange(&g_faults.fail_set_event_hr, wrong_removed_hr ? E_OUTOFMEMORY : DXGI_ERROR_DEVICE_RESET);
        hr = cycle->Present(0, 0);
        std::printf("observed: post_present_device_reset hr=0x%08lx host_presents=%u\n",
                    ul(hr), host_present_count(cycle) - before);
        check(hr == DXGI_ERROR_DEVICE_RESET && host_present_count(cycle) == before + 1,
              "post-present device reset must return DXGI_ERROR_DEVICE_RESET: hr=0x%08lx", ul(hr));
        REQUIRE(SUCCEEDED(wait_gate->Signal(2)), "open the device-reset gate");
        check(cycle->Present(0, 0) == BX_PRESENT_E_NEEDS_IDLE && host_present_count(cycle) == before + 1,
              "post-present device reset on a live device must block reuse");
        check(cycle->WaitForIdle(5000) == S_OK, "idle after device-reset check");
        check(cycle->Present(0, 0) == S_OK && host_present_count(cycle) == before + 2,
              "present after device-reset recovery");
        release(wait_gate);

        /* Post-present Signal failing with DXGI_ERROR_DEVICE_REMOVED must pass through unchanged. */
        before = host_present_count(cycle);
        InterlockedExchange(&g_faults.fail_after_hr, DXGI_ERROR_DEVICE_REMOVED);
        hr = cycle->Present(0, 0);
        check(g_faults.signals_failed == 2 && hr == DXGI_ERROR_DEVICE_REMOVED && host_present_count(cycle) == before + 1,
              "post-present Signal device removal: hr=0x%08lx host_presents=%u signals_failed=%ld",
              ul(hr), host_present_count(cycle) - before, g_faults.signals_failed);
        check(cycle->Present(0, 0) == BX_PRESENT_E_NEEDS_IDLE && host_present_count(cycle) == before + 1,
              "post-present Signal removal result on a live device must block reuse");
        check(cycle->WaitForIdle(5000) == S_OK, "idle after post-present Signal device removal");
        check(cycle->Present(0, 0) == S_OK && host_present_count(cycle) == before + 2,
              "present after Signal removal recovery");
        if (log_path(path)) {
            check(log_has("present_next_buffer_wait_failed", "hr=0x887a0007"),
                  "post-present device reset must log the next-buffer wait failure");
            check(log_has("present_device_removed", "signal after Present hr=0x887a0005"),
                  "post-present Signal device removal must log the original HRESULT");
        } else {
            std::puts("SKIP: post-present removal HRESULT logs (BX_LOG_FILE unset)");
            ++g_skipped;
        }

        /* A failed GetDeviceRemovedReason takes precedence over the fence failure. */
        REQUIRE(SUCCEEDED(cycle->WaitForIdle(5000)), "idle before injecting device status");
        before = host_present_count(cycle);
        InterlockedExchange(&g_faults.device_removed_reason, DXGI_ERROR_DEVICE_HUNG);
        InterlockedExchange(&g_faults.fail_after_hr, E_OUTOFMEMORY);
        hr = cycle->Present(0, 0);
        check(hr == DXGI_ERROR_DEVICE_HUNG && g_faults.device_removed_reason == 0 &&
                  host_present_count(cycle) == before + 1,
              "post-present Signal failure must return the device's removal reason: hr=0x%08lx", ul(hr));
        if (log_path(path)) {
            check(log_has("present_device_removed", "signal after Present hr=0x887a0006"),
                  "post-present Signal failure must log the device's removal reason");
        } else {
            std::puts("SKIP: device removal reason log (BX_LOG_FILE unset)");
            ++g_skipped;
        }

        detach_fence_hook();
        patch_slot(host_cycle, kSlotSwapChainPresent, reinterpret_cast<void*>(g_faults.present));
        release(host_cycle);
        check(cycle->Release() == 0, "fault-injection swapchain still referenced");
        CloseHandle(g_faults.entered);
        CloseHandle(g_faults.resume);
        DestroyWindow(w);
    }
    *reinterpret_cast<void***>(host_queue) = real_queue_vtable;
    {
        /* Final Release with the queue blocked: the leak branch must still return the proxy reference. */
        HWND w = make_window();
        IBxPresentSwapChain* stuck = nullptr;
        ID3D12Fence* gate = nullptr;
        const ULONG refs = ref_count(queue);
        REQUIRE(SUCCEEDED(bridge->CreateSwapChain(w, &kDesc, &stuck)), "leak-path swapchain");
        hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), reinterpret_cast<void**>(&gate));
        REQUIRE(SUCCEEDED(hr) && gate && SUCCEEDED(queue->Wait(gate, 1)), "gate fence hr=0x%08lx", ul(hr));
        check(stuck->Release() == 0, "blocked swapchain Release");
        check(ref_count(queue) == refs, "the leak branch kept a queue proxy reference (%lu, wanted %lu)", ref_count(queue), refs);
        char path[MAX_PATH];
        if (log_path(path)) check(log_count("present_chain_leaked", "not released") == 1, "no present_chain_leaked record");
        REQUIRE(SUCCEEDED(gate->Signal(1)), "release the gate");
        release(gate);
        /* The window is left open: the leaked swapchain still refers to it. */
    }

    /* --- Teardown and repetition ------------------------------------------------------------------ */
    g_stage = kLifetime;
    for (ID3D12Resource*& b : back_buffers) release(b);
    chain->AddRef();
    check(chain->Release() == 1, "swapchain reference counting");
    check(chain->Release() == 0, "the swapchain must reach zero references");
    chain = nullptr;
    check(ref_count(queue) == queue_refs_before, "destroying the swapchain must return its queue reference");

    HostDxgi host;
    REQUIRE(open_host(host), "host DXGI control setup");

    /* Whatever the host does with a second or a recreated swapchain, the bridge must report the same. */
    {
        HWND w = make_window();
        IDXGISwapChain1* first = nullptr;
        IDXGISwapChain1* second = nullptr;
        REQUIRE(SUCCEEDED(host_swapchain(host, w, &first)), "control: first swapchain");
        const HRESULT host_second = host_swapchain(host, w, &second);
        release(second);
        first->Release();
        pump(50);
        const HRESULT host_again = host_swapchain(host, w, &second);
        release(second);
        DestroyWindow(w);

        w = make_window();
        IBxPresentSwapChain* a = nullptr;
        IBxPresentSwapChain* b = nullptr;
        REQUIRE(SUCCEEDED(bridge->CreateSwapChain(w, &kDesc, &a)), "bridge: first swapchain");
        const HRESULT bridge_second = bridge->CreateSwapChain(w, &kDesc, &b);
        release(b);
        release(a);
        pump(50);
        const HRESULT bridge_again = bridge->CreateSwapChain(w, &kDesc, &b);
        release(b);
        DestroyWindow(w);
        check(bridge_second == host_second, "second swapchain on one window: bridge 0x%08lx, host 0x%08lx", ul(bridge_second),
              ul(host_second));
        check(bridge_again == host_again, "recreation on one window: bridge 0x%08lx, host 0x%08lx", ul(bridge_again),
              ul(host_again));
        std::printf("observed: second_swapchain_on_window host=0x%08lx bridge=0x%08lx; recreate_after_release host=0x%08lx bridge=0x%08lx\n",
                    ul(host_second), ul(bridge_second), ul(host_again), ul(bridge_again));
    }

    {
        static Witness host_witness[kProbeCycles]; /* static: a retained swapchain may outlive this scope */
        static Witness bridge_witness[kProbeCycles];
        const Retention direct = probe_host(host, host_witness);
        const Retention bridged = probe_bridge(bridge, bridge_witness, direct);
        check(bridged.holder_refs == direct.holder_refs, "swapchain references while held: bridge %lu, host control %lu",
              bridged.holder_refs, direct.holder_refs);
        check(bridged.retained == direct.retained, "swapchains retained after release: bridge %ld, host control %ld",
              bridged.retained, direct.retained);
        std::printf("observed: swapchain_refs_while_held host=%lu bridge=%lu; retained_after_release host=%ld/%d bridge=%ld/%d\n",
                    direct.holder_refs, bridged.holder_refs, direct.retained, kProbeCycles, bridged.retained, kProbeCycles);
    }

    char configured_log[MAX_PATH];
    if (log_path(configured_log)) {
        char needle[32];
        std::snprintf(needle, sizeof(needle), "presented=%u ", kFrames);
        check(log_has("present_chain_created", "width=100 height=60"), "no present_chain_created record from this run");
        check(log_has("present_chain_destroyed", needle), "no present_chain_destroyed record with %s", needle);
    } else {
        g_skipped++;
        std::printf("SKIP: BX_LOG_FILE is not set; present_chain log records not verified (use bx-cli run --log)\n");
    }

    release(host.factory);
    release(host.queue);
    release(host.device);
    CloseHandle(event);
    release(fence);
    release(list);
    for (ID3D12CommandAllocator*& a : allocators) release(a);
    for (ID3D12Resource*& r : readback) release(r);
    release(rtv_heap);
    release(pso);
    release(root);
    release(bridge);
    check(queue->Release() == 0, "command queue proxy leaked references");
    check(device->Release() == 0, "device proxy leaked references");
    DestroyWindow(window);
    finish();
}
