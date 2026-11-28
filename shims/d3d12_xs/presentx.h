#ifndef BETTERXEN_D3D12_XS_PRESENTX_H
#define BETTERXEN_D3D12_XS_PRESENTX_H

/*
 * Track C (M2-C2, #465): the PresentX presentation bridge over host DXGI.
 *
 * THESE INTERFACES ARE DEFINED BY BETTERXEN. They are not the console's PresentX, whose
 * signature and structures are not defined in abi/, and no binary compatibility with any Xbox
 * interface is claimed. A guest that calls a console-only command queue slot still lands on the
 * proxy's logged stub.
 *
 * Use: QueryInterface the command queue returned by a D3D12XboxCreateDevice device for
 * IID_IBxPresentQueue, then create a swapchain on a Win32 window. No new DLL export is involved.
 *
 * This is a subset of the public DXGI flip model, not all of it: the caller records its own
 * resource transitions and every back buffer must be in D3D12_RESOURCE_STATE_PRESENT when it is
 * presented. D3D12 offers no way to query a resource's state, so the bridge cannot check this.
 * ResizeBuffers and fullscreen are not implemented.
 */
#ifndef WIDL_EXPLICIT_AGGREGATE_RETURNS
#define WIDL_EXPLICIT_AGGREGATE_RETURNS
#endif
#include <windows.h>
#include <unknwn.h>
#include <d3d12.h>
#include <dxgi1_4.h>

/* {34127279-2D93-4642-88A6-97B834217B8A} */
inline const GUID IID_IBxPresentQueue = {0x34127279, 0x2d93, 0x4642, {0x88, 0xa6, 0x97, 0xb8, 0x34, 0x21, 0x7b, 0x8a}};
/* {DEC9F73E-DE84-4F87-9167-26203B3A1152} */
inline const GUID IID_IBxPresentSwapChain = {0xdec9f73e, 0xde84, 0x4f87, {0x91, 0x67, 0x26, 0x20, 0x3b, 0x3a, 0x11, 0x52}};

/* Longest single GPU wait Present() performs, and the destructor's wait for the GPU. */
inline constexpr DWORD BX_PRESENT_WAIT_MS = 5000;
/*
 * Present() result: the frame WAS handed to the host, but the next back buffer is still busy.
 * Do not present that frame again. Call WaitForIdle until it returns S_OK before reusing a buffer.
 */
inline constexpr HRESULT BX_PRESENT_E_NEXT_BUFFER_BUSY = static_cast<HRESULT>(0x80040201u);
/*
 * Present() result: the frame was NOT handed to the host, because another Present or WaitForIdle
 * on this swapchain is in progress. Present never waits for that call. Try again after it returns.
 */
inline constexpr HRESULT BX_PRESENT_E_BUSY = static_cast<HRESULT>(0x80040202u);
/*
 * Present() result: the frame WAS handed to the host, but the following fence signal failed on
 * a live device, so its completion is unknown. Do not present that frame again. Call WaitForIdle
 * until it returns S_OK before recording to any back buffer or presenting again. Device removal
 * returns the removal HRESULT unchanged instead.
 */
inline constexpr HRESULT BX_PRESENT_E_COMPLETION_UNCONFIRMED = static_cast<HRESULT>(0x80040203u);
/*
 * Present() result: the frame was NOT handed to the host, because an earlier Present returned
 * BX_PRESENT_E_COMPLETION_UNCONFIRMED or BX_PRESENT_E_NEXT_BUFFER_WAIT_FAILED and no WaitForIdle
 * has succeeded since.
 */
inline constexpr HRESULT BX_PRESENT_E_NEEDS_IDLE = static_cast<HRESULT>(0x80040204u);
/*
 * Present() result: the frame WAS handed to the host and signaled, but waiting on the next back
 * buffer failed on a live device (for example SetEventOnCompletion or WaitForSingleObject). Do not
 * present that frame again. Call WaitForIdle until it returns S_OK before reusing any back buffer
 * or presenting again. Device removal returns the removal HRESULT unchanged instead.
 */
inline constexpr HRESULT BX_PRESENT_E_NEXT_BUFFER_WAIT_FAILED = static_cast<HRESULT>(0x80040205u);

struct BxPresentDesc {
    UINT width;          /* 1..16384; 0 ("use the window size") is not accepted */
    UINT height;         /* 1..16384 */
    DXGI_FORMAT format;  /* R8G8B8A8_UNORM, B8G8R8A8_UNORM, R10G10B10A2_UNORM or R16G16B16A16_FLOAT */
    UINT buffer_count;   /* 2..DXGI_MAX_SWAP_CHAIN_BUFFERS */
};

