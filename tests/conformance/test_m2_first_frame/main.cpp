// Original synthetic M2 workload. The compute output is copied into the flip
// buffer and read back through a placed D3D12 buffer before the host flip.
//
// Limitations: the frame is a compute write copied into the back buffer, not a
// rasterized draw. The shader-gate stage covers only the DXIL fast path. The
// pixel compare reads pre-flip back-buffer contents, not what reached the
// screen. The readback lives in the D3D12 WRITE_BACK backing heap's own map;
// the xmem page only names the configured region and is checked to stay
// untouched, so this does not show xmem-backed placed memory (#469).
#define WIDL_EXPLICIT_AGGREGATE_RETURNS
#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include "bx_log.h"
#include "fill_cs66.h"
#include "presentx.h"
#include "vmem.h"

using Microsoft::WRL::ComPtr;
extern "C" {
__declspec(dllimport) HRESULT WINAPI D3D12ConfigureVirtualMemoryX(void*);
__declspec(dllimport) HRESULT WINAPI D3D12XboxCreateDevice(void*, REFIID, void**);
__declspec(dllimport) BOOL WINAPI XMemAllocatePhysicalPages(SIZE_T*, PULONG_PTR, uint64_t);
__declspec(dllimport) BOOL WINAPI XMemFreePhysicalPages(SIZE_T*, PULONG_PTR, uint64_t);
__declspec(dllimport) PVOID WINAPI XMemMapPhysicalPages(PVOID, SIZE_T, uint64_t, PULONG_PTR);
}

