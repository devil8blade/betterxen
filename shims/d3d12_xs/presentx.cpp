#include "presentx.h"

#include <atomic>
#include <cstdio>
#include <new>

#include "proxy.h"

/*
 * Track C (M2-C2, #465). See presentx.h for the contract and for what this is not.
 * Host DXGI is loaded on demand, like the host d3d12.dll in d3d12_xs.cpp.
 */

namespace {

constexpr HRESULT kWaitTimedOut = static_cast<HRESULT>(0x800705B4u); /* HRESULT_FROM_WIN32(ERROR_TIMEOUT) */

bool is_device_removed_hr(HRESULT hr) {
    return hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_HUNG ||
           hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_DRIVER_INTERNAL_ERROR;
}

void log_hr(bx_log_level_t level, const char* event, const char* what, HRESULT hr) {
    char detail[160];
    std::snprintf(detail, sizeof(detail), "%s hr=0x%08lx", what, static_cast<unsigned long>(hr));
    bx_log_emit(level, "d3d12_xs", event, detail);
}

/* Releases an already acquired lock and records its owner, so re-entry can be refused. */
class SrwOwner {
public:
    SrwOwner(SRWLOCK& lock, std::atomic<DWORD>& owner) : lock_(lock), owner_(owner) { owner_ = GetCurrentThreadId(); }
    ~SrwOwner() {
        owner_ = 0;
        ReleaseSRWLockExclusive(&lock_);
    }
    SrwOwner(const SrwOwner&) = delete;
    SrwOwner& operator=(const SrwOwner&) = delete;

private:
    SRWLOCK& lock_;
    std::atomic<DWORD>& owner_;
};

class BxPresentSwapChain final : public IBxPresentSwapChain {
public:
    /* `queue_proxy` is referenced for the lifetime of the chain; `host_queue` is its host object. */
    static HRESULT create(ID3D12CommandQueue* queue_proxy, ID3D12CommandQueue* host_queue, HWND window,
                          const BxPresentDesc& desc, IBxPresentSwapChain** out);

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    HRESULT STDMETHODCALLTYPE GetBuffer(UINT index, REFIID riid, void** ppv) override;
    UINT STDMETHODCALLTYPE GetCurrentBackBufferIndex() override;
    HRESULT STDMETHODCALLTYPE Present(UINT sync_interval, UINT flags) override;
    HRESULT STDMETHODCALLTYPE WaitForIdle(DWORD timeout_ms) override;
    HRESULT STDMETHODCALLTYPE GetHostSwapChain(REFIID riid, void** ppv) override;

private:
    BxPresentSwapChain() = default;
    ~BxPresentSwapChain() = default;

    HRESULT init(HWND window, const BxPresentDesc& desc);
    HRESULT signal_locked(UINT64* value);
    HRESULT wait_locked(UINT64 value, ULONGLONG start, DWORD timeout_ms);
    HRESULT idle_locked(ULONGLONG start, DWORD timeout_ms);
    HRESULT post_present_failure(const char* event, const char* what, HRESULT hr, HRESULT live_device_result);
    HRESULT removed_reason();
    void destroy();

