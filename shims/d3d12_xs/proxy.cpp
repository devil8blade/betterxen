#include "proxy.h"
#include "vmem.h"

#include <cstdio>
#include <cstring>
#include <new>
#include <type_traits>
#include <vector>

#include "presentx.h"

#include "shader_gate.h"

/* {6B1D5C0E-2F4A-4E0B-9A57-3C0E5D7B1A42}: private to this DLL, never handed to a host object. */
const GUID IID_BxProxyHost = {0x6b1d5c0e, 0x2f4a, 0x4e0b, {0x9a, 0x57, 0x3c, 0x0e, 0x5d, 0x7b, 0x1a, 0x42}};

uint32_t g_bx_trace_d3d = 0;

void bx_d3d_trace_call(const char* iface, const char* method) {
    char detail[160];
    std::snprintf(detail, sizeof(detail), "%s::%s", iface, method);
    bx_log_emit(BX_LOG_INFO, "d3d12_xs", "call", detail);
}

void bx_format_guid(REFGUID guid, char* out, size_t out_len) {
    std::snprintf(out, out_len, "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                  static_cast<unsigned long>(guid.Data1), guid.Data2, guid.Data3,
                  guid.Data4[0], guid.Data4[1], guid.Data4[2], guid.Data4[3],
                  guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7]);
}

void* bx_unwrap_raw(IUnknown* obj) {
    if (!obj) {
        return nullptr;
    }
    void* host = nullptr;
    if (obj->QueryInterface(IID_BxProxyHost, &host) == S_OK && host) {
        return host; /* our proxy: by contract this answer carries no reference */
    }
    return obj;
}