namespace {
const char* stage = "gdk_boot";
unsigned seq = 0;
void checkpoint(bool ok, const char* detail) {
    std::printf(
        "{\"bx_checkpoint\":\"test_m2_first_frame\",\"seq\":%u,\"stage\":\"%s\",\"status\":\"%s\",\"detail\":\"%s\"}\n",
        ++seq, stage, ok ? "pass" : "fail", detail);
    std::fflush(stdout);
    char message[384];
    std::snprintf(message, sizeof(message), "%s: %s", stage, detail);
    bx_log_emit(ok ? BX_LOG_INFO : BX_LOG_ERROR, "m2_first_frame", ok ? "stage_pass" : "stage_fail", message);
}
bool cleanup_ok = true;
void cleanup(bool ok, const char* call) {
    if (!ok) {
        cleanup_ok = false;
        checkpoint(false, call);
    }
}
struct Failure {};
void require(bool ok, const char* detail) {
    if (!ok) {
        checkpoint(false, detail);
        throw Failure{};
    }
}
void checked(HRESULT hr, const char* call) {
    if (FAILED(hr)) {
        char detail[192];
        std::snprintf(detail, sizeof(detail), "%s hr=0x%08lx", call, static_cast<unsigned long>(hr));
        require(false, detail);
    }
}
template <class T> T symbol(HMODULE module, const char* name) {
    auto address = GetProcAddress(module, name);
    require(address != nullptr, name);
    T result;
    std::memcpy(&result, &address, sizeof(result));
    return result;
}
struct Runtime {
    HMODULE module = LoadLibraryExW(L"xgameruntime.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    HRESULT(WINAPI* uninit)() = nullptr;
    bool active = false;
    ~Runtime() {
        if (active)
            cleanup(uninit() == S_OK, "UninitializeApiImpl");
        if (module)
            cleanup(FreeLibrary(module), "FreeLibrary");
    }
};
struct Region {
    static constexpr size_t page_bytes = 65536;
    static constexpr unsigned char sentinel = 0xa5;
    SIZE_T count = 1;
    ULONG_PTR page = 0;
    void* view = nullptr;
    BxD3d12VirtualMemoryConfig config = [] {
        // Assigned by name so a field reorder in vmem.h cannot compile silently.
        BxD3d12VirtualMemoryConfig c{};
        c.magic = BX_D3D12_VMEM_MAGIC;
        c.struct_size = sizeof(BxD3d12VirtualMemoryConfig);
        c.version = 1;
        c.flags = BX_D3D12_VMEM_FLAG_BACKING_HEAP;
        c.base_va = 0;
        c.size_bytes = page_bytes;
        c.reserved = 0;
        c.cpu_pointer = 0;
        return c;
    }();
    ~Region() {
        if (config.base_va) {
            config.base_va = config.size_bytes = 0;
            cleanup(D3D12ConfigureVirtualMemoryX(&config) == S_OK, "clear configured region");
        }
        if (view)
            cleanup(XMemMapPhysicalPages(view, count, 0, nullptr) == view, "unmap physical page");
        if (page)
            cleanup(XMemFreePhysicalPages(&count, &page, 0), "free physical page");
    }
};
// Declared just before ExecuteCommandLists, so on a failure after the submit it is
// destroyed before the queue, back buffer and output resource it protects: it
// signals the queue and waits (bounded) so nothing is released while the GPU may
// still be using it. Disarmed once the readback fence has completed.
struct GpuDrain {
    ID3D12CommandQueue* queue = nullptr;
    ID3D12Fence* fence = nullptr;
    bool armed = false;
    ~GpuDrain() {
        if (!armed) return;
        HANDLE done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        bool drained = done && SUCCEEDED(queue->Signal(fence, 2)) &&
                       (fence->GetCompletedValue() >= 2 ||
                        (SUCCEEDED(fence->SetEventOnCompletion(2, done)) &&
                         WaitForSingleObject(done, 5000) == WAIT_OBJECT_0));
        if (done) CloseHandle(done);
        cleanup(drained, "drain the queue before releasing resources after a failure");
    }
};
struct Window {
    HWND hwnd = CreateWindowExW(0, L"STATIC", L"BetterXen M2", WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT,
                                CW_USEDEFAULT, 320, 240, nullptr, nullptr, nullptr, nullptr);
    ~Window() {
        if (hwnd)
            cleanup(DestroyWindow(hwnd), "destroy window");
    }
};

void run(bool inject_pixel_mismatch) {
    Runtime rt;
    require(rt.module, "LoadLibraryExW system32");
    auto init = symbol<HRESULT(WINAPI*)(uint32_t, uint32_t)>(rt.module, "InitializeApiImpl");
    rt.uninit = symbol<decltype(rt.uninit)>(rt.module, "UninitializeApiImpl");
    auto query = symbol<HRESULT(WINAPI*)(const GUID*, const GUID*, void**)>(rt.module, "QueryApiImpl");
    checked(init(1, 1), "InitializeApiImpl");
    rt.active = true;
    const GUID core_guid = {0x073B7DCB, 0x1FCF, 0x4030, {0x94, 0xBE, 0xE3, 0xC9, 0xEB, 0x62, 0x34, 0x28}};
    const GUID sys_clsid = {0xE349BD1A, 0xFC20, 0x4E40, {0xB9, 0x9C, 0x41, 0x78, 0xCC, 0x6B, 0x40, 0x9F}};
    const GUID sys_iid = {0x1861CF2E, 0xE18B, 0x4834, {0xA9, 0xF5, 0xB4, 0xA4, 0xE6, 0xEF, 0xB4, 0xCF}};
    const GUID user_guid = {0x01ACD177, 0x91F9, 0x4763, {0xA3, 0x8E, 0xCC, 0xBB, 0x55, 0xCE, 0x32, 0xE0}};
    ComPtr<IUnknown> core, system, user;
    checked(query(&core_guid, &core_guid, reinterpret_cast<void**>(core.GetAddressOf())), "IXGameRuntimeCore");
    checked(query(&sys_clsid, &sys_iid, reinterpret_cast<void**>(system.GetAddressOf())), "IXSystem");
    checked(query(&user_guid, &user_guid, reinterpret_cast<void**>(user.GetAddressOf())), "IXUser");
    require(core && system && user, "GDK COM interfaces");
    checkpoint(true, "runtime initialized and three public interfaces acquired");

    stage = "placed_resource";
    Region region;
    require(XMemAllocatePhysicalPages(&region.count, &region.page, 0) && region.count == 1, "allocate physical page");
    region.view = XMemMapPhysicalPages(nullptr, 1, 0, &region.page);
    require(region.view, "map physical page");
    std::memset(region.view, Region::sentinel, Region::page_bytes);
    region.config.base_va = reinterpret_cast<uintptr_t>(region.view);
    checked(D3D12ConfigureVirtualMemoryX(&region.config), "configure xmem region");
    ComPtr<ID3D12Device> device;
    checked(D3D12XboxCreateDevice(nullptr, IID_PPV_ARGS(&device)), "create Xbox device");
    D3D12_HEAP_DESC hd{};
    hd.SizeInBytes = region.config.size_bytes;
    hd.Properties.Type = D3D12_HEAP_TYPE_CUSTOM;
    hd.Properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_BACK;
    hd.Properties.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
    ComPtr<ID3D12Heap> heap;
    checked(device->CreateHeap(&hd, IID_PPV_ARGS(&heap)), "create configured heap");
    checked(D3D12ConfigureVirtualMemoryX(&region.config), "query backing map");
    require(region.config.cpu_pointer != 0, "configured heap must publish its backing map");
    require(region.config.cpu_pointer != reinterpret_cast<uintptr_t>(region.view),
            "D3D12 backing map must be distinct from the xmem region view");
    constexpr UINT width = 64, height = 4, pitch = 256;
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = height * pitch;
    buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> readback;
    checked(device->CreatePlacedResource(heap.Get(), 0, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                         IID_PPV_ARGS(&readback)),
            "place readback buffer");
    checkpoint(true, "xmem region configured; readback placed in a separate D3D12 WRITE_BACK heap");

    stage = "shader_gate";
    char path[MAX_PATH];
    DWORD length = GetEnvironmentVariableA("BX_LOG_FILE", path, sizeof(path));
    require(length && length < sizeof(path), "BX_LOG_FILE required");
    // Start at EOF before PSO creation so stale shader_gate records cannot pass this check.
    std::ifstream log(path, std::ios::binary | std::ios::ate);
    require(log.good(), "open launch log");
    D3D12_ROOT_PARAMETER param{};
    param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC signature{};
    signature.Version = D3D_ROOT_SIGNATURE_VERSION_1_0;
    signature.Desc_1_0.NumParameters = 1;
    signature.Desc_1_0.pParameters = &param;
    ComPtr<ID3DBlob> blob;
    checked(D3D12SerializeVersionedRootSignature(&signature, &blob, nullptr), "serialize root signature");
    ComPtr<ID3D12RootSignature> root;
    checked(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root)),
            "root signature");
    D3D12_COMPUTE_PIPELINE_STATE_DESC ps{};
    ps.pRootSignature = root.Get();
    ps.CS = {g_fill_cs66, sizeof(g_fill_cs66)};
    ComPtr<ID3D12PipelineState> pso;
    checked(device->CreateComputePipelineState(&ps, IID_PPV_ARGS(&pso)), "SM 6.6 compute PSO");
    bool inspected = false;
    for (std::string line; std::getline(log, line);) {
        if (line.find("\"event\":\"shader_gate\"") != line.npos && line.find("decision=pass_dxil") != line.npos &&
            line.find("pso=compute") != line.npos && line.find("sm=6.6") != line.npos)
            inspected = true;
    }
    require(inspected, "missing SM 6.6 pass_dxil telemetry for PSO");
    checkpoint(true, "inspected SM 6.6 compute PSO");