    std::atomic<ULONG> refs_{1};
    SRWLOCK lock_ = SRWLOCK_INIT; /* guards the fence counter, the event and Present */
    ID3D12CommandQueue* queue_proxy_ = nullptr;
    ID3D12CommandQueue* host_queue_ = nullptr;
    HWND window_ = nullptr;
    IDXGISwapChain3* swap_ = nullptr;
    ID3D12Fence* fence_ = nullptr;
    HANDLE event_ = nullptr;
    std::atomic<DWORD> owner_{0}; /* thread holding lock_ */
    UINT64 last_signaled_ = 0;
    UINT64 presented_ = 0;
    bool needs_idle_ = false; /* post-present signal or wait failed; only an idle queue clears this */
    UINT64 buffer_value_[DXGI_MAX_SWAP_CHAIN_BUFFERS] = {};
};

HRESULT BxPresentSwapChain::create(ID3D12CommandQueue* queue_proxy, ID3D12CommandQueue* host_queue, HWND window,
                                   const BxPresentDesc& desc, IBxPresentSwapChain** out) {
    auto* self = new (std::nothrow) BxPresentSwapChain();
    if (!self) {
        return E_OUTOFMEMORY;
    }
    queue_proxy->AddRef();
    self->queue_proxy_ = queue_proxy;
    host_queue->AddRef();
    self->host_queue_ = host_queue;
    self->window_ = window;

    HRESULT hr = self->init(window, desc);
    if (FAILED(hr)) {
        self->destroy(); /* nothing was submitted, so every partially created object can go */
        return hr;
    }
    *out = self;
    return S_OK;
}

HRESULT BxPresentSwapChain::init(HWND window, const BxPresentDesc& desc) {
    static HMODULE dxgi = LoadLibraryA("dxgi.dll");
    using CreateFactoryFn = HRESULT(WINAPI*)(REFIID, void**);
    auto create_factory = dxgi
        ? reinterpret_cast<CreateFactoryFn>(reinterpret_cast<void*>(GetProcAddress(dxgi, "CreateDXGIFactory1")))
        : nullptr;
    if (!create_factory) {
        bx_log_emit(BX_LOG_ERROR, "d3d12_xs", "present_dxgi_missing", "dxgi.dll!CreateDXGIFactory1 not available");
        return DXGI_ERROR_UNSUPPORTED;
    }

    IDXGIFactory4* factory = nullptr;
    HRESULT hr = create_factory(__uuidof(IDXGIFactory4), reinterpret_cast<void**>(&factory));
    if (FAILED(hr)) {
        log_hr(BX_LOG_ERROR, "present_create_failed", "CreateDXGIFactory1", hr);
        return hr;
    }

    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Width = desc.width;
    sd.Height = desc.height;
    sd.Format = desc.format;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = desc.buffer_count;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    IDXGISwapChain1* swap1 = nullptr;
    /* For D3D12 the "device" argument of CreateSwapChainForHwnd is the command queue. */
    hr = factory->CreateSwapChainForHwnd(host_queue_, window, &sd, nullptr, nullptr, &swap1);
    if (SUCCEEDED(hr)) {
        /* Alt+Enter would require ResizeBuffers handling that the bridge does not have. */
        HRESULT assoc = factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);
        if (FAILED(assoc)) {
            log_hr(BX_LOG_WARN, "present_window_association", "MakeWindowAssociation", assoc);
        }
    }
    factory->Release();
    if (FAILED(hr)) {
        log_hr(BX_LOG_ERROR, "present_create_failed", "CreateSwapChainForHwnd", hr);
        return hr;
    }

    hr = swap1->QueryInterface(__uuidof(IDXGISwapChain3), reinterpret_cast<void**>(&swap_));
    swap1->Release();
    if (FAILED(hr)) {
        log_hr(BX_LOG_ERROR, "present_create_failed", "QueryInterface(IDXGISwapChain3)", hr);
        return hr;
    }

    ID3D12Device* device = nullptr;
    hr = host_queue_->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&device));
    if (SUCCEEDED(hr)) {
        hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), reinterpret_cast<void**>(&fence_));
        device->Release();
    }
    if (FAILED(hr)) {
        log_hr(BX_LOG_ERROR, "present_create_failed", "CreateFence", hr);
        return hr;
    }

    event_ = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (!event_) {
        hr = HRESULT_FROM_WIN32(GetLastError());
        log_hr(BX_LOG_ERROR, "present_create_failed", "CreateEvent", hr);
        return hr;
    }

    char detail[128];
    std::snprintf(detail, sizeof(detail), "width=%u height=%u format=%d buffers=%u",
                  desc.width, desc.height, static_cast<int>(desc.format), desc.buffer_count);
    bx_log_emit(BX_LOG_INFO, "d3d12_xs", "present_chain_created", detail);
    return S_OK;
}