namespace {

/*
 * Shared tail of QueryInterface for an IID the proxy does not model. The host is asked so that
 * host-internal and newer interfaces keep working, but the pointer it returns is NOT proxied:
 * that is logged every time so the gap is visible in `bx-cli trace`.
 */
HRESULT query_host_unproxied(IUnknown* host, const char* leaf, REFIID riid, void** ppv) {
    char guid[40];
    bx_format_guid(riid, guid, sizeof(guid));
    char detail[160];
    HRESULT hr = host ? host->QueryInterface(riid, ppv) : E_NOINTERFACE;
    if (hr == S_OK && *ppv) {
        std::snprintf(detail, sizeof(detail), "%s iid=%s served by host object without proxy", leaf, guid);
        bx_log_emit(BX_LOG_WARN, "d3d12_xs", "qi_unproxied", detail);
        return S_OK;
    }
    *ppv = nullptr;
    std::snprintf(detail, sizeof(detail), "%s iid=%s hr=0x%08lx", leaf, guid, static_cast<unsigned long>(hr));
    bx_log_emit(BX_LOG_WARN, "d3d12_xs", "qi_unsupported", detail);
    return FAILED(hr) ? hr : E_NOINTERFACE;
}

/* Copies `count` object pointers into `out`, replacing our proxies with their host objects. */
template <typename T>
T** unwrap_array(UINT count, T* const* in, T** stack, UINT stack_cap) {
    T** out = stack;
    if (count > stack_cap) {
        out = static_cast<T**>(HeapAlloc(GetProcessHeap(), 0, sizeof(T*) * count));
        if (!out) {
            return nullptr;
        }
    }
    for (UINT i = 0; i < count; ++i) {
        out[i] = bx_unwrap(in[i]);
    }
    return out;
}

template <typename T>
void free_unwrapped(T** arr, T** stack) {
    if (arr && arr != stack) {
        HeapFree(GetProcessHeap(), 0, arr);
    }
}

constexpr UINT kStackObjects = 64;

/* ---- Shader container metadata (public DXBC container layout) ------------------------------ */

uint32_t read_u32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

void put_fourcc(char* dst, const uint8_t* src) {
    for (int i = 0; i < 4; ++i) {
        uint8_t ch = src[i];
        bool ok = (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch == '_';
        dst[i] = ok ? static_cast<char>(ch) : '?';
    }
    dst[4] = '\0';
}

/*
 * Logs what kind of container a shader stage carries: size, magic and (for DXBC) the chunk FourCCs,
 * so the M2 shader-format gate can tell DXIL from other payloads. Nothing is modified or copied.
 *
 * Returns false when a non-empty DXBC container is malformed or has no shader program chunk
 * (DXIL/SHEX/SHDR), so we return E_INVALIDARG instead of forwarding a container that crashes
 * D3DMetal 3.0.
 */
bool log_shader(const char* pso_kind, const char* stage, const D3D12_SHADER_BYTECODE& code) {
    if (!code.pShaderBytecode || code.BytecodeLength == 0) {
        return true;
    }
    auto* bytes = static_cast<const uint8_t*>(code.pShaderBytecode);
    size_t len = code.BytecodeLength;
    char magic[5] = "none";
    if (len >= 4) {
        put_fourcc(magic, bytes);
    }
    char chunks[128] = "";
    uint32_t chunk_count = 0;
    bool well_formed = false;
    bool is_dxbc = len >= 4 && std::memcmp(bytes, "DXBC", 4) == 0;
    bool has_shader_chunk = false;
    /* DXBC header: magic[4] digest[16] version[4] total_size[4] chunk_count[4] chunk_offsets[4*n]. */
    if (is_dxbc && len >= 32) {
        uint32_t declared = read_u32(bytes + 24);
        chunk_count = read_u32(bytes + 28);
        well_formed = declared >= 32 && declared <= len && chunk_count <= (declared - 32) / 4;
        size_t used = 0;
        for (uint32_t i = 0; well_formed && i < chunk_count; ++i) {
            uint32_t off = read_u32(bytes + 32 + 4 * static_cast<size_t>(i));
            if (off > declared || declared - off < 8) {
                well_formed = false;
                break;
            }
            /* ACCC counts too: the shader format gate (#466) decides what happens to it. */
            if (std::memcmp(bytes + off, "DXIL", 4) == 0 ||
                std::memcmp(bytes + off, "SHEX", 4) == 0 ||
                std::memcmp(bytes + off, "SHDR", 4) == 0 ||
                std::memcmp(bytes + off, "ACCC", 4) == 0) {
                has_shader_chunk = true;
            }
            if (i < 16) {
                char cc[5];
                put_fourcc(cc, bytes + off);
                int n = std::snprintf(chunks + used, sizeof(chunks) - used, "%s%s", used ? "," : "", cc);
                if (n > 0 && static_cast<size_t>(n) < sizeof(chunks) - used) {
                    used += static_cast<size_t>(n);
                }
            }
        }
    }
    char detail[320];
    std::snprintf(detail, sizeof(detail), "pso=%s stage=%s bytes=%llu magic=%s well_formed=%d chunk_count=%lu chunks=%s",
                  pso_kind, stage, static_cast<unsigned long long>(len), magic, well_formed ? 1 : 0,
                  static_cast<unsigned long>(chunk_count), chunks);
    bx_log_emit(BX_LOG_INFO, "d3d12_xs", "pso_shader", detail);
    return !is_dxbc || (well_formed && has_shader_chunk);
}

void log_created(const char* what, HRESULT hr) {
    char detail[96];
    std::snprintf(detail, sizeof(detail), "%s hr=0x%08lx", what, static_cast<unsigned long>(hr));
    bx_log_emit(FAILED(hr) ? BX_LOG_WARN : BX_LOG_INFO, "d3d12_xs", "create", detail);
}

} // namespace

/* ---- Device ----------------------------------------------------------------------------------- */

BxDeviceProxy* BxDeviceProxy::create(ID3D12Device* host) {
    auto* self = new (std::nothrow) BxDeviceProxy();
    if (!self) {
        return nullptr;
    }
    self->bx_acquire_hosts(host);
    if (!self->h_ID3D12Device) {
        self->bx_release_hosts();
        delete self;
        return nullptr;
    }
    return self;
}

HRESULT STDMETHODCALLTYPE BxDeviceProxy::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) {
        return E_POINTER;
    }
    if (IsEqualGUID(riid, IID_BxProxyHost)) {
        *ppv = h_ID3D12Device;
        return S_OK;
    }
    if (bx_match_iid(riid)) {
        AddRef();
        *ppv = static_cast<ID3D12Device10*>(this);
        return S_OK;
    }
    return query_host_unproxied(h_ID3D12Device, kLeafName, riid, ppv);
}

ULONG STDMETHODCALLTYPE BxDeviceProxy::AddRef() {
    return refs_.fetch_add(1, std::memory_order_relaxed) + 1;
}

bool BxDeviceProxy::bx_try_acquire_if_nonzero() {
    ULONG cur = refs_.load(std::memory_order_acquire);
    while (cur != 0) {
        if (refs_.compare_exchange_weak(cur, cur + 1, std::memory_order_acq_rel, std::memory_order_acquire)) {
            return true;
        }
    }
    return false;
}

bool BxDeviceProxy::bx_try_release_last() {
    ULONG expected = 1;
    return refs_.compare_exchange_strong(expected, 0, std::memory_order_acq_rel, std::memory_order_acquire);
}

