/*
 * ADR 0009: d3d12_xs.dll hands a title an Xbox-layout device, and the objects it creates are
 * Xbox-layout too. This program calls them the way a title built against the Xbox Series D3D12
 * header does: through the object's function table, by slot number.
 *
 * The slot numbers and object offsets below were written out by hand from that header (used
 * under Microsoft's authorization). They are not taken from the shim's generated tables.
 * Expected results come from a second device created in the PC layout (Version 0) and called
 * through <d3d12.h>, not from the Xbox objects.
 */
#include <windows.h>
#include <d3d12.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

extern "C" __declspec(dllimport) HRESULT WINAPI D3D12XboxCreateDevice(PVOID adapter, const void* params, REFIID riid,
                                                                      void** ppv);

namespace {

// Xbox function-table slots (Series header).
constexpr int kQueryInterface = 0, kAddRef = 1, kRelease = 2;
constexpr int kDevGetNodeCount = 7, kDevCheckFeatureSupport = 13, kDevGetResourceAllocationInfo = 25;
constexpr int kDevCreateHeap = 28, kDevCreatePlacedResource = 29, kDevGetAdapterLuid = 43;
constexpr int kDevCreatePlacedResourceX = 44;      // first Xbox-only slot after ID3D12Device's PC methods
constexpr int kDevCreateCommittedResource1 = 92;   // ID3D12Device4; PC places it elsewhere
constexpr int kResMap = 8, kResGetDesc = 10, kResGetGpuVirtualAddress = 11;
// Object offsets (Series header): m_RefCount at +12; ID3D12Device's m_OutOfLineFlags at +24.
constexpr size_t kRefCountOffset = 12, kOutOfLineOffset = 24;

const IID kIidDevice11 = {0x5405c344, 0xd457, 0x444e, {0xb4, 0xdd, 0x23, 0x66, 0xe4, 0x5a, 0xee, 0x39}};

int g_failed = 0;
#define CHECK(cond, ...)                                          \
    do {                                                          \
        if (!(cond)) {                                            \
            ++g_failed;                                           \
            std::fprintf(stderr, "FAIL: test_d3d12_xbox_layout:%d: ", __LINE__); \
            std::fprintf(stderr, __VA_ARGS__);                    \
            std::fprintf(stderr, "\n");                           \
        }                                                         \
    } while (0)

template <typename Fn>
Fn slot(void* object, int index) {
    return reinterpret_cast<Fn>((*static_cast<void* const* const*>(object))[index]);
}

using QueryInterfaceFn = HRESULT(STDMETHODCALLTYPE*)(void*, REFIID, void**);
using RefFn = ULONG(STDMETHODCALLTYPE*)(void*);
using GetNodeCountFn = UINT(STDMETHODCALLTYPE*)(void*);
using CheckFeatureSupportFn = HRESULT(STDMETHODCALLTYPE*)(void*, D3D12_FEATURE, void*, UINT);
using GetAllocationInfoFn = D3D12_RESOURCE_ALLOCATION_INFO(STDMETHODCALLTYPE*)(void*, UINT, UINT,
                                                                              const D3D12_RESOURCE_DESC*);
using GetAdapterLuidFn = LUID(STDMETHODCALLTYPE*)(void*);
using CreateHeapFn = HRESULT(STDMETHODCALLTYPE*)(void*, const D3D12_HEAP_DESC*, REFIID, void**);
using CreatePlacedFn = HRESULT(STDMETHODCALLTYPE*)(void*, void*, UINT64, const D3D12_RESOURCE_DESC*,
                                                   D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**);
using StubFn = HRESULT(STDMETHODCALLTYPE*)(void*);
using CreateCommitted1Fn = HRESULT(STDMETHODCALLTYPE*)(void*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
                                                       const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES,
                                                       const D3D12_CLEAR_VALUE*, void*, REFIID, void**);
using MapFn = HRESULT(STDMETHODCALLTYPE*)(void*, UINT, const D3D12_RANGE*, void**);
using GetDescFn = void(STDMETHODCALLTYPE*)(void*, D3D12_RESOURCE_DESC*);
using GetGpuVaFn = D3D12_GPU_VIRTUAL_ADDRESS(STDMETHODCALLTYPE*)(void*);

D3D12_RESOURCE_DESC buffer_desc(UINT64 bytes) {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return desc;
}

std::string read_log() {
    char path[MAX_PATH];
    const DWORD n = GetEnvironmentVariableA("BX_LOG_FILE", path, sizeof(path));
    if (n == 0 || n >= sizeof(path)) return {};
    std::string text;
    if (FILE* f = std::fopen(path, "rb")) {
        char buffer[4096];
        size_t got;
        while ((got = std::fread(buffer, 1, sizeof(buffer), f)) > 0) text.append(buffer, got);
        std::fclose(f);
    }
    return text;
}

} // namespace