HRESULT BxPresentSwapChain::removed_reason() {
    ID3D12Device* device = nullptr;
    HRESULT hr = host_queue_->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&device));
    if (FAILED(hr)) {
        return hr;
    }
    hr = device->GetDeviceRemovedReason();
    device->Release();
    return hr;
}

HRESULT BxPresentSwapChain::signal_locked(UINT64* value) {
    const UINT64 next = last_signaled_ + 1;
    HRESULT hr = host_queue_->Signal(fence_, next);
    if (FAILED(hr)) {
        log_hr(BX_LOG_ERROR, "present_signal_failed", "ID3D12CommandQueue::Signal", hr);
        return hr;
    }
    last_signaled_ = next;
    *value = next;
    return S_OK;
}

HRESULT BxPresentSwapChain::wait_locked(UINT64 value, ULONGLONG start, DWORD timeout_ms) {
    for (;;) {
        const UINT64 completed = fence_->GetCompletedValue();
        if (completed == UINT64_MAX) { /* the documented value after device removal */
            HRESULT reason = removed_reason();
            log_hr(BX_LOG_ERROR, "present_device_removed", "fence", reason);
            return FAILED(reason) ? reason : DXGI_ERROR_DEVICE_REMOVED;
        }
        if (completed >= value) {
            return S_OK;
        }
        const ULONGLONG elapsed = GetTickCount64() - start;
        if (elapsed >= timeout_ms) {
            break;
        }
        HRESULT hr = fence_->SetEventOnCompletion(value, event_);
        if (FAILED(hr)) {
            log_hr(BX_LOG_ERROR, "present_wait_failed", "SetEventOnCompletion", hr);
            return hr;
        }
        const ULONGLONG elapsed_after_registration = GetTickCount64() - start;
        if (elapsed_after_registration >= timeout_ms) {
            continue; /* Recheck the fence before reporting a timeout. */
        }
        /* The event can be left over from an earlier timed-out wait, so the loop re-reads the fence. */
        DWORD waited = WaitForSingleObject(event_, static_cast<DWORD>(timeout_ms - elapsed_after_registration));
        if (waited == WAIT_FAILED) {
            hr = HRESULT_FROM_WIN32(GetLastError());
            log_hr(BX_LOG_ERROR, "present_wait_failed", "WaitForSingleObject", hr);
            return hr;
        }
    }
    char detail[128];
    std::snprintf(detail, sizeof(detail), "timeout_ms=%lu wanted=%llu completed=%llu", static_cast<unsigned long>(timeout_ms),
                  static_cast<unsigned long long>(value), static_cast<unsigned long long>(fence_->GetCompletedValue()));
    bx_log_emit(BX_LOG_ERROR, "d3d12_xs", "present_wait_timeout", detail);
    return kWaitTimedOut;
}

HRESULT BxPresentSwapChain::post_present_failure(const char* event, const char* what, HRESULT hr,
                                                 HRESULT live_device_result) {
    const HRESULT removed = removed_reason();
    if (FAILED(removed)) {
        log_hr(BX_LOG_ERROR, "present_device_removed", what, removed);
        return removed;
    }
    needs_idle_ = true;
    if (event) {
        log_hr(BX_LOG_ERROR, event, what, hr);
    }
    if (is_device_removed_hr(hr)) {
        log_hr(BX_LOG_ERROR, "present_device_removed", what, hr);
        return hr;
    }
    return live_device_result;
}

HRESULT BxPresentSwapChain::idle_locked(ULONGLONG start, DWORD timeout_ms) {
    if (GetTickCount64() - start >= timeout_ms) {
        char detail[96];
        std::snprintf(detail, sizeof(detail), "timeout_ms=%lu wanted=%llu completed=unknown; expired before Signal",
                      static_cast<unsigned long>(timeout_ms), static_cast<unsigned long long>(last_signaled_ + 1));
        bx_log_emit(BX_LOG_ERROR, "d3d12_xs", "present_wait_timeout", detail);
        return kWaitTimedOut;
    }
    UINT64 value = 0;
    HRESULT hr = signal_locked(&value);
    return FAILED(hr) ? hr : wait_locked(value, start, timeout_ms);
}