/*
 * One swapchain. Holds a reference on the command queue it was created from.
 * The final Release waits (bounded) for the queue to go idle. If that fails and the device
 * reports it has not been removed, it logs and deliberately leaks what pending GPU work may
 * still touch (swapchain, fence, event, host queue) and releases its queue proxy reference.
 * After device removal everything is released. Time spent inside the host's own Release is not bounded: on the
 * wined3d/vkd3d path (not D3DMetal) an intermittent stall in it was observed.
 */
struct IBxPresentSwapChain : public IUnknown {
    /* Same rules as IDXGISwapChain::GetBuffer. The caller owns the returned reference. */
    virtual HRESULT STDMETHODCALLTYPE GetBuffer(UINT index, REFIID riid, void** ppv) = 0;
    virtual UINT STDMETHODCALLTYPE GetCurrentBackBufferIndex() = 0;
    /*
     * Waits (bounded) until the current back buffer is idle, calls IDXGISwapChain::Present,
     * signals a fence on the queue, then waits until the next back buffer is idle. It never waits
     * for another thread's call on this swapchain. Results:
     *   - a DXGI success code, including DXGI_STATUS_OCCLUDED: presented, next buffer idle;
     *   - HRESULT_FROM_WIN32(ERROR_TIMEOUT): NOT presented (the current buffer is still busy);
     *   - BX_PRESENT_E_NEXT_BUFFER_BUSY: presented, see its definition;
     *   - BX_PRESENT_E_BUSY: NOT presented, another call holds the swapchain;
     *   - BX_PRESENT_E_COMPLETION_UNCONFIRMED: presented, the fence signal failed on a live device;
     *   - BX_PRESENT_E_NEXT_BUFFER_WAIT_FAILED: presented, waiting on the next back buffer failed
     *     on a live device;
     *   - BX_PRESENT_E_NEEDS_IDLE: NOT presented, WaitForIdle must succeed first;
     *   - another failure: the host's Present failure, a pre-Present fence wait or device failure
     *     (nothing was presented), or a device-removal HRESULT (such as DXGI_ERROR_DEVICE_REMOVED
     *     or DXGI_ERROR_DEVICE_RESET) observed after host Present, returned unchanged. If the
     *     device still reports itself as live, WaitForIdle must succeed before the next Present.
     * The bounds cover BetterXen's own waits, not time spent inside the host's Present.
     *
     * The bridge itself returns DXGI_ERROR_INVALID_CALL, without calling the host, for a
     * sync_interval above 4, for any flag (none is supported, including DXGI_PRESENT_TEST),
     * once the window is destroyed, and when Present or WaitForIdle is re-entered on a thread
     * already inside one of them. D3DMetal 3.0 was observed to answer S_OK to the first three.
     */
    virtual HRESULT STDMETHODCALLTYPE Present(UINT sync_interval, UINT flags) = 0;
    /*
     * Waits until everything submitted to the queue before this call has finished. S_OK also ends
     * the recovery state left by a post-Present synchronization failure on a live device.
     * Only positive, finite timeouts are supported. Zero and INFINITE return E_INVALIDARG
     * without signaling. A timeout returns HRESULT_FROM_WIN32(ERROR_TIMEOUT).
     * Lock acquisition, fence signaling, and fence waiting share timeout_ms; if lock acquisition
     * uses the whole budget, WaitForIdle times out without signaling. Time spent inside host calls
     * reduces the remaining wait, but the bridge cannot interrupt those calls.
     */
    virtual HRESULT STDMETHODCALLTYPE WaitForIdle(DWORD timeout_ms) = 0;
    /*
     * The host DXGI swapchain, for diagnostics and tests. The caller owns the reference and must
     * not call Present or ResizeBuffers on it: the bridge's buffer tracking would not follow.
     */
    virtual HRESULT STDMETHODCALLTYPE GetHostSwapChain(REFIID riid, void** ppv) = 0;
};

/* Tear-off on the command queue proxy; QueryInterface for any other IID returns the queue's answer. */
struct IBxPresentQueue : public IUnknown {
    /*
     * The queue must be a DIRECT queue and `window` an existing window. Argument errors return
     * E_POINTER or DXGI_ERROR_INVALID_CALL; host failures are returned unchanged and logged.
     */
    virtual HRESULT STDMETHODCALLTYPE CreateSwapChain(HWND window, const BxPresentDesc* desc, IBxPresentSwapChain** out) = 0;
};

/* Internal to d3d12_xs.dll: the command queue proxy answers IID_IBxPresentQueue with this. */
HRESULT bx_present_query_queue(ID3D12CommandQueue* queue_proxy, void** ppv);

#endif /* BETTERXEN_D3D12_XS_PRESENTX_H */
