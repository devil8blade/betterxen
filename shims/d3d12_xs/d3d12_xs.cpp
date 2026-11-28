#include "proxy.h"
#include "vmem.h"
#include "xbox_facade.h"

#include <cstdio>
#include <cstring>
#include <mutex>

/*
 * Track C (M1-C1, #414): Xbox Series D3D12 entry points over the host's d3d12.dll
 * (Wine's d3d12.dll, which CrossOver backs with D3DMetal or another translation layer).
 *
 * D3D12XboxCreateDevice creates a host device with D3D12CreateDevice and returns an
 * auto-logging COM proxy around it (see proxy.h). Root signature serialization and
 * deserialization forward directly to the host d3d12.dll.
 */

namespace {

HMODULE get_host_d3d12() {
    static HMODULE mod = LoadLibraryA("d3d12.dll");
    return mod;
}

void initialize_trace_flag() {
    static std::once_flag initialized;
    std::call_once(initialized, [] {
        const bx_runtime_config_t* cfg = bx_get_runtime_config();
        g_bx_trace_d3d = (cfg && cfg->trace_d3d) ? 1u : 0u;
    });
}

} // namespace

extern "C" {

/*
 * The documented four-parameter form (authorized GDK documentation, d3d12_x.h / d3d12_xs.h):
 * an optional adapter, a required D3D12XBOX_CREATE_DEVICE_PARAMETERS block, the interface ID and
 * the device out pointer. A NULL parameter block fails with E_INVALIDARG, as documented.
 *
 * BetterXen has no adapter objects, so a non-NULL adapter is logged and the default host adapter
 * is used (an inference, not a documented fallback). Of the parameter block only `Version`, its
 * first UINT, is read and logged; the other fields are not interpreted. The host device is
 * created at the lowest feature level D3D12 accepts. `riid` must name an interface the proxy
 * models (or one the host device answers); anything else fails with E_NOINTERFACE and a logged
 * IID.
 */
HRESULT WINAPI D3D12XboxCreateDevice(PVOID adapter, const void* params, REFIID riid, void** ppv) {
    if (!ppv) {
        return E_POINTER;
    }
    *ppv = nullptr;
    initialize_trace_flag();
    if (!params) {
        bx_log_emit(BX_LOG_WARN, "d3d12_xs", "D3D12XboxCreateDevice", "refused: NULL parameter block (E_INVALIDARG)");
        return E_INVALIDARG;
    }
    UINT version = 0;
    std::memcpy(&version, params, sizeof(version));

    using CreateFn = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
    HMODULE d3d12 = get_host_d3d12();
    auto create = d3d12
        ? reinterpret_cast<CreateFn>(reinterpret_cast<void*>(GetProcAddress(d3d12, "D3D12CreateDevice")))
        : nullptr;
    if (!create) {
        bx_log_emit(BX_LOG_ERROR, "d3d12_xs", "host_d3d12_missing", "d3d12.dll!D3D12CreateDevice not available");
        return E_NOINTERFACE;
    }

    ID3D12Device* host = nullptr;
    char reuse[8] = {};
    /* Test hook. While the backing cache holds a host heap, that heap keeps the host
       device alive, and a real runtime then returns the same ID3D12Device. Set the
       variable so the next proxy wraps that device even if this host does not. */
    const bool reuse_backing = GetEnvironmentVariableA("BX_D3D12_REUSE_BACKING_HOST", reuse, sizeof(reuse)) > 0 &&
                               reuse[0] == '1' && reuse[1] == '\0';
    if (reuse_backing) {
        host = static_cast<ID3D12Device*>(bx_vmem_addref_backing_host());
    }
    HRESULT hr = S_OK;
    if (!host) {
        hr = create(nullptr, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), reinterpret_cast<void**>(&host));
        if (FAILED(hr) || !host) {
            char detail[96];
            std::snprintf(detail, sizeof(detail), "D3D12CreateDevice hr=0x%08lx", static_cast<unsigned long>(hr));
            bx_log_emit(BX_LOG_ERROR, "d3d12_xs", "host_device_failed", detail);
            return FAILED(hr) ? hr : E_FAIL;
        }
    }

    BxDeviceProxy* proxy = BxDeviceProxy::create(host);
    host->Release();
    if (!proxy) {
        return E_OUTOFMEMORY;
    }
    if (!bx_vmem_install_device_hooks(proxy)) {
        proxy->Release();
        return E_FAIL;
    }

    /* Record how far up the ID3D12DeviceN chain the host goes: QueryInterface on the proxy stops there. */
    int highest = 0;
    ID3D12Device* const levels[] = {
        proxy->h_ID3D12Device1, proxy->h_ID3D12Device2, proxy->h_ID3D12Device3, proxy->h_ID3D12Device4,
        proxy->h_ID3D12Device5, proxy->h_ID3D12Device6, proxy->h_ID3D12Device7, proxy->h_ID3D12Device8,
        proxy->h_ID3D12Device9, proxy->h_ID3D12Device10,
    };
    for (int i = 0; i < 10; ++i) {
        if (levels[i]) highest = i + 1;
    }
    char detail[256];
    std::snprintf(detail, sizeof(detail),
                  "host_device=%p highest_host_interface=ID3D12Device%d feature_level=11_0 params_version=%u "
                  "(other fields not interpreted) adapter=%s trace_d3d=%u",
                  static_cast<void*>(proxy->h_ID3D12Device), highest, version,
                  adapter ? "non-null (ignored, default host adapter)" : "default", g_bx_trace_d3d);
    bx_log_emit(BX_LOG_INFO, "d3d12_xs", "D3D12XboxCreateDevice", detail);

    /*
     * ADR 0009: a title built against the Xbox header calls through Xbox-order function tables,
     * so it gets an Xbox-layout device over the PC proxy. The documentation requires titles to
     * set Version (D3D12_SDK_VERSION); BetterXen's synthetic fixtures, which are PC-layout
     * clients built against <d3d12.h>, pass a zeroed block and keep the PC proxy.
     */
    if (version != 0 && bx_xbox_iface_for(riid) == &bx_xbox_Device) {
        void* pc = nullptr;
        hr = proxy->QueryInterface(riid, &pc);
        proxy->Release();
        if (FAILED(hr) || !pc) return FAILED(hr) ? hr : E_NOINTERFACE;
        *ppv = bx_xbox_wrap(riid, pc);
        bx_log_emit(BX_LOG_INFO, "d3d12_xs", "xbox_layout", *ppv ? "device=xbox_function_tables" : "device=wrap_failed");
        return *ppv ? S_OK : E_OUTOFMEMORY;
    }
    hr = proxy->QueryInterface(riid, ppv);
    proxy->Release();
    if (SUCCEEDED(hr)) {
        bx_log_emit(BX_LOG_INFO, "d3d12_xs", "xbox_layout",
                    version == 0 ? "device=pc_layout (params Version 0: synthetic PC-layout client)"
                                 : "device=pc_layout (interface has no Xbox table)");
    }
    return hr;
}