void BxPresentSwapChain::destroy() {
    if (swap_ && fence_ && event_) {
        HRESULT idle = idle_locked(GetTickCount64(), BX_PRESENT_WAIT_MS);
        if (FAILED(idle) && SUCCEEDED(removed_reason())) {
            /* The device is alive and the GPU may still be using the back buffers. */
            log_hr(BX_LOG_ERROR, "present_chain_leaked", "queue not idle; GPU-visible objects deliberately not released", idle);
            /*
             * swap_, fence_, event_ and host_queue_ stay: pending work may touch them. The proxy
             * is only BetterXen's wrapper; host_queue_ keeps the host queue alive without it.
             */
            ID3D12CommandQueue* proxy = queue_proxy_;
            delete this;
            proxy->Release();
            return;
        }
    }
    const bool had_swap = swap_ != nullptr;
    ULONG swap_refs = 0;
    if (swap_) swap_refs = swap_->Release();
    if (fence_) fence_->Release();
    if (event_) CloseHandle(event_);
    if (had_swap) {
        /*
         * The count the host's Release returned. Non-zero means something else still references
         * the swapchain: back buffers or host pointers the caller holds, or the host itself
         * (see docs/research/d3dmetal-swapchain-references.md). It is reported, never "fixed up"
         * by releasing references this object does not own.
         */
        char detail[96];
        std::snprintf(detail, sizeof(detail), "presented=%llu swapchain_release_returned=%lu",
                      static_cast<unsigned long long>(presented_), static_cast<unsigned long>(swap_refs));
        bx_log_emit(swap_refs ? BX_LOG_WARN : BX_LOG_INFO, "d3d12_xs", "present_chain_destroyed", detail);
    }
    ID3D12CommandQueue* host_queue = host_queue_;
    ID3D12CommandQueue* queue_proxy = queue_proxy_;
    delete this;
    if (host_queue) host_queue->Release();
    if (queue_proxy) queue_proxy->Release();
}

