/*
 * Resources placed at a title-chosen GPU virtual address (ADR 0009, "translation by copy").
 *
 * On the console a title maps its own physical pages at an address and places resources there
 * with ID3D12Device::CreatePlacedResourceX / CreatePlacedResourceX1; CPU and GPU see the same
 * memory. D3DMetal cannot create a resource over existing memory (the existing-memory imports
 * measured for #473 return E_NOTIMPL), so each such resource gets a shadow resource on the host:
 *
 *  - a buffer gets a committed buffer in a CPU-visible write-back heap, filled from the title's
 *    memory when it is created;
 *  - a texture gets a committed texture whose contents are not filled (logged); texture
 *    contents need the console tiling translation (xg_xs) first.
 *
 * The title's address and the shadow are recorded, and the resource reports the title's address
 * from GetGPUVirtualAddress. Keeping the shadow in step with later CPU writes, and translating
 * title addresses in GPU calls, are not done yet; both are logged when reached.
 */
#include "xbox_facade.h"

#include <cstdio>
#include <cstring>
#include <mutex>

namespace {

struct Shadow {
    ID3D12Resource* host;          // not owned: the Xbox object owns the reference
    uint64_t title_va;
    uint64_t size;
    bool buffer;
};

std::mutex g_lock;
Shadow* g_shadows = nullptr;
size_t g_count = 0;
size_t g_capacity = 0;

bool remember(const Shadow& shadow) {
    std::lock_guard<std::mutex> guard(g_lock);
    if (g_count == g_capacity) {
        const size_t capacity = g_capacity ? g_capacity * 2 : 64;
        auto* grown = new (std::nothrow) Shadow[capacity];
        if (!grown) return false;
        if (g_count) std::memcpy(grown, g_shadows, g_count * sizeof(Shadow));
        delete[] g_shadows;
        g_shadows = grown;
        g_capacity = capacity;
    }
    g_shadows[g_count++] = shadow;
    return true;
}

bool readable(const void* p, uint64_t size) {
    auto cursor = reinterpret_cast<uintptr_t>(p);
    const uintptr_t end = cursor + size;
    while (cursor < end) {
        MEMORY_BASIC_INFORMATION region{};
        if (!VirtualQuery(reinterpret_cast<void*>(cursor), &region, sizeof(region)) || region.State != MEM_COMMIT
            || (region.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
            return false;
        }
        cursor = reinterpret_cast<uintptr_t>(region.BaseAddress) + region.RegionSize;
    }
    return true;
}

HRESULT create_shadow(void* self, uint64_t title_va, const D3D12_RESOURCE_DESC& desc, REFIID riid, void** ppv,
                      const char* method) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    auto* device = bx_xbox_host<ID3D12Device>(self);
    const bool buffer = desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER;
    D3D12_HEAP_PROPERTIES heap{};
    if (buffer) {
        heap.Type = D3D12_HEAP_TYPE_CUSTOM;
        heap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_BACK;
        heap.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
    } else {
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    }
    D3D12_RESOURCE_DESC host_desc = desc;
    if (buffer) host_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* resource = nullptr;
    HRESULT hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &host_desc, D3D12_RESOURCE_STATE_COMMON,
                                                 nullptr, __uuidof(ID3D12Resource), reinterpret_cast<void**>(&resource));
    char detail[224];
    std::snprintf(detail, sizeof(detail), "%s title_va=0x%llx dim=%d width=%llu height=%u format=%d hr=0x%08lx",
                  method, static_cast<unsigned long long>(title_va), static_cast<int>(desc.Dimension),
                  static_cast<unsigned long long>(desc.Width), desc.Height, static_cast<int>(desc.Format),
                  static_cast<unsigned long>(hr));
    bx_log_emit(SUCCEEDED(hr) ? BX_LOG_INFO : BX_LOG_WARN, "d3d12_xs", "placed_x_shadow", detail);
    if (FAILED(hr) || !resource) return FAILED(hr) ? hr : E_FAIL;

    const auto* title = reinterpret_cast<const void*>(title_va);
    if (buffer) {
        void* mapped = nullptr;
        if (SUCCEEDED(resource->Map(0, nullptr, &mapped)) && mapped) {
            if (readable(title, desc.Width)) {
                std::memcpy(mapped, title, static_cast<size_t>(desc.Width));
            } else {
                bx_log_emit(BX_LOG_WARN, "d3d12_xs", "placed_x_shadow", "title memory not readable; shadow left zeroed");
            }
            resource->Unmap(0, nullptr);
        }
    } else {
        BX_STUB_LOG("d3d12_xs.dll", "CreatePlacedResourceX(texture contents)");
    }
    if (!remember({resource, title_va, desc.Width, buffer})) {
        resource->Release();
        return E_OUTOFMEMORY;
    }
    *ppv = resource;
    return bx_xbox_wrap_result(S_OK, riid, ppv);
}

} // namespace

extern "C" {

HRESULT STDMETHODCALLTYPE bx_xbox_device_create_placed_resource_x(void* self, D3D12_GPU_VIRTUAL_ADDRESS location,
                                                                  const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES,
                                                                  const D3D12_CLEAR_VALUE*, REFIID riid, void** ppv) {
    BX_D3D_TRACE("XboxDevice", "CreatePlacedResourceX");
    if (!desc || !location) return E_INVALIDARG;
    return create_shadow(self, location, *desc, riid, ppv, "CreatePlacedResourceX");
}

HRESULT STDMETHODCALLTYPE bx_xbox_device_create_placed_resource_x1(void* self, D3D12_GPU_VIRTUAL_ADDRESS location,
                                                                   const D3D12_RESOURCE_DESC1* desc1, UINT,
                                                                   const D3D12_CLEAR_VALUE*, REFIID riid, void** ppv) {
    BX_D3D_TRACE("XboxDevice", "CreatePlacedResourceX1");
    if (!desc1 || !location) return E_INVALIDARG;
    // D3D12_RESOURCE_DESC1 is D3D12_RESOURCE_DESC followed by SamplerFeedbackMipRegion.
    D3D12_RESOURCE_DESC desc{};
    std::memcpy(&desc, desc1, sizeof(desc));
    return create_shadow(self, location, desc, riid, ppv, "CreatePlacedResourceX1");
}

void bx_xbox_placed_forget(IUnknown* host) {
    std::lock_guard<std::mutex> guard(g_lock);
    for (size_t i = 0; i < g_count; ++i) {
        if (static_cast<IUnknown*>(g_shadows[i].host) == host) {
            g_shadows[i] = g_shadows[--g_count];
            return;
        }
    }
}

D3D12_GPU_VIRTUAL_ADDRESS STDMETHODCALLTYPE bx_xbox_resource_get_gpu_virtual_address(void* self) {
    BX_D3D_TRACE("XboxResource", "GetGPUVirtualAddress");
    auto* host = bx_xbox_host<ID3D12Resource>(self);
    {
        std::lock_guard<std::mutex> guard(g_lock);
        for (size_t i = 0; i < g_count; ++i) {
            if (g_shadows[i].host == host) return g_shadows[i].title_va;
        }
    }
    return host->GetGPUVirtualAddress();
}

} // extern "C"