HRESULT WINAPI D3D12ConfigureVirtualMemoryX(PVOID config) {
    return bx_d3d12_configure_virtual_memory(config);
}

HRESULT WINAPI D3D12SerializeVersionedRootSignature(
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* desc,
    ID3DBlob** blob,
    ID3DBlob** error_blob
) {
    using Fn = HRESULT(WINAPI*)(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC*, ID3DBlob**, ID3DBlob**);
    HMODULE d3d12 = get_host_d3d12();
    if (d3d12) {
        auto fn = reinterpret_cast<Fn>(reinterpret_cast<void*>(
            GetProcAddress(d3d12, "D3D12SerializeVersionedRootSignature")));
        if (fn) {
            return fn(desc, blob, error_blob);
        }
    }
    BX_STUB_LOG("d3d12_xs.dll", "D3D12SerializeVersionedRootSignature");
    return E_NOTIMPL;
}

HRESULT WINAPI D3D12CreateRootSignatureDeserializer(
    LPCVOID src_data,
    SIZE_T src_size,
    REFIID riid,
    void** out_deserializer
) {
    using Fn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, REFIID, void**);
    HMODULE d3d12 = get_host_d3d12();
    if (d3d12) {
        auto fn = reinterpret_cast<Fn>(reinterpret_cast<void*>(
            GetProcAddress(d3d12, "D3D12CreateRootSignatureDeserializer")));
        if (fn) {
            return fn(src_data, src_size, riid, out_deserializer);
        }
    }
    BX_STUB_LOG("d3d12_xs.dll", "D3D12CreateRootSignatureDeserializer");
    return E_NOTIMPL;
}

} // extern "C"