ULONG STDMETHODCALLTYPE BxDeviceProxy::Release() {
    /* The 1->0 transition and a weak GetDevice acquire share g_section.
       GetDevice adds a reference only when the count is non-zero, so a proxy
       at zero is never brought back. Exactly one Release performs final cleanup. */
    ULONG cur = refs_.load(std::memory_order_acquire);
    for (;;) {
        if (cur == 0) {
            return 0;
        }
        if (cur > 1) {
            if (refs_.compare_exchange_weak(cur, cur - 1, std::memory_order_acq_rel, std::memory_order_acquire)) {
                return cur - 1;
            }
            continue;
        }
        if (bx_vmem_final_release(this)) {
            bx_release_hosts();
            delete this;
            return 0;
        }
        cur = refs_.load(std::memory_order_acquire);
    }
}

HRESULT BxDeviceProxy::wrap_child(IUnknown* host_child, const char* what, REFIID riid, void** ppv) {
    HRESULT hr = E_OUTOFMEMORY;
    IUnknown* wrapper = nullptr;
    void* probe = nullptr;
    if (host_child->QueryInterface(__uuidof(ID3D12CommandQueue), &probe) == S_OK) {
        static_cast<IUnknown*>(probe)->Release();
        wrapper = static_cast<ID3D12CommandQueue*>(BxCommandQueueProxy::create(host_child, this));
    } else if (host_child->QueryInterface(__uuidof(ID3D12CommandAllocator), &probe) == S_OK) {
        static_cast<IUnknown*>(probe)->Release();
        wrapper = static_cast<ID3D12CommandAllocator*>(BxCommandAllocatorProxy::create(host_child, this));
    } else if (host_child->QueryInterface(__uuidof(ID3D12GraphicsCommandList), &probe) == S_OK) {
        static_cast<IUnknown*>(probe)->Release();
        wrapper = static_cast<ID3D12GraphicsCommandList*>(BxCommandListProxy::create(host_child, this));
    } else {
        /* For example a video or other non-graphics command list: hand the host object out, visibly. */
        char guid[40];
        bx_format_guid(riid, guid, sizeof(guid));
        char detail[160];
        std::snprintf(detail, sizeof(detail), "%s iid=%s is not a proxied interface family; host object returned", what, guid);
        bx_log_emit(BX_LOG_WARN, "d3d12_xs", "child_unproxied", detail);
        return host_child->QueryInterface(riid, ppv);
    }
    if (wrapper) {
        hr = wrapper->QueryInterface(riid, ppv);
        wrapper->Release();
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE BxDeviceProxy::CreateCommandQueue(const D3D12_COMMAND_QUEUE_DESC* desc, REFIID riid, void** ppv) {
    BX_D3D_TRACE("ID3D12Device", "CreateCommandQueue");
    if (!ppv) {
        return h_ID3D12Device->CreateCommandQueue(desc, riid, nullptr);
    }
    *ppv = nullptr;
    IUnknown* host_child = nullptr;
    HRESULT hr = h_ID3D12Device->CreateCommandQueue(desc, __uuidof(IUnknown), reinterpret_cast<void**>(&host_child));
    if (hr == S_OK && host_child) {
        hr = wrap_child(host_child, "CreateCommandQueue", riid, ppv);
        host_child->Release();
    }
    log_created("ID3D12CommandQueue", hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE BxDeviceProxy::CreateCommandQueue1(const D3D12_COMMAND_QUEUE_DESC* desc, REFIID creator_id, REFIID riid, void** ppv) {
    BX_D3D_TRACE("ID3D12Device9", "CreateCommandQueue1");
    if (!h_ID3D12Device9) {
        BX_VTABLE_STUB_LOG("ID3D12Device9", 75u, "CreateCommandQueue1");
        if (ppv) *ppv = nullptr;
        return E_NOTIMPL;
    }
    if (!ppv) {
        return h_ID3D12Device9->CreateCommandQueue1(desc, creator_id, riid, nullptr);
    }
    *ppv = nullptr;
    IUnknown* host_child = nullptr;
    HRESULT hr = h_ID3D12Device9->CreateCommandQueue1(desc, creator_id, __uuidof(IUnknown), reinterpret_cast<void**>(&host_child));
    if (hr == S_OK && host_child) {
        hr = wrap_child(host_child, "CreateCommandQueue1", riid, ppv);
        host_child->Release();
    }
    log_created("ID3D12CommandQueue (CreateCommandQueue1)", hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE BxDeviceProxy::CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE type, REFIID riid, void** ppv) {
    BX_D3D_TRACE("ID3D12Device", "CreateCommandAllocator");
    if (!ppv) {
        return h_ID3D12Device->CreateCommandAllocator(type, riid, nullptr);
    }
    *ppv = nullptr;
    IUnknown* host_child = nullptr;
    HRESULT hr = h_ID3D12Device->CreateCommandAllocator(type, __uuidof(IUnknown), reinterpret_cast<void**>(&host_child));
    if (hr == S_OK && host_child) {
        hr = wrap_child(host_child, "CreateCommandAllocator", riid, ppv);
        host_child->Release();
    }
    log_created("ID3D12CommandAllocator", hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE BxDeviceProxy::CreateCommandList(UINT node_mask, D3D12_COMMAND_LIST_TYPE type, ID3D12CommandAllocator* allocator,
                                                           ID3D12PipelineState* initial_state, REFIID riid, void** ppv) {
    BX_D3D_TRACE("ID3D12Device", "CreateCommandList");
    if (!ppv) {
        return h_ID3D12Device->CreateCommandList(node_mask, type, bx_unwrap(allocator), initial_state, riid, nullptr);
    }
    *ppv = nullptr;
    IUnknown* host_child = nullptr;
    HRESULT hr = h_ID3D12Device->CreateCommandList(node_mask, type, bx_unwrap(allocator), initial_state,
                                                   __uuidof(IUnknown), reinterpret_cast<void**>(&host_child));
    if (hr == S_OK && host_child) {
        hr = wrap_child(host_child, "CreateCommandList", riid, ppv);
        host_child->Release();
    }
    log_created("ID3D12GraphicsCommandList", hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE BxDeviceProxy::CreateCommandList1(UINT node_mask, D3D12_COMMAND_LIST_TYPE type, D3D12_COMMAND_LIST_FLAGS flags,
                                                            REFIID riid, void** ppv) {
    BX_D3D_TRACE("ID3D12Device4", "CreateCommandList1");
    if (!h_ID3D12Device4) {
        BX_VTABLE_STUB_LOG("ID3D12Device4", 51u, "CreateCommandList1");
        if (ppv) *ppv = nullptr;
        return E_NOTIMPL;
    }
    if (!ppv) {
        return h_ID3D12Device4->CreateCommandList1(node_mask, type, flags, riid, nullptr);
    }
    *ppv = nullptr;
    IUnknown* host_child = nullptr;
    HRESULT hr = h_ID3D12Device4->CreateCommandList1(node_mask, type, flags, __uuidof(IUnknown), reinterpret_cast<void**>(&host_child));
    if (hr == S_OK && host_child) {
        hr = wrap_child(host_child, "CreateCommandList1", riid, ppv);
        host_child->Release();
    }
    log_created("ID3D12GraphicsCommandList (CreateCommandList1)", hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE BxDeviceProxy::CreateGraphicsPipelineState(const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc, REFIID riid, void** ppv) {
    BX_D3D_TRACE("ID3D12Device", "CreateGraphicsPipelineState");
    bool shaders_ok = true;
    if (desc) {
        shaders_ok &= log_shader("graphics", "VS", desc->VS);
        shaders_ok &= log_shader("graphics", "PS", desc->PS);
        shaders_ok &= log_shader("graphics", "DS", desc->DS);
        shaders_ok &= log_shader("graphics", "HS", desc->HS);
        shaders_ok &= log_shader("graphics", "GS", desc->GS);
    }
    if (!shaders_ok) {
        if (ppv) *ppv = nullptr;
        log_created("graphics pipeline state", E_INVALIDARG);
        return E_INVALIDARG;
    }
    if (!desc) {
        HRESULT hr = h_ID3D12Device->CreateGraphicsPipelineState(desc, riid, ppv);
        log_created("graphics pipeline state", hr);
        return hr;
    }
    /* M2 shader format gate (#466): every stage is classified; an ACCC-only stage fails the PSO. */
    BxShaderGate gate;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC admitted = *desc;
    HRESULT gated = gate.admit("graphics", "VS", desc->VS, admitted.VS);
    if (SUCCEEDED(gated)) gated = gate.admit("graphics", "PS", desc->PS, admitted.PS);
    if (SUCCEEDED(gated)) gated = gate.admit("graphics", "DS", desc->DS, admitted.DS);
    if (SUCCEEDED(gated)) gated = gate.admit("graphics", "HS", desc->HS, admitted.HS);
    if (SUCCEEDED(gated)) gated = gate.admit("graphics", "GS", desc->GS, admitted.GS);
    if (FAILED(gated)) {
        if (ppv) *ppv = nullptr;
        log_created("graphics pipeline state", gated);
        return gated;
    }
    HRESULT hr = h_ID3D12Device->CreateGraphicsPipelineState(&admitted, riid, ppv);
    log_created("graphics pipeline state", hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE BxDeviceProxy::CreateComputePipelineState(const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc, REFIID riid, void** ppv) {
    BX_D3D_TRACE("ID3D12Device", "CreateComputePipelineState");
    if (desc && !log_shader("compute", "CS", desc->CS)) {
        if (ppv) *ppv = nullptr;
        log_created("compute pipeline state", E_INVALIDARG);
        return E_INVALIDARG;
    }
    if (!desc) {
        HRESULT hr = h_ID3D12Device->CreateComputePipelineState(desc, riid, ppv);
        log_created("compute pipeline state", hr);
        return hr;
    }
    /* M2 shader format gate (#466): DXIL is passed on, an ACCC-only container is refused. */
    BxShaderGate gate;
    D3D12_COMPUTE_PIPELINE_STATE_DESC admitted = *desc;
    const HRESULT gated = gate.admit("compute", "CS", desc->CS, admitted.CS);
    if (FAILED(gated)) {
        if (ppv) *ppv = nullptr;
        log_created("compute pipeline state", gated);
        return gated;
    }
    HRESULT hr = h_ID3D12Device->CreateComputePipelineState(&admitted, riid, ppv);
    log_created("compute pipeline state", hr);
    return hr;
}

/* Stream PSO subobjects (#576). A stream is a sequence of 8-byte-aligned records, each a
 * subobject type and a type-dependent payload. The real layout aligns the record, not the
 * payload: a payload whose type is only 4-byte aligned starts at offset 4, not 8. Measured
 * with the shim's own toolchain, that is the case for BLEND, SAMPLE_MASK, RASTERIZER,
 * DEPTH_STENCIL, DEPTH_STENCIL1, IB_STRIP_CUT_VALUE, PRIMITIVE_TOPOLOGY,
 * RENDER_TARGET_FORMATS, DEPTH_STENCIL_FORMAT, SAMPLE_DESC, NODE_MASK and FLAGS. Deriving
 * both the payload offset and the record size from the compiler keeps this table honest. */
template<class Payload>
struct alignas(void*) StreamRecord {
    /* alignas is on the CLASS, as in CD3DX12_PIPELINE_STATE_STREAM_SUBOBJECT: it pads the
     * whole record to 8 bytes but does not move a 4-byte-aligned payload off offset 4. */
    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type;
    Payload payload;
};
template<class Payload>
constexpr size_t payload_at() { return offsetof(StreamRecord<Payload>, payload); }
template<class Payload>
constexpr size_t record_size() {
    return (offsetof(StreamRecord<Payload>, payload) + sizeof(Payload) + 7) & ~static_cast<size_t>(7);
}

struct StreamSubobject {
    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type;
    size_t payload_offset;
    size_t record_size;
    const char* stage; /* non-null only for a shader subobject */
};

#define BX_SUBOBJECT(tag, type, stage) \
    {tag, payload_at<type>(), record_size<type>(), stage}
constexpr StreamSubobject kStreamSubobjects[] = {
    BX_SUBOBJECT(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, void*, nullptr),
    BX_SUBOBJECT(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS, D3D12_SHADER_BYTECODE, "VS"),
    BX_SUBOBJECT(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, D3D12_SHADER_BYTECODE, "PS"),
    BX_SUBOBJECT(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS, D3D12_SHADER_BYTECODE, "DS"),
    BX_SUBOBJECT(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS, D3D12_SHADER_BYTECODE, "HS"),
    BX_SUBOBJECT(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS, D3D12_SHADER_BYTECODE, "GS"),
    BX_SUBOBJECT(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS, D3D12_SHADER_BYTECODE, "CS"),
    BX_SUBOBJECT(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT, D3D12_STREAM_OUTPUT_DESC, nullptr),
    BX_SUBOBJECT(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND, D3D12_BLEND_DESC, nullptr),
    BX_SUBOBJECT(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK, UINT, nullptr),
    BX_SUBOBJECT(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, D3D12_RASTERIZER_DESC, nullptr),
    BX_SUBOBJECT(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL, D3D12_DEPTH_STENCIL_DESC, nullptr),
    BX_SUBOBJECT(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT, D3D12_INPUT_LAYOUT_DESC, nullptr),
    BX_SUBOBJECT(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE, UINT, nullptr),
    BX_SUBOBJECT(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY, UINT, nullptr),
    BX_SUBOBJECT(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, D3D12_RT_FORMAT_ARRAY, nullptr),
    BX_SUBOBJECT(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT, DXGI_FORMAT, nullptr),
    BX_SUBOBJECT(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC, DXGI_SAMPLE_DESC, nullptr),
    BX_SUBOBJECT(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK, UINT, nullptr),
    BX_SUBOBJECT(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO, D3D12_CACHED_PIPELINE_STATE, nullptr),
    BX_SUBOBJECT(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS, UINT, nullptr),
    BX_SUBOBJECT(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1, D3D12_DEPTH_STENCIL_DESC1, nullptr),
    BX_SUBOBJECT(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING, D3D12_VIEW_INSTANCING_DESC, nullptr),
    BX_SUBOBJECT(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS, D3D12_SHADER_BYTECODE, "AS"),
    BX_SUBOBJECT(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS, D3D12_SHADER_BYTECODE, "MS"),
};
#undef BX_SUBOBJECT

/* Pin the layout facts the walker depends on, measured with this toolchain. A header change
 * that moves a payload must fail to compile rather than desync the walk (#576 review). */
static_assert(payload_at<D3D12_SHADER_BYTECODE>() == 8, "shader payloads sit at offset 8");
static_assert(record_size<D3D12_SHADER_BYTECODE>() == 24, "a shader record is 24 bytes");
static_assert(payload_at<UINT>() == 4, "4-byte payloads sit at offset 4, not 8");
static_assert(record_size<UINT>() == 8, "a UINT record is 8 bytes");
static_assert(payload_at<D3D12_BLEND_DESC>() == 4, "BLEND payload sits at offset 4");
static_assert(payload_at<D3D12_RASTERIZER_DESC>() == 4, "RASTERIZER payload sits at offset 4");
static_assert(payload_at<D3D12_RT_FORMAT_ARRAY>() == 4, "RT_FORMAT_ARRAY payload sits at offset 4");
static_assert(payload_at<D3D12_DEPTH_STENCIL_DESC1>() == 4, "DEPTH_STENCIL1 payload sits at offset 4");
static_assert(payload_at<void*>() == 8 && record_size<void*>() == 16, "root signature record");
static_assert(payload_at<D3D12_INPUT_LAYOUT_DESC>() == 8, "INPUT_LAYOUT payload sits at offset 8");

const StreamSubobject* find_subobject(uint32_t type) {
    for (const StreamSubobject& entry : kStreamSubobjects) {
        if (static_cast<uint32_t>(entry.type) == type) return &entry;
    }
    return nullptr;
}

/* Copies the caller's stream into `copy` so the gate can substitute prepared bytecode without
 * touching memory it does not own, and records where each shader payload sits. An unknown tag,
 * or a payload that overruns SizeInBytes, is rejected. */
HRESULT copy_stream(const D3D12_PIPELINE_STATE_STREAM_DESC& desc, std::vector<uint8_t>& copy,
                    std::vector<size_t>& stage_offsets, std::vector<const char*>& stage_names) {
    if (desc.SizeInBytes == 0 || !desc.pPipelineStateSubobjectStream) return E_INVALIDARG;
    const auto* source = static_cast<const uint8_t*>(desc.pPipelineStateSubobjectStream);
    copy.assign(source, source + desc.SizeInBytes);
    size_t offset = 0;
    while (offset < copy.size()) {
        if (copy.size() - offset < sizeof(SIZE_T)) return E_INVALIDARG;
        /* The tag is a 4-byte enum at the start of an 8-byte-aligned record; the rest of
         * those 8 bytes is padding a caller may leave uninitialized, so read only the enum
         * and step to the payload at offset sizeof(SIZE_T). */
        uint32_t raw = 0;
        std::memcpy(&raw, &copy[offset], sizeof(raw));
        if (raw >= static_cast<uint32_t>(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MAX_VALID)) return E_INVALIDARG;
        const StreamSubobject* entry = find_subobject(raw);
        if (!entry) return E_INVALIDARG;
        /* Microsoft's ID3D12Device2 rules reject a duplicated subobject type. */
        for (size_t seen = 0; seen < offset; ) {
            uint32_t prior = 0;
            std::memcpy(&prior, &copy[seen], sizeof(prior));
            if (prior == raw) return E_INVALIDARG;
            const StreamSubobject* prior_entry = find_subobject(prior);
            if (!prior_entry) return E_INVALIDARG;
            seen += prior_entry->record_size;
        }
        const size_t payload = offset + entry->payload_offset;
        const size_t record = entry->record_size;
        if (copy.size() - offset < record) return E_INVALIDARG;
        if (entry->stage) {
            D3D12_SHADER_BYTECODE code{};
            std::memcpy(&code, &copy[payload], sizeof(code));
            if (code.BytecodeLength != 0 && !code.pShaderBytecode) return E_INVALIDARG;
            stage_offsets.push_back(payload);
            stage_names.push_back(entry->stage);
        }
        offset += record;
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE BxDeviceProxy::CreatePipelineState(const D3D12_PIPELINE_STATE_STREAM_DESC* desc, REFIID riid, void** ppv) {
    BX_D3D_TRACE("ID3D12Device2", "CreatePipelineState");
    if (!h_ID3D12Device2) {
        BX_VTABLE_STUB_LOG("ID3D12Device2", 47u, "CreatePipelineState");
        if (ppv) *ppv = nullptr;
        return E_NOTIMPL;
    }
    if (ppv) *ppv = nullptr;
    if (!desc) {
        bx_log_emit(BX_LOG_INFO, "d3d12_xs", "pso_stream", "pso=stream bytes=0 shaders_not_parsed=1");
        const HRESULT hr = h_ID3D12Device2->CreatePipelineState(desc, riid, ppv);
        log_created("stream pipeline state", hr);
        return hr;
    }

    /* M2 shader format gate (#576): the stream form classifies its stages like the desc forms,
     * so an ACCC-only container is refused instead of reaching D3DMetal unparsed. */
    std::vector<uint8_t> stream;
    std::vector<size_t> offsets;
    std::vector<const char*> names;
    const HRESULT copied = copy_stream(*desc, stream, offsets, names);
    {
        char detail[96];
        std::snprintf(detail, sizeof(detail), "pso=stream bytes=%llu shaders_not_parsed=%d",
                      static_cast<unsigned long long>(desc->SizeInBytes), offsets.empty() ? 1 : 0);
        bx_log_emit(BX_LOG_INFO, "d3d12_xs", "pso_stream", detail);
    }
    if (FAILED(copied) || offsets.empty()) {
        const HRESULT hr = FAILED(copied) ? copied : h_ID3D12Device2->CreatePipelineState(desc, riid, ppv);
        log_created("stream pipeline state", hr);
        return hr;
    }

    BxShaderGate gate;
    HRESULT gated = S_OK;
    for (size_t i = 0; i < offsets.size() && SUCCEEDED(gated); ++i) {
        D3D12_SHADER_BYTECODE in{};
        std::memcpy(&in, &stream[offsets[i]], sizeof(in));
        D3D12_SHADER_BYTECODE out = in;
        gated = gate.admit("stream", names[i], in, out);
        /* `out` is either the caller's bytes or the gate's copy, which lives until `gate`
         * is destroyed, so only the pointer/length inside our own copy change. */
        if (SUCCEEDED(gated)) std::memcpy(&stream[offsets[i]], &out, sizeof(out));
    }
    if (FAILED(gated)) {
        log_created("stream pipeline state", gated);
        return gated;
    }
    D3D12_PIPELINE_STATE_STREAM_DESC admitted = *desc;
    admitted.pPipelineStateSubobjectStream = stream.data();
    const HRESULT hr = h_ID3D12Device2->CreatePipelineState(&admitted, riid, ppv);
    log_created("stream pipeline state", hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE BxDeviceProxy::MakeResident(UINT count, ID3D12Pageable* const* objects) {
    BX_D3D_TRACE("ID3D12Device", "MakeResident");
    ID3D12Pageable* stack[kStackObjects];
    ID3D12Pageable** hosts = objects ? unwrap_array(count, objects, stack, kStackObjects) : nullptr;
    if (objects && !hosts) return E_OUTOFMEMORY;
    HRESULT hr = h_ID3D12Device->MakeResident(count, hosts);
    free_unwrapped(hosts, stack);
    return hr;
}

HRESULT STDMETHODCALLTYPE BxDeviceProxy::Evict(UINT count, ID3D12Pageable* const* objects) {
    BX_D3D_TRACE("ID3D12Device", "Evict");
    ID3D12Pageable* stack[kStackObjects];
    ID3D12Pageable** hosts = objects ? unwrap_array(count, objects, stack, kStackObjects) : nullptr;
    if (objects && !hosts) return E_OUTOFMEMORY;
    HRESULT hr = h_ID3D12Device->Evict(count, hosts);
    free_unwrapped(hosts, stack);
    return hr;
}

HRESULT STDMETHODCALLTYPE BxDeviceProxy::EnqueueMakeResident(D3D12_RESIDENCY_FLAGS flags, UINT count, ID3D12Pageable* const* objects,
                                                             ID3D12Fence* fence, UINT64 fence_value) {
    BX_D3D_TRACE("ID3D12Device3", "EnqueueMakeResident");
    if (!h_ID3D12Device3) {
        BX_VTABLE_STUB_LOG("ID3D12Device3", 50u, "EnqueueMakeResident");
        return E_NOTIMPL;
    }
    ID3D12Pageable* stack[kStackObjects];
    ID3D12Pageable** hosts = objects ? unwrap_array(count, objects, stack, kStackObjects) : nullptr;
    if (objects && !hosts) return E_OUTOFMEMORY;
    HRESULT hr = h_ID3D12Device3->EnqueueMakeResident(flags, count, hosts, fence, fence_value);
    free_unwrapped(hosts, stack);
    return hr;
}

HRESULT STDMETHODCALLTYPE BxDeviceProxy::SetResidencyPriority(UINT count, ID3D12Pageable* const* objects,
                                                              const D3D12_RESIDENCY_PRIORITY* priorities) {
    BX_D3D_TRACE("ID3D12Device1", "SetResidencyPriority");
    if (!h_ID3D12Device1) {
        BX_VTABLE_STUB_LOG("ID3D12Device1", 46u, "SetResidencyPriority");
        return E_NOTIMPL;
    }
    ID3D12Pageable* stack[kStackObjects];
    ID3D12Pageable** hosts = objects ? unwrap_array(count, objects, stack, kStackObjects) : nullptr;
    if (objects && !hosts) return E_OUTOFMEMORY;
    HRESULT hr = h_ID3D12Device1->SetResidencyPriority(count, hosts, priorities);
    free_unwrapped(hosts, stack);
    return hr;
}

/* ---- Device children (queue, allocator, list) ------------------------------------------------- */

template <typename Gen>
BxChildProxy<Gen>* BxChildProxy<Gen>::create(IUnknown* host, BxDeviceProxy* device) {
    auto* self = new (std::nothrow) BxChildProxy<Gen>();
    if (!self) {
        return nullptr;
    }
    self->bx_acquire_hosts(host);
    host->QueryInterface(__uuidof(IUnknown), reinterpret_cast<void**>(&self->host_identity_));
    self->device_ = device;
    device->AddRef();
    return self;
}

template <typename Gen>
HRESULT STDMETHODCALLTYPE BxChildProxy<Gen>::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) {
        return E_POINTER;
    }
    if (IsEqualGUID(riid, IID_BxProxyHost)) {
        *ppv = host_identity_;
        return S_OK;
    }
    if (this->bx_match_iid(riid)) {
        AddRef();
        *ppv = static_cast<Gen*>(this);
        return S_OK;
    }
    if constexpr (std::is_same_v<Gen, BxCommandQueueProxyImpl>) {
        if (IsEqualGUID(riid, IID_IBxPresentQueue)) {
            return bx_present_query_queue(this, ppv); /* BetterXen's presentation bridge, presentx.h */
        }
    }
    return query_host_unproxied(host_identity_, Gen::kLeafName, riid, ppv);
}

template <typename Gen>
ULONG STDMETHODCALLTYPE BxChildProxy<Gen>::AddRef() {
    return refs_.fetch_add(1, std::memory_order_relaxed) + 1;
}

template <typename Gen>
ULONG STDMETHODCALLTYPE BxChildProxy<Gen>::Release() {
    ULONG left = refs_.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (left == 0) {
        this->bx_release_hosts();
        if (host_identity_) host_identity_->Release();
        BxDeviceProxy* device = device_;
        delete this;
        if (device) device->Release();
    }
    return left;
}

template <typename Gen>
HRESULT STDMETHODCALLTYPE BxChildProxy<Gen>::GetDevice(REFIID riid, void** ppv) {
    BX_D3D_TRACE("ID3D12DeviceChild", "GetDevice");
    if (!ppv) {
        return E_POINTER;
    }
    return device_->QueryInterface(riid, ppv); /* the proxy device, never the host device */
}

void STDMETHODCALLTYPE BxCommandQueueProxyImpl::ExecuteCommandLists(UINT count, ID3D12CommandList* const* lists) {
    BX_D3D_TRACE("ID3D12CommandQueue", "ExecuteCommandLists");
    ID3D12CommandList* stack[kStackObjects];
    ID3D12CommandList** hosts = lists ? unwrap_array(count, lists, stack, kStackObjects) : nullptr;
    if (lists && !hosts) {
        bx_log_emit(BX_LOG_ERROR, "d3d12_xs", "execute_command_lists_dropped", "out of memory unwrapping command lists");
        return;
    }
    h_ID3D12CommandQueue->ExecuteCommandLists(count, hosts);
    free_unwrapped(hosts, stack);
}

template class BxChildProxy<BxCommandQueueProxyImpl>;
template class BxChildProxy<BxCommandAllocatorProxyGen>;
template class BxChildProxy<BxCommandListProxyGen>;