    stage = "present";
    Window window;
    require(window.hwnd, "create window");
    D3D12_COMMAND_QUEUE_DESC qd{};
    ComPtr<ID3D12CommandQueue> queue;
    checked(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)), "direct queue");
    ComPtr<IBxPresentQueue> bridge;
    checked(queue->QueryInterface(IID_IBxPresentQueue, reinterpret_cast<void**>(bridge.GetAddressOf())),
            "PresentX queue interface");
    BxPresentDesc desc{width, height, DXGI_FORMAT_R8G8B8A8_UNORM, 2};
    ComPtr<IBxPresentSwapChain> chain;
    checked(bridge->CreateSwapChain(window.hwnd, &desc, &chain), "create flip chain");
    ComPtr<ID3D12Resource> back;
    checked(chain->GetBuffer(chain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&back)), "get current backbuffer");
    D3D12_HEAP_PROPERTIES props{};
    props.Type = D3D12_HEAP_TYPE_DEFAULT;
    buffer.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> output;
    checked(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &buffer,
                                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&output)),
            "shader output");
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> list;
    checked(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)), "command allocator");
    checked(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), pso.Get(), IID_PPV_ARGS(&list)),
            "command list");
    list->SetComputeRootSignature(root.Get());
    list->SetComputeRootUnorderedAccessView(0, output->GetGPUVirtualAddress());
    list->Dispatch(height, 1, 1);
    auto transition = [&](ID3D12Resource* resource, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from, to};
        list->ResourceBarrier(1, &barrier);
    };
    transition(output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    transition(back.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION texture{}, linear{};
    texture.pResource = back.Get();
    texture.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    linear.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    linear.PlacedFootprint.Footprint = {desc.format, width, height, 1, pitch};
    linear.pResource = output.Get();
    list->CopyTextureRegion(&texture, 0, 0, 0, &linear, nullptr);
    if (inject_pixel_mismatch) {
        const D3D12_BOX second_pixel{1, 0, 0, 2, 1, 1};
        list->CopyTextureRegion(&texture, 0, 0, 0, &linear, &second_pixel);
    }
    transition(back.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    linear.pResource = readback.Get();
    list->CopyTextureRegion(&linear, 0, 0, 0, &texture, nullptr);
    transition(back.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
    checked(list->Close(), "close command list");
    ID3D12CommandList* commands[] = {list.Get()};
    ComPtr<IDXGISwapChain1> host;
    checked(chain->GetHostSwapChain(IID_PPV_ARGS(&host)), "get host swapchain");
    UINT before = 0, after = 0;
    checked(host->GetLastPresentCount(&before), "initial host present count");
    ComPtr<ID3D12Fence> fence;
    checked(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "readback fence");
    GpuDrain drain{queue.Get(), fence.Get(), true};
    queue->ExecuteCommandLists(1, commands);
    const HRESULT present_hr = chain->Present(0, 0);
    if (present_hr != S_OK) {
        char detail[96];
        std::snprintf(detail, sizeof(detail), "PresentX hr=0x%08lx", static_cast<unsigned long>(present_hr));
        require(false, detail);
    }
    checked(chain->WaitForIdle(5000), "wait for frame");
    // Wait on our own queue fence before the CPU reads the backing map, so the
    // readback does not depend on what WaitForIdle covers.
    checked(queue->Signal(fence.Get(), 1), "signal readback fence");
    if (fence->GetCompletedValue() < 1) {
        HANDLE done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        require(done != nullptr, "readback fence event");
        const HRESULT wait_hr = fence->SetEventOnCompletion(1, done);
        const DWORD waited = SUCCEEDED(wait_hr) ? WaitForSingleObject(done, 5000) : WAIT_FAILED;
        cleanup(CloseHandle(done) != 0, "close readback fence event");
        checked(wait_hr, "readback fence SetEventOnCompletion");
        require(waited == WAIT_OBJECT_0, "readback fence wait timed out");
    }
    drain.armed = false;  // everything submitted has completed
    checked(host->GetLastPresentCount(&after), "final host present count");
    require(after == before + 1, "host must present exactly once");
    checkpoint(true, "one host present completed");

    stage = "pixel_readback";
    auto pixels = reinterpret_cast<const uint32_t*>(region.config.cpu_pointer);
    for (UINT i = 0; i < width * height; ++i) {
        // Mirrors fill_cs66.hlsl in shader-transpiler/crates/d3dmetal-harness/probe/shaders;
        // a change to that shader must change this formula too.
        uint32_t v = i * 0x9e3779b1u;
        uint32_t expected = (v ^ (v >> 15)) + 0x01234567u;
        if (pixels[i] != expected) {
            char detail[128];
            std::snprintf(detail, sizeof(detail), "pixel (%u,%u) expected %08x got %08x", i % width, i / width,
                          expected, pixels[i]);
            require(false, detail);
        }
    }
    const auto* xmem_bytes = static_cast<const unsigned char*>(region.view);
    require(std::all_of(xmem_bytes, xmem_bytes + Region::page_bytes,
                        [](unsigned char byte) { return byte == Region::sentinel; }),
            "xmem region bytes changed during frame readback");
    checkpoint(true, "256 pre-flip RGBA8 pixels matched in the D3D12 map; xmem region sentinel intact");
    stage = "teardown";
}
} // namespace

int main() {
    // The mismatch control is armed only by exactly "1". Any other value is a
    // configuration error rather than a silently ignored control, so a run that
    // meant to arm it cannot pass as an ordinary run.
    char inject[16];
    const DWORD inject_len =
        GetEnvironmentVariableA("BX_M2_FIRST_FRAME_INJECT_PIXEL_MISMATCH", inject, sizeof(inject));
    const bool inject_pixel_mismatch = inject_len == 1 && inject[0] == '1';
    if (inject_len != 0 && !inject_pixel_mismatch) {
        std::fprintf(stderr, "FAIL: BX_M2_FIRST_FRAME_INJECT_PIXEL_MISMATCH must be unset or exactly 1\n");
        return 1;
    }
    if (inject_pixel_mismatch) {
        std::printf("NOTE: pixel mismatch control armed; this run must fail at pixel_readback\n");
    }
    try {
        run(inject_pixel_mismatch);
    } catch (const Failure&) {
        return 1;
    }
    if (!cleanup_ok)
        return 1;
    checkpoint(true, "resources released and runtime shut down");
    return 0;
}
