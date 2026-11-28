#include "xbox_facade.h"

#include <cstdio>
#include <cstring>
#include <mutex>

/*
 * ADR 0009: Xbox-layout objects over the PC proxies. See xbox_facade.h for the layout and
 * xbox_tables_generated.cpp (tools/gen_xbox_tables.py) for the slot order.
 */

namespace {

constexpr uint8_t kOutOfLineCommon = 0x1;   // Xbox OutOfLineFlags_Common

struct Entry {
    IUnknown* host;
    const BxXboxIface* iface;
    BxXboxObject* object;
};

std::mutex g_lock;
Entry* g_entries = nullptr;
size_t g_count = 0;
size_t g_capacity = 0;

bool same_iid(REFIID a, REFIID b) {
    return std::memcmp(&a, &b, sizeof(IID)) == 0;
}

bool answers(const BxXboxIface* iface, REFIID riid) {
    for (size_t i = 0; i < iface->iid_count; ++i) {
        if (same_iid(iface->iids[i], riid)) return true;
    }
    return false;
}

const BxXboxIface* iface_of_object(const void* p) {
    if (!p) return nullptr;
    const void* table = *static_cast<const void* const*>(p);
    for (size_t i = 0; i < bx_xbox_iface_count; ++i) {
        if (bx_xbox_ifaces[i]->table == table) return bx_xbox_ifaces[i];
    }
    return nullptr;
}

void log_iid(const char* event, const char* what, REFIID riid) {
    char guid[40];
    bx_format_guid(riid, guid, sizeof(guid));
    char detail[160];
    std::snprintf(detail, sizeof(detail), "%s iid=%s", what, guid);
    bx_log_emit(BX_LOG_WARN, "d3d12_xs", event, detail);
}

BxXboxObject* find_locked(IUnknown* host, const BxXboxIface* iface) {
    for (size_t i = 0; i < g_count; ++i) {
        if (g_entries[i].host == host && g_entries[i].iface == iface) return g_entries[i].object;
    }
    return nullptr;
}

bool insert_locked(Entry entry) {
    if (g_count == g_capacity) {
        const size_t capacity = g_capacity ? g_capacity * 2 : 64;
        auto* grown = new (std::nothrow) Entry[capacity];
        if (!grown) return false;
        if (g_count) std::memcpy(grown, g_entries, g_count * sizeof(Entry));
        delete[] g_entries;
        g_entries = grown;
        g_capacity = capacity;
    }
    g_entries[g_count++] = entry;
    return true;
}

void erase_locked(BxXboxObject* object) {
    for (size_t i = 0; i < g_count; ++i) {
        if (g_entries[i].object == object) {
            g_entries[i] = g_entries[--g_count];
            return;
        }
    }
}

} // namespace

const BxXboxIface* bx_xbox_iface_for(REFIID riid) {
    for (size_t i = 0; i < bx_xbox_iface_count; ++i) {
        if (answers(bx_xbox_ifaces[i], riid)) return bx_xbox_ifaces[i];
    }
    return nullptr;
}

void* bx_xbox_wrap(REFIID riid, void* raw) {
    if (!raw) return nullptr;
    auto* unknown = static_cast<IUnknown*>(raw);
    const BxXboxIface* iface = bx_xbox_iface_for(riid);
    if (!iface) {
        log_iid("xbox_unwrapped", "no Xbox table answers this interface; returning the PC object", riid);
        return raw;
    }
    void* typed = nullptr;
    if (FAILED(unknown->QueryInterface(*iface->host_iid, &typed)) || !typed) {
        log_iid("xbox_unwrapped", iface->name, *iface->host_iid);
        return raw;
    }
    unknown->Release();
    auto* host = static_cast<IUnknown*>(typed);

    std::lock_guard<std::mutex> guard(g_lock);
    if (BxXboxObject* existing = find_locked(host, iface)) {
        ULONG refs = existing->refs.load();
        while (refs != 0 && !existing->refs.compare_exchange_weak(refs, refs + 1)) {
        }
        if (refs != 0) {
            existing->xbox_ref_count = refs + 1;
            host->Release();
            return existing;
        }
    }
    auto* object = new (std::nothrow) BxXboxObject{};
    if (!object) {
        host->Release();
        return nullptr;
    }
    object->function_table = iface->table;
    object->xbox_ref_count = 1;
    if (iface == &bx_xbox_Device) {
        object->xbox_fields[kBxXboxDeviceOutOfLineOffset - offsetof(BxXboxObject, xbox_fields)] = kOutOfLineCommon;
    }
    object->host = host;
    object->iface = iface;
    object->refs.store(1);
    if (!insert_locked({host, iface, object})) {
        host->Release();
        delete object;
        return nullptr;
    }
    return object;
}

