#ifndef BETTERXEN_D3D12_XS_XBOX_FACADE_H
#define BETTERXEN_D3D12_XS_XBOX_FACADE_H

/*
 * Xbox-layout objects in front of the PC proxies (ADR 0009).
 *
 * A title built against the Xbox Series D3D12 header calls every method through the object's
 * function table, whose slot order differs from PC D3D12. A BxXboxObject has the Xbox object
 * layout and an Xbox-order table (xbox_tables_generated.cpp); each slot forwards to the PC
 * object it hosts, which is usually one of the PC proxies in proxy.h.
 *
 * Layout (from the Xbox header): the function-table pointer, a 32-bit flags word and the
 * reference count make up every object's 16-byte base. ID3D12Device adds a 64-bit null constant
 * buffer address at +16 and a 64-byte union at +24 whose first byte, m_OutOfLineFlags, makes
 * inline helpers call out through the table when non-zero. The same layout is used for every
 * interface family; the fields after the base are ignored by the others. Command lists add a
 * put buffer and are not wrapped yet.
 */
#include "proxy.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>

struct BxXboxIface {
    const char* name;              // Xbox leaf interface, for logs
    const IID* iids;               // Xbox interface IDs this table answers
    size_t iid_count;
    const IID* host_iid;           // PC interface the hosted object provides
    const void* const* table;      // Xbox-order function table
    size_t slots;
};

struct BxXboxObject {
    const void* const* function_table;
    uint32_t xbox_flags;
    uint32_t xbox_ref_count;       // kept equal to `refs` for anything reading the Xbox field
    uint64_t device_null_cb_address;
    uint8_t device_out_of_line[64];
    // BetterXen state, after every field the Xbox layout defines.
    IUnknown* host;                // owns one reference; typed as iface->host_iid
    const BxXboxIface* iface;
    std::atomic<ULONG> refs;
};
static_assert(offsetof(BxXboxObject, xbox_ref_count) == 12, "Xbox m_RefCount offset");
static_assert(offsetof(BxXboxObject, device_null_cb_address) == 16, "Xbox device null CB address offset");
static_assert(offsetof(BxXboxObject, device_out_of_line) == 24, "Xbox device m_OutOfLineFlags offset");

/* The Xbox table for `riid`, or nullptr when none of the wrapped families answers it. */
const BxXboxIface* bx_xbox_iface_for(REFIID riid);

/* Wraps `host` (consumes the caller's reference) in the Xbox object for `riid`. One Xbox object
   exists per host object and family. When no family answers `riid`, `host` is returned as is
   and that is logged. */
void* bx_xbox_wrap(REFIID riid, void* host);

/* After a (REFIID, void**) creation call: wraps a successful result. */
HRESULT bx_xbox_wrap_result(HRESULT hr, REFIID riid, void** ppv);

/* The hosted PC object when `p` is one of our Xbox objects, otherwise `p` itself. */
IUnknown* bx_xbox_unwrap(const void* p);
template <typename T>
T* bx_xbox_unwrap_as(T* p) {
    return static_cast<T*>(static_cast<void*>(bx_xbox_unwrap(p)));
}
template <typename T>
const T* bx_xbox_unwrap_as(const T* p) {
    return static_cast<const T*>(static_cast<void*>(bx_xbox_unwrap(p)));
}

template <typename T>
T* bx_xbox_host(void* self) {
    return static_cast<T*>(static_cast<BxXboxObject*>(self)->host);
}

/* Unwraps an array of interface pointers for one forwarded call. */
template <typename T>
class BxXboxUnwrapArray {
public:
    BxXboxUnwrapArray(T* const* items, UINT count) : count_(count) {
        if (!items || count == 0) {
            ok_ = true;
            return;
        }
        data_ = count <= kInline ? inline_ : new (std::nothrow) T*[count];
        ok_ = data_ != nullptr;
        for (UINT i = 0; ok_ && i < count; ++i) data_[i] = bx_xbox_unwrap_as(items[i]);
    }
    ~BxXboxUnwrapArray() {
        if (data_ && data_ != inline_) delete[] data_;
    }
    BxXboxUnwrapArray(const BxXboxUnwrapArray&) = delete;
    BxXboxUnwrapArray& operator=(const BxXboxUnwrapArray&) = delete;
    bool ok() const { return ok_; }
    T* const* data() const { return data_; }

private:
    static constexpr UINT kInline = 16;
    T* inline_[kInline] = {};
    T** data_ = nullptr;
    UINT count_ = 0;
    bool ok_ = false;
};

extern "C" {
HRESULT STDMETHODCALLTYPE bx_xbox_query_interface(void* self, REFIID riid, void** ppv);
ULONG STDMETHODCALLTYPE bx_xbox_add_ref(void* self);
ULONG STDMETHODCALLTYPE bx_xbox_release(void* self);
void STDMETHODCALLTYPE bx_xbox_get_device(void* self, REFIID riid, void** ppv);

/* Xbox ID3D12Device::CreateCommandQueueX: the Xbox queue description selects a hardware
   engine/pipe and queue index; the PC queue gets the same type and flags at normal priority. */
struct BxXboxCommandQueueDesc {
    D3D12_COMMAND_LIST_TYPE Type;
    D3D12_COMMAND_QUEUE_FLAGS Flags;
    UINT EngineOrPipeIndex;
    UINT QueueIndex;
};
HRESULT STDMETHODCALLTYPE bx_xbox_device_create_command_queue_x(void* self, const BxXboxCommandQueueDesc* desc,
                                                                REFIID riid, void** ppv);
}

#include "xbox_tables_generated.h"

#endif
