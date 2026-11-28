#include <windows.h>
#include <unknwn.h>
#include <cstdint>
#include <mutex>
#include <new>

#include "bx_log.h"

namespace {
class AcpHal final : public IUnknown {
    LONG refs = 1;
public:
    HRESULT WINAPI QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid != IID_IUnknown) return E_NOINTERFACE;
        AddRef();
        *out = this;
        return S_OK;
    }
    ULONG WINAPI AddRef() override { return InterlockedIncrement(&refs); }
    ULONG WINAPI Release() override {
        ULONG left = InterlockedDecrement(&refs);
        if (!left) delete this;
        return left;
    }
};

// Sorted synthetic address ranges. Zero is reserved for failed lookups.
struct Allocation {
    uintptr_t va;
    uint32_t base, size;
    Allocation* next;
};
Allocation* allocations;
std::mutex heap_lock;
uint64_t capacity = 0;
uint64_t used = 0;
bool heap_created = false;
}

extern "C" {

HRESULT WINAPI AcpHalCreate(void** out) {
    if (!out) return E_POINTER;
    *out = new (std::nothrow) AcpHal;
    return *out ? S_OK : E_OUTOFMEMORY;
}

// NDA constraint: the public Microsoft Learn page for AcpHal is gated behind
// an Xbox developer NDA, so the true signatures and context layout are unknown
// from public sources. BetterXen therefore returns E_NOTIMPL (not the console's
// behaviour) and keeps the stub log so bx-cli trace still surfaces the call.
HRESULT WINAPI AcpHalAllocateShapeContexts(void* context) {
    (void)context;
    BX_STUB_LOG("AcpHal.dll", "AcpHalAllocateShapeContexts");
    return E_NOTIMPL;
}

HRESULT WINAPI AcpHalReleaseShapeContexts(void) {
    BX_STUB_LOG("AcpHal.dll", "AcpHalReleaseShapeContexts");
    return E_NOTIMPL;
}

HRESULT WINAPI ApuCreateHeap(uint32_t cached, uint32_t non_cached) {
    std::lock_guard guard(heap_lock);
    if (allocations) return HRESULT_FROM_WIN32(ERROR_BUSY);
    // A zero-capacity heap must not report S_OK: it can never satisfy an
    // allocation, so creating it is a caller error (#580). ApuAlloc already
    // refused such requests with E_OUTOFMEMORY.
    if (uint64_t(cached) + non_cached == 0) return E_INVALIDARG;
    // Invariant: allocations == nullptr implies used == 0; reset explicitly.
    used = 0;
    capacity = uint64_t(cached) + non_cached;
    heap_created = true;
    return S_OK;
}

HRESULT WINAPI ApuAlloc(void** va, uint32_t* apu, uint32_t size, uint32_t alignment) {
    if (va) *va = nullptr;
    if (apu) *apu = 0;
    if (!va && !apu) return E_POINTER;
    if (alignment && (alignment & (alignment - 1))) return E_INVALIDARG;
    if (alignment < 64) alignment = 64;
    if (!size) size = 64;
    std::lock_guard guard(heap_lock);
    if (!heap_created || size > capacity - used) return E_OUTOFMEMORY;
    uint64_t base = alignment;
    Allocation** link = &allocations;
    while (*link && base + size > (*link)->base) {
        const uint64_t end = uint64_t((*link)->base) + (*link)->size;
        base = (end + alignment - 1) & ~(uint64_t(alignment) - 1);
        link = &(*link)->next;
    }
    if (base + size > (uint64_t{1} << 32)) return E_OUTOFMEMORY;
    void* ptr = _aligned_malloc(size, alignment);
    if (!ptr) return E_OUTOFMEMORY;
    auto* entry = new (std::nothrow) Allocation{reinterpret_cast<uintptr_t>(ptr),
                                                static_cast<uint32_t>(base), size, *link};
    if (!entry) {
        _aligned_free(ptr);
        return E_OUTOFMEMORY;
    }
    *link = entry;
    used += size;
    if (va) *va = ptr;
    if (apu) *apu = entry->base;
    return S_OK;
}

HRESULT WINAPI ApuFree(void* va) {
    if (!va) return S_OK;
    std::lock_guard guard(heap_lock);
    for (auto** link = &allocations; *link; link = &(*link)->next) {
        auto* entry = *link;
        if (entry->va != reinterpret_cast<uintptr_t>(va)) continue;
        *link = entry->next;
        used -= entry->size;
        _aligned_free(va);
        delete entry;
        return S_OK;
    }
    return E_INVALIDARG;
}

static uint32_t lookup_va(const void* va, uint32_t size) {
    const auto address = reinterpret_cast<uintptr_t>(va);
    std::lock_guard guard(heap_lock);
    for (auto* entry = allocations; entry; entry = entry->next) {
        const auto offset = address - entry->va;
        if (offset < entry->size && size <= entry->size - offset)
            return entry->base + static_cast<uint32_t>(offset);
    }
    return 0;
}

BOOL WINAPI ApuIsVirtualAddressValid(const void* va, uint32_t size) {
    return lookup_va(va, size) != 0;
}

void* WINAPI ApuMapApuAddress(uint32_t address) {
    std::lock_guard guard(heap_lock);
    for (auto* entry = allocations; entry && entry->base <= address; entry = entry->next) {
        const auto offset = address - entry->base;
        if (offset < entry->size) return reinterpret_cast<void*>(entry->va + offset);
    }
    return nullptr;
}

uint32_t WINAPI ApuMapVirtualAddress(const void* va) { return lookup_va(va, 1); }
}