HRESULT bx_xbox_wrap_result(HRESULT hr, REFIID riid, void** ppv) {
    if (SUCCEEDED(hr) && ppv && *ppv) {
        void* wrapped = bx_xbox_wrap(riid, *ppv);
        if (!wrapped) {
            *ppv = nullptr;
            return E_OUTOFMEMORY;
        }
        *ppv = wrapped;
    }
    return hr;
}

IUnknown* bx_xbox_unwrap(const void* p) {
    if (iface_of_object(p)) return static_cast<const BxXboxObject*>(p)->host;
    return static_cast<IUnknown*>(const_cast<void*>(p));
}

extern "C" {

HRESULT STDMETHODCALLTYPE bx_xbox_query_interface(void* self, REFIID riid, void** ppv) {
    auto* object = static_cast<BxXboxObject*>(self);
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (same_iid(riid, IID_BxProxyHost)) {
        *ppv = object->host;   // by contract, no reference (see bx_unwrap_raw)
        return S_OK;
    }
    if (same_iid(riid, __uuidof(IUnknown)) || answers(object->iface, riid)) {
        bx_xbox_add_ref(self);
        *ppv = self;
        return S_OK;
    }
    void* raw = nullptr;
    HRESULT hr = object->host->QueryInterface(riid, &raw);
    if (FAILED(hr) || !raw) return hr;
    if (bx_xbox_iface_for(riid)) {
        *ppv = bx_xbox_wrap(riid, raw);
        return *ppv ? S_OK : E_OUTOFMEMORY;
    }
    log_iid("xbox_qi_unwrapped", object->iface->name, riid);
    *ppv = raw;
    return S_OK;
}

ULONG STDMETHODCALLTYPE bx_xbox_add_ref(void* self) {
    auto* object = static_cast<BxXboxObject*>(self);
    const ULONG refs = object->refs.fetch_add(1) + 1;
    object->xbox_ref_count = refs;
    return refs;
}

ULONG STDMETHODCALLTYPE bx_xbox_release(void* self) {
    auto* object = static_cast<BxXboxObject*>(self);
    std::unique_lock<std::mutex> guard(g_lock);
    const ULONG refs = object->refs.fetch_sub(1) - 1;
    object->xbox_ref_count = refs;
    if (refs != 0) return refs;
    erase_locked(object);
    guard.unlock();
    object->host->Release();
    delete object;
    return 0;
}

void STDMETHODCALLTYPE bx_xbox_get_device(void* self, REFIID riid, void** ppv) {
    if (!ppv) return;
    *ppv = nullptr;
    auto* child = bx_xbox_host<ID3D12DeviceChild>(self);
    void* device = nullptr;
    if (SUCCEEDED(child->GetDevice(riid, &device)) && device) *ppv = bx_xbox_wrap(riid, device);
}

HRESULT STDMETHODCALLTYPE bx_xbox_device_create_command_queue_x(void* self, const BxXboxCommandQueueDesc* desc,
                                                                REFIID riid, void** ppv) {
    BX_D3D_TRACE("XboxDevice", "CreateCommandQueueX");
    if (!desc || !ppv) return E_INVALIDARG;
    *ppv = nullptr;
    D3D12_COMMAND_QUEUE_DESC pc{};
    pc.Type = desc->Type;
    pc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    pc.Flags = desc->Flags;
    char detail[160];
    std::snprintf(detail, sizeof(detail), "type=%d flags=0x%x engine_or_pipe=%u queue=%u (indices not used by the host)",
                  static_cast<int>(desc->Type), static_cast<unsigned>(desc->Flags), desc->EngineOrPipeIndex,
                  desc->QueueIndex);
    bx_log_emit(BX_LOG_INFO, "d3d12_xs", "CreateCommandQueueX", detail);
    const HRESULT hr = bx_xbox_host<ID3D12Device>(self)->CreateCommandQueue(&pc, riid, ppv);
    return bx_xbox_wrap_result(hr, riid, ppv);
}

HRESULT STDMETHODCALLTYPE bx_xbox_device_create_command_list_x(void* self, const BxXboxCommandListDesc* desc,
                                                               ID3D12CommandAllocator* allocator,
                                                               ID3D12PipelineState* initial_state, REFIID riid,
                                                               void** ppv) {
    BX_D3D_TRACE("XboxDevice", "CreateCommandListX");
    if (!desc || !ppv) return E_INVALIDARG;
    *ppv = nullptr;
    char detail[160];
    std::snprintf(detail, sizeof(detail), "type=%d xbox_flags=0x%x node_mask=0x%x barrier_batch=%u (flags and batch not used by the host)",
                  static_cast<int>(desc->Type), desc->Flags, desc->NodeMask, desc->ResourceBarrierBatchSize);
    bx_log_emit(BX_LOG_INFO, "d3d12_xs", "CreateCommandListX", detail);
    const HRESULT hr = bx_xbox_host<ID3D12Device>(self)->CreateCommandList(
        desc->NodeMask, desc->Type, bx_xbox_unwrap_as(allocator), bx_xbox_unwrap_as(initial_state), riid, ppv);
    return bx_xbox_wrap_result(hr, riid, ppv);
}

UINT STDMETHODCALLTYPE bx_xbox_list_resource_barrier(void* self, UINT count, const D3D12_RESOURCE_BARRIER* barriers) {
    BX_D3D_TRACE("XboxGraphicsCommandList", "ResourceBarrier");
    if (!barriers || count == 0) return 0;
    constexpr UINT kInline = 32;
    D3D12_RESOURCE_BARRIER inline_copy[kInline];
    D3D12_RESOURCE_BARRIER* copy = count <= kInline ? inline_copy : new (std::nothrow) D3D12_RESOURCE_BARRIER[count];
    if (!copy) return 0;
    for (UINT i = 0; i < count; ++i) {
        copy[i] = barriers[i];
        switch (copy[i].Type) {
        case D3D12_RESOURCE_BARRIER_TYPE_TRANSITION:
            copy[i].Transition.pResource = bx_xbox_unwrap_as(copy[i].Transition.pResource);
            break;
        case D3D12_RESOURCE_BARRIER_TYPE_ALIASING:
            copy[i].Aliasing.pResourceBefore = bx_xbox_unwrap_as(copy[i].Aliasing.pResourceBefore);
            copy[i].Aliasing.pResourceAfter = bx_xbox_unwrap_as(copy[i].Aliasing.pResourceAfter);
            break;
        case D3D12_RESOURCE_BARRIER_TYPE_UAV:
            copy[i].UAV.pResource = bx_xbox_unwrap_as(copy[i].UAV.pResource);
            break;
        default:
            break;
        }
    }
    bx_xbox_host<ID3D12GraphicsCommandList>(self)->ResourceBarrier(count, copy);
    if (copy != inline_copy) delete[] copy;
    return 0;
}

void STDMETHODCALLTYPE bx_xbox_list_copy_texture_region(void* self, const D3D12_TEXTURE_COPY_LOCATION* dst, UINT x,
                                                        UINT y, UINT z, const D3D12_TEXTURE_COPY_LOCATION* src,
                                                        const D3D12_BOX* box) {
    BX_D3D_TRACE("XboxGraphicsCommandList", "CopyTextureRegion");
    D3D12_TEXTURE_COPY_LOCATION d{}, s{};
    if (dst) {
        d = *dst;
        d.pResource = bx_xbox_unwrap_as(d.pResource);
    }
    if (src) {
        s = *src;
        s.pResource = bx_xbox_unwrap_as(s.pResource);
    }
    bx_xbox_host<ID3D12GraphicsCommandList>(self)->CopyTextureRegion(dst ? &d : nullptr, x, y, z, src ? &s : nullptr, box);
}

} // extern "C"