HRESULT STDMETHODCALLTYPE BxPresentSwapChain::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) {
        return E_POINTER;
    }
    if (IsEqualGUID(riid, __uuidof(IUnknown)) || IsEqualGUID(riid, IID_IBxPresentSwapChain)) {
        AddRef();
        *ppv = static_cast<IBxPresentSwapChain*>(this);
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE BxPresentSwapChain::AddRef() {
    return refs_.fetch_add(1, std::memory_order_relaxed) + 1;
}

ULONG STDMETHODCALLTYPE BxPresentSwapChain::Release() {
    ULONG left = refs_.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (left == 0) {
        destroy();
    }
    return left;
}

HRESULT STDMETHODCALLTYPE BxPresentSwapChain::GetBuffer(UINT index, REFIID riid, void** ppv) {
    if (!ppv) {
        return E_POINTER;
    }
    *ppv = nullptr;
    HRESULT hr = swap_->GetBuffer(index, riid, ppv);
    if (FAILED(hr)) {
        log_hr(BX_LOG_WARN, "present_get_buffer_failed", "IDXGISwapChain::GetBuffer", hr);
    }
    return hr;
}

UINT STDMETHODCALLTYPE BxPresentSwapChain::GetCurrentBackBufferIndex() {
    return swap_->GetCurrentBackBufferIndex();
}

HRESULT STDMETHODCALLTYPE BxPresentSwapChain::Present(UINT sync_interval, UINT flags) {
    BX_D3D_TRACE("IBxPresentSwapChain", "Present");
    if (sync_interval > 4 || flags != 0 || !IsWindow(window_)) {
        char detail[96];
        std::snprintf(detail, sizeof(detail), "sync=%u flags=0x%x window_alive=%d", sync_interval, flags,
                      IsWindow(window_) ? 1 : 0);
        bx_log_emit(BX_LOG_WARN, "d3d12_xs", "present_rejected", detail);
        return DXGI_ERROR_INVALID_CALL;
    }
    if (owner_ == GetCurrentThreadId()) {
        bx_log_emit(BX_LOG_WARN, "d3d12_xs", "present_rejected", "re-entered on the same thread");
        return DXGI_ERROR_INVALID_CALL;
    }
    /*
     * Never wait for the lock. Its holder may be inside the host's Present, and DXGI can make that
     * call wait for the window's thread: the caller here.
     */
    if (!TryAcquireSRWLockExclusive(&lock_)) {
        bx_log_emit(BX_LOG_WARN, "d3d12_xs", "present_busy", "another call holds the swapchain; nothing presented");
        return BX_PRESENT_E_BUSY;
    }
    SrwOwner guard(lock_, owner_);
    if (needs_idle_) {
        bx_log_emit(BX_LOG_WARN, "d3d12_xs", "present_needs_idle", "completion unconfirmed; nothing presented");
        return BX_PRESENT_E_NEEDS_IDLE;
    }
    const UINT presented_index = swap_->GetCurrentBackBufferIndex();
    if (presented_index < DXGI_MAX_SWAP_CHAIN_BUFFERS) {
        /* Busy only after an earlier BX_PRESENT_E_NEXT_BUFFER_BUSY. Nothing is presented then. */
        const HRESULT ready = wait_locked(buffer_value_[presented_index], GetTickCount64(), BX_PRESENT_WAIT_MS);
        if (FAILED(ready)) {
            return ready;
        }
    }
    HRESULT hr = swap_->Present(sync_interval, flags);
    if (FAILED(hr)) {
        char detail[128];
        std::snprintf(detail, sizeof(detail), "sync=%u flags=0x%x hr=0x%08lx device_removed_reason=0x%08lx", sync_interval,
                      flags, static_cast<unsigned long>(hr), static_cast<unsigned long>(removed_reason()));
        bx_log_emit(BX_LOG_ERROR, "d3d12_xs", "present_failed", detail);
        return hr;
    }
    ++presented_;

    UINT64 value = 0;
    HRESULT sync_hr = signal_locked(&value);
    if (FAILED(sync_hr)) {
        /* signal_locked already logged present_signal_failed when the device stays live. */
        return post_present_failure(nullptr, "signal after Present", sync_hr, BX_PRESENT_E_COMPLETION_UNCONFIRMED);
    }
    if (presented_index < DXGI_MAX_SWAP_CHAIN_BUFFERS) {
        buffer_value_[presented_index] = value;
    }
    const UINT next = swap_->GetCurrentBackBufferIndex();
    if (next < DXGI_MAX_SWAP_CHAIN_BUFFERS) {
        sync_hr = wait_locked(buffer_value_[next], GetTickCount64(), BX_PRESENT_WAIT_MS);
        if (sync_hr == kWaitTimedOut) {
            return BX_PRESENT_E_NEXT_BUFFER_BUSY;
        }
        if (FAILED(sync_hr)) {
            return post_present_failure("present_next_buffer_wait_failed", "wait_locked", sync_hr,
                                      BX_PRESENT_E_NEXT_BUFFER_WAIT_FAILED);
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE BxPresentSwapChain::WaitForIdle(DWORD timeout_ms) {
    if (timeout_ms == 0 || timeout_ms == INFINITE) {
        return E_INVALIDARG;
    }
    if (owner_ == GetCurrentThreadId()) {
        return DXGI_ERROR_INVALID_CALL;
    }
    const ULONGLONG start = GetTickCount64();
    while (!TryAcquireSRWLockExclusive(&lock_)) {
        if (GetTickCount64() - start >= timeout_ms) {
            bx_log_emit(BX_LOG_ERROR, "d3d12_xs", "present_wait_timeout", "swapchain busy in another call");
            return kWaitTimedOut;
        }
        Sleep(1);
    }
    SrwOwner guard(lock_, owner_);
    const HRESULT hr = idle_locked(start, timeout_ms);
    if (hr == S_OK) {
        /* A signal queued after every presented frame has completed. */
        needs_idle_ = false;
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE BxPresentSwapChain::GetHostSwapChain(REFIID riid, void** ppv) {
    if (!ppv) {
        return E_POINTER;
    }
    *ppv = nullptr;
    return swap_->QueryInterface(riid, ppv);
}

bool desc_is_valid(const BxPresentDesc& d) {
    const bool format_ok = d.format == DXGI_FORMAT_R8G8B8A8_UNORM || d.format == DXGI_FORMAT_B8G8R8A8_UNORM ||
                           d.format == DXGI_FORMAT_R10G10B10A2_UNORM || d.format == DXGI_FORMAT_R16G16B16A16_FLOAT;
    return format_ok && d.width >= 1 && d.width <= D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION && d.height >= 1 &&
           d.height <= D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION && d.buffer_count >= 2 &&
           d.buffer_count <= DXGI_MAX_SWAP_CHAIN_BUFFERS;
}

/* COM tear-off: its own reference count, one reference on the queue, and the queue's identity. */
class BxPresentQueue final : public IBxPresentQueue {
public:
    explicit BxPresentQueue(ID3D12CommandQueue* queue_proxy) : queue_(queue_proxy) { queue_->AddRef(); }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) {
            return E_POINTER;
        }
        if (IsEqualGUID(riid, IID_IBxPresentQueue)) {
            AddRef();
            *ppv = static_cast<IBxPresentQueue*>(this);
            return S_OK;
        }
        return queue_->QueryInterface(riid, ppv);
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return refs_.fetch_add(1, std::memory_order_relaxed) + 1; }

    ULONG STDMETHODCALLTYPE Release() override {
        ULONG left = refs_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (left == 0) {
            ID3D12CommandQueue* queue = queue_;
            delete this;
            queue->Release();
        }
        return left;
    }

    HRESULT STDMETHODCALLTYPE CreateSwapChain(HWND window, const BxPresentDesc* desc, IBxPresentSwapChain** out) override {
        if (!out) {
            return E_POINTER;
        }
        *out = nullptr;
        if (!desc) {
            return E_POINTER;
        }
        if (!window || !IsWindow(window) || !desc_is_valid(*desc)) {
            log_hr(BX_LOG_WARN, "present_create_rejected", "invalid window or description", DXGI_ERROR_INVALID_CALL);
            return DXGI_ERROR_INVALID_CALL;
        }
        ID3D12CommandQueue* host = nullptr;
        HRESULT hr = static_cast<IUnknown*>(bx_unwrap_raw(queue_))
                         ->QueryInterface(__uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(&host));
        if (FAILED(hr)) {
            log_hr(BX_LOG_ERROR, "present_create_failed", "host command queue", hr);
            return hr;
        }
        D3D12_COMMAND_QUEUE_DESC queue_desc = {};
        host->GetDesc(&queue_desc);
        if (queue_desc.Type != D3D12_COMMAND_LIST_TYPE_DIRECT) {
            log_hr(BX_LOG_WARN, "present_create_rejected", "queue is not a DIRECT queue", DXGI_ERROR_INVALID_CALL);
            hr = DXGI_ERROR_INVALID_CALL;
        } else {
            hr = BxPresentSwapChain::create(queue_, host, window, *desc, out);
        }
        host->Release();
        return hr;
    }

private:
    ~BxPresentQueue() = default;
    std::atomic<ULONG> refs_{1};
    ID3D12CommandQueue* queue_;
};

} // namespace

HRESULT bx_present_query_queue(ID3D12CommandQueue* queue_proxy, void** ppv) {
    if (!ppv) {
        return E_POINTER;
    }
    auto* bridge = new (std::nothrow) BxPresentQueue(queue_proxy);
    if (!bridge) {
        *ppv = nullptr;
        return E_OUTOFMEMORY;
    }
    *ppv = static_cast<IBxPresentQueue*>(bridge);
    return S_OK;
}