int main() {
    // A title sets Version (the GDK documentation asks for D3D12_SDK_VERSION); a zeroed block is
    // BetterXen's synthetic PC-layout client.
    alignas(8) unsigned char title_params[80] = {};
    const UINT version = 0x20004;
    std::memcpy(title_params, &version, sizeof(version));
    alignas(8) static const unsigned char pc_params[80] = {};

    ID3D12Device* pc = nullptr;
    HRESULT hr = D3D12XboxCreateDevice(nullptr, pc_params, __uuidof(ID3D12Device), reinterpret_cast<void**>(&pc));
    CHECK(hr == S_OK && pc, "PC-layout device hr=0x%08lx", static_cast<unsigned long>(hr));

    void* dev = nullptr;
    hr = D3D12XboxCreateDevice(nullptr, title_params, __uuidof(ID3D12Device), &dev);
    CHECK(hr == S_OK && dev, "Xbox-layout device hr=0x%08lx", static_cast<unsigned long>(hr));
    if (!dev || !pc) return 1;
    CHECK(*reinterpret_cast<void**>(dev) != *reinterpret_cast<void**>(pc), "Xbox device must not use the PC table");

    // Object layout a title can read.
    CHECK(*reinterpret_cast<uint32_t*>(static_cast<char*>(dev) + kRefCountOffset) == 1, "m_RefCount starts at 1");
    CHECK(static_cast<uint8_t*>(dev)[kOutOfLineOffset] != 0, "m_OutOfLineFlags makes inline helpers call out");

    // IUnknown slots; QueryInterface for the interface the title asks for returns the same object.
    void* dev11 = nullptr;
    CHECK(slot<QueryInterfaceFn>(dev, kQueryInterface)(dev, kIidDevice11, &dev11) == S_OK && dev11 == dev,
          "QueryInterface(ID3D12Device11) returns the Xbox device");
    CHECK(slot<RefFn>(dev, kAddRef)(dev) == 3, "AddRef counts the QueryInterface reference");
    CHECK(*reinterpret_cast<uint32_t*>(static_cast<char*>(dev) + kRefCountOffset) == 3, "m_RefCount mirrors AddRef");
    slot<RefFn>(dev, kRelease)(dev);
    slot<RefFn>(dev, kRelease)(dev);

    // Shared ID3D12Device slots, including the two struct-return conventions.
    CHECK(slot<GetNodeCountFn>(dev, kDevGetNodeCount)(dev) == pc->GetNodeCount(), "GetNodeCount at slot 7");
    D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
    CHECK(slot<CheckFeatureSupportFn>(dev, kDevCheckFeatureSupport)(dev, D3D12_FEATURE_D3D12_OPTIONS, &options,
                                                                     sizeof(options)) == S_OK,
          "CheckFeatureSupport at slot 13");
    LUID pc_luid{};
    pc->GetAdapterLuid(&pc_luid);
    const LUID luid = slot<GetAdapterLuidFn>(dev, kDevGetAdapterLuid)(dev);
    CHECK(luid.LowPart == pc_luid.LowPart && luid.HighPart == pc_luid.HighPart,
          "GetAdapterLuid (8-byte struct, returned in RAX) matches the PC device");
    const D3D12_RESOURCE_DESC desc = buffer_desc(64 * 1024);
    D3D12_RESOURCE_ALLOCATION_INFO pc_info{};
    pc->GetResourceAllocationInfo(&pc_info, 0, 1, &desc);
    const D3D12_RESOURCE_ALLOCATION_INFO info =
        slot<GetAllocationInfoFn>(dev, kDevGetResourceAllocationInfo)(dev, 0, 1, &desc);
    CHECK(info.SizeInBytes == pc_info.SizeInBytes && info.Alignment == pc_info.Alignment && info.SizeInBytes >= 65536,
          "GetResourceAllocationInfo (16-byte struct, hidden pointer) matches the PC device");

    // The first Xbox-only slot is a logged stub.
    CHECK(slot<StubFn>(dev, kDevCreatePlacedResourceX)(dev) == E_NOTIMPL, "CreatePlacedResourceX is a stub");

    // A Device4 method at its Xbox slot creates an Xbox-layout resource.
    D3D12_HEAP_PROPERTIES upload{};
    upload.Type = D3D12_HEAP_TYPE_UPLOAD;
    void* resource = nullptr;
    hr = slot<CreateCommitted1Fn>(dev, kDevCreateCommittedResource1)(dev, &upload, D3D12_HEAP_FLAG_NONE, &desc,
                                                                      D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                                      nullptr, __uuidof(ID3D12Resource), &resource);
    CHECK(hr == S_OK && resource, "CreateCommittedResource1 at slot 92 hr=0x%08lx", static_cast<unsigned long>(hr));
    if (resource) {
        D3D12_RESOURCE_DESC got{};
        slot<GetDescFn>(resource, kResGetDesc)(resource, &got);
        CHECK(got.Width == desc.Width && got.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER, "resource GetDesc at slot 10");
        CHECK(slot<GetGpuVaFn>(resource, kResGetGpuVirtualAddress)(resource) != 0, "resource GPU VA at slot 11");
        void* mapped = nullptr;
        CHECK(slot<MapFn>(resource, kResMap)(resource, 0, nullptr, &mapped) == S_OK && mapped, "resource Map at slot 8");
        if (mapped) static_cast<unsigned char*>(mapped)[0] = 0x5a;
        CHECK(slot<RefFn>(resource, kRelease)(resource) == 0, "resource Release at slot 2");
    }

    // An Xbox heap passed back into a forwarded call is unwrapped to its host.
    D3D12_HEAP_DESC heap_desc{};
    heap_desc.SizeInBytes = 64 * 1024;
    heap_desc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap_desc.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
    void* heap = nullptr;
    hr = slot<CreateHeapFn>(dev, kDevCreateHeap)(dev, &heap_desc, __uuidof(ID3D12Heap), &heap);
    CHECK(hr == S_OK && heap, "CreateHeap at slot 28 hr=0x%08lx", static_cast<unsigned long>(hr));
    if (heap) {
        void* placed = nullptr;
        hr = slot<CreatePlacedFn>(dev, kDevCreatePlacedResource)(dev, heap, 0, &desc, D3D12_RESOURCE_STATE_COMMON,
                                                                 nullptr, __uuidof(ID3D12Resource), &placed);
        CHECK(hr == S_OK && placed, "CreatePlacedResource on an Xbox heap hr=0x%08lx", static_cast<unsigned long>(hr));
        if (placed) slot<RefFn>(placed, kRelease)(placed);
        slot<RefFn>(heap, kRelease)(heap);
    }

    CHECK(slot<RefFn>(dev, kRelease)(dev) == 0, "device Release at slot 2");
    pc->Release();

    const std::string log = read_log();
    if (!log.empty()) {
        CHECK(log.find("device=xbox_function_tables") != std::string::npos, "log records the Xbox layout");
        CHECK(log.find("XboxDevice[44]::CreatePlacedResourceX") != std::string::npos, "the stub slot is logged");
    }
    if (g_failed) return 1;
    std::printf("PASS: test_d3d12_xbox_layout\n");
    return 0;
}
