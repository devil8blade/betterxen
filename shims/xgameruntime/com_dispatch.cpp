/*
 * Clean-room COM IUnknown vtable dispatch for xgameruntime.dll!QueryApiImpl (#456).
 *
 * Provenance: Interface identifiers, IUnknown inheritance, and vtable slot indices were
 * established via clean-room black-box interoperability testing against Microsoft's publicly
 * distributed PC GDK NuGet package (Microsoft.GDK.Windows 2610.0.9661, nuget.org) using an
 * independently authored synthetic caller invoking public documented GDK APIs. No Microsoft
 * source code or binary code is copied or bundled.
 *
 * XGameSave and XPackage class ids, interface ids, and vtable order are separate from that
 * work. Those facts are from https://github.com/xodus-gaming/xgameruntime-docs commit
 * 7090008692569aa6081e30dfdd9de58237741e24. Function signatures follow the public Microsoft
 * Learn GDK reference (learn.microsoft.com/gaming/gdk). No text from either source is copied.
 */
#include "com_dispatch.h"

#include "runtime_internal.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_set>
#include <utility>

#include "bx_log.h"
#include "task_queue.h"

extern "C" {
HRESULT WINAPI XTaskQueueCreate(bx::gdk::DispatchMode, bx::gdk::DispatchMode, void**);
void WINAPI XTaskQueueCloseHandle(void*);
bool WINAPI XTaskQueueDispatch(void*, bx::gdk::Port, uint32_t);
HRESULT WINAPI XTaskQueueSubmitCallback(void*, bx::gdk::Port, void*, void (CALLBACK*)(void*, bool));
HRESULT WINAPI XTaskQueueSubmitDelayedCallback(void*, bx::gdk::Port, uint32_t, void*,
                                                           void (CALLBACK*)(void*, bool));
bool WINAPI XTaskQueueGetCurrentProcessTaskQueue(void**);

HRESULT WINAPI XAsyncGetStatus(void*, bool);
HRESULT WINAPI XAsyncGetResultSize(void*, size_t*);
void WINAPI XAsyncCancel(void*);

HRESULT WINAPI XUserAddAsync(uint32_t, void*);
HRESULT WINAPI XUserAddResult(void*, void**);
void WINAPI XUserCloseHandle(void*);
HRESULT WINAPI XUserGetId(void*, uint64_t*);
HRESULT WINAPI XUserGetGamertag(void*, uint32_t, size_t, char*, size_t*);
HRESULT WINAPI XUserGetTokenAndSignatureAsync(void*, uint32_t, const char*, const char*, size_t, const void*, size_t,
                                              const uint8_t*, void*);
HRESULT WINAPI XUserGetTokenAndSignatureResultSize(void*, size_t*);
HRESULT WINAPI XUserGetTokenAndSignatureResult(void*, size_t, void*, void**, size_t*);
HRESULT WINAPI XUserResolvePrivilegeWithUiAsync(void*, uint32_t, uint32_t, void*);
HRESULT WINAPI XUserResolvePrivilegeWithUiResult(void*);

HRESULT WINAPI XStoreCreateContext(void*, void**);
void WINAPI XStoreCloseContextHandle(void*);
HRESULT WINAPI XStoreQueryGameLicenseAsync(void*, void*);
HRESULT WINAPI XStoreQueryGameLicenseResult(void*, void*);
HRESULT WINAPI XStoreAcquireLicenseForPackageAsync(void*, const char*, void*);
HRESULT WINAPI XStoreAcquireLicenseForPackageResult(void*, void**);
HRESULT WINAPI XStoreAcquireLicenseForDurablesAsync(void*, const char*, void*);
HRESULT WINAPI XStoreAcquireLicenseForDurablesResult(void*, void**);
HRESULT WINAPI XStoreQueryEntitledProductsAsync(void*, uint32_t, uint32_t, void*);
HRESULT WINAPI XStoreQueryEntitledProductsResult(void*, void**);
HRESULT WINAPI XStoreQueryAssociatedProductsAsync(void*, uint32_t, uint32_t, void*);
HRESULT WINAPI XStoreQueryAssociatedProductsResult(void*, void**);

HRESULT WINAPI XSystemGetConsoleId(size_t, char*, size_t*);
uint32_t WINAPI XSystemGetDeviceType();
HRESULT WINAPI XSystemGetXboxLiveSandboxId(size_t, char*, size_t*);

HRESULT WINAPI XPersistentLocalStorageGetPathSize(size_t*);
HRESULT WINAPI XPersistentLocalStorageGetPath(size_t, char*, size_t*);
HRESULT WINAPI XPersistentLocalStorageGetSpaceInfo(void*);

HRESULT WINAPI XGameUiShowPlayerProfileCardAsync(void*, uint64_t, void*);
HRESULT WINAPI XGameUiShowTextEntryAsync(const char*, const char*, const char*, uint32_t, uint32_t, void*);
HRESULT WINAPI XNetworkingQueryPreferredLocalUdpMultiplayerPortAsync(void*);
HRESULT WINAPI XNetworkingQuerySecurityInformationForUrlAsync(const char*, void*);
} // extern "C"

namespace bx::gdk {
namespace {

constexpr GUID kIUnknown = {0x00000000, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

// Public Microsoft.GDK.Windows 2610.0.9661 QueryApiImpl class & interface identifiers.
constexpr GUID kCoreGuid = {0x073B7DCB, 0x1FCF, 0x4030, {0x94, 0xBE, 0xE3, 0xC9, 0xEB, 0x62, 0x34, 0x28}};
constexpr GUID kFeatureGuid = {0x8836FE87, 0xEDB9, 0x4FE3, {0x8D, 0xAD, 0x05, 0xF0, 0xD2, 0xCD, 0x5B, 0x40}};
constexpr GUID kSystemClsid = {0xE349BD1A, 0xFC20, 0x4E40, {0xB9, 0x9C, 0x41, 0x78, 0xCC, 0x6B, 0x40, 0x9F}};
constexpr GUID kSystemIid = {0x1861CF2E, 0xE18B, 0x4834, {0xA9, 0xF5, 0xB4, 0xA4, 0xE6, 0xEF, 0xB4, 0xCF}};
constexpr GUID kErrorGuid = {0x8CA467F7, 0x22E8, 0x4096, {0x84, 0x56, 0xBB, 0x8A, 0xA1, 0x3F, 0x79, 0xD8}};
constexpr GUID kPlsClsid = {0xF4FAF4D4, 0x2D04, 0x4FCE, {0xB3, 0xE0, 0x47, 0x4A, 0x71, 0x3A, 0x3E, 0x84}};
constexpr GUID kPlsIid = {0x41A4E10C, 0x5A7E, 0x41D9, {0x8C, 0x37, 0x37, 0xBD, 0xE6, 0x2A, 0x07, 0xD6}};
constexpr GUID kUserClsid = {0x01ACD177, 0x91F9, 0x4763, {0xA3, 0x8E, 0xCC, 0xBB, 0x55, 0xCE, 0x32, 0xE0}};
constexpr GUID kUserGamertagIid = {0xCEF4FAC0, 0x7676, 0x4A94, {0xA1, 0x19, 0x4C, 0x43, 0xF9, 0xEB, 0x5B, 0x74}};
constexpr GUID kStoreClsid = {0x0DD112AC, 0x7C24, 0x448C, {0xB9, 0x2B, 0x39, 0x60, 0xFB, 0x5B, 0xD3, 0x0C}};
constexpr GUID kStoreIid = {0xB09D803C, 0x2414, 0x4A05, {0x82, 0xC6, 0x66, 0xDF, 0xDC, 0x9E, 0x9A, 0x44}};
constexpr GUID kStoreDurablesIid = {0x2D42FEA5, 0xE71D, 0x4B76, {0x97, 0xCD, 0xC5, 0x0A, 0xFB, 0xB3, 0xAE, 0x5D}};
constexpr GUID kGameUiClsid = {0xDFCD4649, 0x4FF8, 0x4043, {0xBA, 0x07, 0x35, 0xD6, 0x07, 0xDF, 0x98, 0xB0}};
constexpr GUID kGameUiIid = {0xEAF669DF, 0x5542, 0x4590, {0x99, 0xA3, 0x8D, 0xC0, 0x61, 0xF8, 0x37, 0xCC}};
constexpr GUID kNetworkingClsid = {0x37E56907, 0x2F10, 0x41E8, {0xB7, 0x2F, 0x36, 0xED, 0xB1, 0x85, 0x33, 0x1A}};
constexpr GUID kNetworkingIid = {0xBF2346B2, 0x39AF, 0x4658, {0xB5, 0xEA, 0x44, 0x71, 0x3C, 0x7E, 0x83, 0xB3}};
constexpr GUID kGameClsid = {0x973A344E, 0x24BF, 0x4D0F, {0x84, 0x57, 0x56, 0xC5, 0x34, 0x89, 0x2B, 0x29}};
constexpr GUID kGameIid = {0x2549F142, 0x6419, 0x4A06, {0x97, 0xB5, 0x93, 0x1A, 0xAB, 0x7C, 0x2F, 0x34}};
// XSystemGetAnalyticsInfo and XDisplayTryEnableHdrMode reach the runtime through these keys
// (from authorized GDK material; see the CHANGELOG). The function contracts are public.
constexpr GUID kAnalyticsGuid = {0xB884675D, 0xB738, 0x4A9C, {0x81, 0x5D, 0x9A, 0x9A, 0x1E, 0x0C, 0x6C, 0x9B}};
constexpr GUID kDisplayClsid = {0x03F0FE74, 0xFDD9, 0x4E5C, {0xB6, 0x30, 0xF9, 0x33, 0x9C, 0x47, 0xAC, 0xC5}};
constexpr GUID kDisplayIid = {0x35F07670, 0x706E, 0x4BFB, {0x94, 0x76, 0x09, 0x07, 0x98, 0xC5, 0xEB, 0xF3}};

bool guid_eq(const GUID& a, const GUID& b) {
    return std::memcmp(&a, &b, sizeof(GUID)) == 0;
}

struct ComSingleton {
    const void* const* vtbl;
    const GUID* iid;
    const char* name;
};

HRESULT WINAPI com_query_interface(void* self, const GUID* riid, void** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!riid) return E_INVALIDARG;
    auto* obj = static_cast<ComSingleton*>(self);
    if (guid_eq(*riid, kIUnknown) || guid_eq(*riid, *obj->iid)) {
        *out = obj;
        return S_OK;
    }
    return E_NOINTERFACE;
}

ULONG WINAPI com_add_ref(void*) {
    return 1;
}

ULONG WINAPI com_release(void*) {
    return 1;
}

template <size_t Slot>
HRESULT WINAPI com_unimpl_slot(void* self) {
    auto* obj = static_cast<const ComSingleton*>(self);
    char method[32];
    std::snprintf(method, sizeof(method), "slot_%zu", Slot);
    BX_VTABLE_STUB_LOG(obj ? obj->name : "IXGameRuntime", static_cast<uint32_t>(Slot), method);
    return E_NOTIMPL;
}

template <size_t N, size_t... Is>
constexpr std::array<const void*, N> make_default_vtbl_impl(std::index_sequence<Is...>) {
    return {((Is == 0)   ? reinterpret_cast<const void*>(&com_query_interface)
             : (Is == 1) ? reinterpret_cast<const void*>(&com_add_ref)
             : (Is == 2) ? reinterpret_cast<const void*>(&com_release)
                         : reinterpret_cast<const void*>(&com_unimpl_slot<Is>))...};
}

template <size_t N>
std::array<const void*, N> make_default_vtbl() {
    return make_default_vtbl_impl<N>(std::make_index_sequence<N>{});
}

// --- Core (XAsync + XTaskQueue + XThread) ---

thread_local bool tls_time_sensitive = false;

HRESULT WINAPI core_async_get_status(void*, void* async, bool wait) {
    return XAsyncGetStatus(async, wait);
}
HRESULT WINAPI core_async_get_result_size(void*, void* async, size_t* size) {
    return XAsyncGetResultSize(async, size);
}
void WINAPI core_async_cancel(void*, void* async) {
    XAsyncCancel(async);
}
HRESULT WINAPI core_tq_create(void*, DispatchMode work, DispatchMode comp, void** out) {
    return XTaskQueueCreate(work, comp, out);
}
bool WINAPI core_tq_dispatch(void*, void* queue, Port port, uint32_t timeout) {
    return XTaskQueueDispatch(queue, port, timeout);
}
void WINAPI core_tq_close(void*, void* queue) {
    XTaskQueueCloseHandle(queue);
}
HRESULT WINAPI core_tq_submit(void*, void* queue, Port port, void* ctx, void (CALLBACK* cb)(void*, bool)) {
    return XTaskQueueSubmitCallback(queue, port, ctx, cb);
}
HRESULT WINAPI core_tq_submit_delayed(void*, void* queue, Port port, uint32_t delay_ms, void* ctx,
                                      void (CALLBACK* cb)(void*, bool)) {
    if (delay_ms > 0) {
        char detail[96];
        std::snprintf(detail, sizeof(detail), "queued delay_ms=%lu", static_cast<unsigned long>(delay_ms));
        bx_log_emit(BX_LOG_INFO, "xgameruntime", "XTaskQueueSubmitDelayedCallback", detail);
    }
    return XTaskQueueSubmitDelayedCallback(queue, port, delay_ms, ctx, cb);
}
bool WINAPI core_tq_get_process(void*, void** out) {
    return XTaskQueueGetCurrentProcessTaskQueue(out);
}
HRESULT WINAPI core_thread_set_time_sensitive(void*, bool is_ts) {
    tls_time_sensitive = is_ts;
    return S_OK;
}
HRESULT WINAPI core_thread_verify_not_time_sensitive(void*) {
    return tls_time_sensitive ? E_FAIL : S_OK;
}
HRESULT WINAPI core_runtime_guard_hook(void*, uint32_t, void*) {
    // Invoked by public GDK static thunks before subsystem dispatch.
    return S_OK;
}
bool WINAPI core_thread_is_time_sensitive(void*) {
    return tls_time_sensitive;
}

const std::array<const void*, 36> kCoreVtbl = [] {
    auto v = make_default_vtbl<36>();
    v[3] = reinterpret_cast<const void*>(&core_async_get_status);
    v[4] = reinterpret_cast<const void*>(&core_async_get_result_size);
    v[5] = reinterpret_cast<const void*>(&core_async_cancel);
    v[12] = reinterpret_cast<const void*>(&core_tq_create);
    v[16] = reinterpret_cast<const void*>(&core_tq_dispatch);
    v[17] = reinterpret_cast<const void*>(&core_tq_close);
    v[18] = reinterpret_cast<const void*>(&core_tq_submit);
    v[19] = reinterpret_cast<const void*>(&core_tq_submit_delayed);
    v[25] = reinterpret_cast<const void*>(&core_tq_get_process);
    v[27] = reinterpret_cast<const void*>(&core_thread_set_time_sensitive);
    v[28] = reinterpret_cast<const void*>(&core_thread_verify_not_time_sensitive);
    v[29] = reinterpret_cast<const void*>(&core_runtime_guard_hook);
    v[30] = reinterpret_cast<const void*>(&core_thread_is_time_sensitive);
    return v;
}();
ComSingleton kCoreObj{kCoreVtbl.data(), &kCoreGuid, "IXGameRuntimeCore"};

// --- XGameRuntimeIsFeatureAvailable ---

bool WINAPI feature_is_available(void*, uint32_t feature) {
    // XGameRuntimeFeature enum values from public XGameRuntimeFeature.h.
    switch (feature) {
        case 2:  // XAsync
        case 5:  // XGame
        case 8:  // XGameUI
        case 10: // XNetworking
        case 12: // XPersistentLocalStorage
        case 14: // XStore
        case 15: // XSystem
        case 16: // XTaskQueue
        case 17: // XThread
        case 18: // XUser
        case 19: // XError
            return true;
        default:
            return false;
    }
}

const std::array<const void*, 8> kFeatureVtbl = [] {
    auto v = make_default_vtbl<8>();
    v[3] = reinterpret_cast<const void*>(&feature_is_available);
    return v;
}();
ComSingleton kFeatureObj{kFeatureVtbl.data(), &kFeatureGuid, "IXGameRuntimeFeature"};

// --- XSystem ---

HRESULT WINAPI sys_get_console_id(void*, size_t sz, char* out, size_t* used) {
    return XSystemGetConsoleId(sz, out, used);
}
HRESULT WINAPI sys_get_sandbox_id(void*, size_t sz, char* out, size_t* used) {
    return XSystemGetXboxLiveSandboxId(sz, out, used);
}
HRESULT WINAPI sys_get_app_device_id(void*, size_t sz, char* out, size_t* used) {
    constexpr char kDeviceId[] = "BX-OFFLINE-APP-DEVICE-000000000001";
    if (used) *used = 0;
    if (!out) return E_POINTER;
    const size_t req = sizeof(kDeviceId);
    if (sz < req) return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    std::memcpy(out, kDeviceId, req);
    if (used) *used = req;
    return S_OK;
}
HRESULT WINAPI sys_handle_track(void*, void*, void*) {
    return S_OK;
}
bool WINAPI sys_is_handle_valid(void*, void* handle) {
    return handle != nullptr;
}
void WINAPI sys_allow_full_bandwidth(void*, bool) {}

const std::array<const void*, 12> kSystemVtbl = [] {
    auto v = make_default_vtbl<12>();
    v[3] = reinterpret_cast<const void*>(&sys_get_console_id);
    v[4] = reinterpret_cast<const void*>(&sys_get_sandbox_id);
    v[5] = reinterpret_cast<const void*>(&sys_get_app_device_id);
    v[6] = reinterpret_cast<const void*>(&sys_handle_track);
    v[7] = reinterpret_cast<const void*>(&sys_is_handle_valid);
    v[8] = reinterpret_cast<const void*>(&sys_allow_full_bandwidth);
    return v;
}();
ComSingleton kSystemObj{kSystemVtbl.data(), &kSystemIid, "IXSystem"};

// --- XSystemGetAnalyticsInfo (public XSystem.h contract) ---

// Public XVersion: four uint16_t fields in this order, overlaid on a uint64_t.
struct XVersionLayout {
    uint16_t major;
    uint16_t minor;
    uint16_t build;
    uint16_t revision;
};
// Public XSystemAnalyticsInfo.
struct XSystemAnalyticsInfoLayout {
    XVersionLayout osVersion;
    XVersionLayout hostingOsVersion;
    char family[64];
    char form[64];
};
static_assert(sizeof(XSystemAnalyticsInfoLayout) == 0x90, "XSystemAnalyticsInfo is 144 bytes");

// Slot 3 returns the structure by value: MSVC passes the result buffer after `this` and
// returns the same pointer.
XSystemAnalyticsInfoLayout* WINAPI analytics_get_info(void*, XSystemAnalyticsInfoLayout* out) {
    if (!out) return out;
    std::memset(out, 0, sizeof(*out));
    // BetterXen's own synthetic title and system OS version; not a console build number.
    out->osVersion = {10, 0, 26100, 0};
    out->hostingOsVersion = {10, 0, 26100, 0};
    std::snprintf(out->family, sizeof(out->family), "Xbox");
    // Series S for the Lockhart profile, Series X for Anaconda (XSystemGetDeviceType 6 / 7).
    std::snprintf(out->form, sizeof(out->form), "%s",
                  XSystemGetDeviceType() == 7u ? "Xbox Series X" : "Xbox Series S");
    char detail[160];
    std::snprintf(detail, sizeof(detail), "family=%s form='%s' os=10.0.26100.0 synthetic=1", out->family,
                  out->form);
    bx_log_emit(BX_LOG_INFO, "xgameruntime", "XSystemGetAnalyticsInfo", detail);
    return out;
}

const std::array<const void*, 8> kAnalyticsVtbl = [] {
    auto v = make_default_vtbl<8>();
    v[3] = reinterpret_cast<const void*>(&analytics_get_info);
    return v;
}();
ComSingleton kAnalyticsObj{kAnalyticsVtbl.data(), &kAnalyticsGuid, "IXSystemAnalytics"};

// --- XDisplayTryEnableHdrMode (public XDisplay.h contract) ---

// Public XDisplayHdrModeInfo.
struct XDisplayHdrModeInfoLayout {
    float minToneMapLuminance;
    float maxToneMapLuminance;
    float maxFullFrameToneMapLuminance;
};
constexpr uint32_t kHdrModeResultDisabled = 2;  // XDisplayHdrModeResult::Disabled

// BetterXen presents SDR through the host swap chain, so HDR is reported as not enabled:
// a successful Disabled answer, not the failure value Unknown. The output is only meaningful
// when HDR is enabled, so it is cleared.
uint32_t WINAPI display_try_enable_hdr(void*, uint32_t preference, XDisplayHdrModeInfoLayout* info) {
    if (info) std::memset(info, 0, sizeof(*info));
    char detail[96];
    std::snprintf(detail, sizeof(detail), "preference=%u result=disabled reason=sdr_presentation", preference);
    bx_log_emit(BX_LOG_INFO, "xgameruntime", "XDisplayTryEnableHdrMode", detail);
    return kHdrModeResultDisabled;
}

const std::array<const void*, 8> kDisplayVtbl = [] {
    auto v = make_default_vtbl<8>();
    v[4] = reinterpret_cast<const void*>(&display_try_enable_hdr);
    return v;
}();
ComSingleton kDisplayObj{kDisplayVtbl.data(), &kDisplayIid, "IXDisplay"};

// --- XError ---

void WINAPI err_set_callback(void*, void*, void*) {}
void WINAPI err_set_options(void*, uint32_t, uint32_t) {}

/*
 * IXError slot 3. xodus-gaming/xgameruntime-docs (commit 7090008) lists this slot as unknown.
 * Its behaviour here comes from the #492 review's interop observation (atesahmet0,
 * 2026-11-26): the GDK's static library calls it before some subsystem calls, for example
 * XPackageMount, and continues only when it returns false. The default stub returned E_NOTIMPL,
 * whose low byte reads as true and stopped those calls. The parameters are not known, so none
 * are read. It is logged once so the call stays visible in `bx-cli trace`.
 */
bool WINAPI err_slot3(void*) {
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true)) {
        bx_log_emit(BX_LOG_INFO, "xgameruntime", "IXError_slot3", "returning false (continue); arguments not read");
    }
    return false;
}

const std::array<const void*, 8> kErrorVtbl = [] {
    auto v = make_default_vtbl<8>();
    v[3] = reinterpret_cast<const void*>(&err_slot3);
    v[4] = reinterpret_cast<const void*>(&err_set_callback);
    v[5] = reinterpret_cast<const void*>(&err_set_options);
    return v;
}();
ComSingleton kErrorObj{kErrorVtbl.data(), &kErrorGuid, "IXError"};

// --- XPersistentLocalStorage ---

HRESULT WINAPI pls_get_path_size(void*, size_t* sz) {
    return XPersistentLocalStorageGetPathSize(sz);
}
HRESULT WINAPI pls_get_path(void*, size_t sz, char* out, size_t* used) {
    return XPersistentLocalStorageGetPath(sz, out, used);
}
HRESULT WINAPI pls_get_space_info(void*, void* info) {
    return XPersistentLocalStorageGetSpaceInfo(info);
}

const std::array<const void*, 12> kPlsVtbl = [] {
    auto v = make_default_vtbl<12>();
    v[3] = reinterpret_cast<const void*>(&pls_get_path_size);
    v[4] = reinterpret_cast<const void*>(&pls_get_path);
    v[5] = reinterpret_cast<const void*>(&pls_get_space_info);
    return v;
}();
ComSingleton kPlsObj{kPlsVtbl.data(), &kPlsIid, "IXPersistentLocalStorage"};

// --- XUser ---

std::mutex user_event_mutex;
uint64_t next_user_event_token = 1;
std::unordered_set<uint64_t> user_event_tokens;

void WINAPI user_close_handle(void*, void* h) {
    XUserCloseHandle(h);
}
int32_t WINAPI user_compare(void*, void* u1, void* u2) {
    if (u1 == u2) return 0;
    return reinterpret_cast<uintptr_t>(u1) < reinterpret_cast<uintptr_t>(u2) ? -1 : 1;
}
HRESULT WINAPI user_get_max_users(void*, uint32_t* max_users) {
    if (!max_users) return E_POINTER;
    *max_users = 1;
    return S_OK;
}
HRESULT WINAPI user_add_async(void*, uint32_t opts, void* async) {
    return XUserAddAsync(opts, async);
}
HRESULT WINAPI user_add_result(void*, void* async, void** out) {
    return XUserAddResult(async, out);
}
HRESULT WINAPI user_get_local_id(void*, void* user, uint64_t* local_id) {
    if (!local_id) return E_POINTER;
    if (!user) return E_INVALIDARG;
    *local_id = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(user));
    return S_OK;
}
HRESULT WINAPI user_get_id(void*, void* user, uint64_t* id) {
    return XUserGetId(user, id);
}
HRESULT WINAPI user_get_is_guest(void*, void* user, bool* is_guest) {
    if (!is_guest) return E_POINTER;
    if (!user) return E_INVALIDARG;
    *is_guest = false;
    return S_OK;
}
HRESULT WINAPI user_get_state(void*, void* user, uint32_t* state) {
    if (!state) return E_POINTER;
    if (!user) return E_INVALIDARG;
    *state = 0; // XUserState::SignedIn
    return S_OK;
}
HRESULT WINAPI user_get_age_group(void*, void* user, uint32_t* age_group) {
    if (!age_group) return E_POINTER;
    if (!user) return E_INVALIDARG;
    *age_group = 3; // XUserAgeGroup::Adult
    return S_OK;
}
HRESULT WINAPI user_check_privilege(void*, void* user, uint32_t, uint32_t, bool* has_priv, uint32_t* deny_reason) {
    if (!has_priv) return E_POINTER;
    if (!user) return E_INVALIDARG;
    *has_priv = true;
    if (deny_reason) *deny_reason = 0; // XUserPrivilegeDenyReason::None
    return S_OK;
}
HRESULT WINAPI user_resolve_priv_async(void*, void* user, uint32_t opts, uint32_t priv, void* async) {
    return XUserResolvePrivilegeWithUiAsync(user, opts, priv, async);
}
HRESULT WINAPI user_resolve_priv_result(void*, void* async) {
    return XUserResolvePrivilegeWithUiResult(async);
}
HRESULT WINAPI user_token_async(void*, void* user, uint32_t opts, const char* method, const char* url,
                                size_t hdr_cnt, const void* hdrs, size_t body_sz, const uint8_t* body, void* async) {
    return XUserGetTokenAndSignatureAsync(user, opts, method, url, hdr_cnt, hdrs, body_sz, body, async);
}
HRESULT WINAPI user_token_result_size(void*, void* async, size_t* sz) {
    return XUserGetTokenAndSignatureResultSize(async, sz);
}
HRESULT WINAPI user_token_result(void*, void* async, size_t sz, void* buf, void** res, size_t* used) {
    return XUserGetTokenAndSignatureResult(async, sz, buf, res, used);
}
HRESULT WINAPI user_register_change_event(void*, void*, void*, void* callback, uint64_t* token) {
    if (!token) return E_POINTER;
    if (!callback) return E_INVALIDARG;
    std::lock_guard lock(user_event_mutex);
    const uint64_t id = next_user_event_token++;
    user_event_tokens.insert(id);
    *token = id;
    return S_OK;
}
bool WINAPI user_unregister_change_event(void*, uint64_t token, bool) {
    std::lock_guard lock(user_event_mutex);
    return user_event_tokens.erase(token) > 0;
}
HRESULT WINAPI user_get_gamertag(void*, void* user, uint32_t comp, size_t sz, char* out, size_t* used) {
    return XUserGetGamertag(user, comp, sz, out, used);
}

const std::array<const void*, 64> kUserVtbl = [] {
    auto v = make_default_vtbl<64>();
    v[4] = reinterpret_cast<const void*>(&user_close_handle);
    v[5] = reinterpret_cast<const void*>(&user_compare);
    v[6] = reinterpret_cast<const void*>(&user_get_max_users);
    v[7] = reinterpret_cast<const void*>(&user_add_async);
    v[8] = reinterpret_cast<const void*>(&user_add_result);
    v[9] = reinterpret_cast<const void*>(&user_get_local_id);
    v[11] = reinterpret_cast<const void*>(&user_get_id);
    v[13] = reinterpret_cast<const void*>(&user_get_is_guest);
    v[14] = reinterpret_cast<const void*>(&user_get_state);
    v[19] = reinterpret_cast<const void*>(&user_get_age_group);
    v[20] = reinterpret_cast<const void*>(&user_check_privilege);
    v[21] = reinterpret_cast<const void*>(&user_resolve_priv_async);
    v[22] = reinterpret_cast<const void*>(&user_resolve_priv_result);
    v[23] = reinterpret_cast<const void*>(&user_token_async);
    v[24] = reinterpret_cast<const void*>(&user_token_result_size);
    v[25] = reinterpret_cast<const void*>(&user_token_result);
    v[33] = reinterpret_cast<const void*>(&user_register_change_event);
    v[34] = reinterpret_cast<const void*>(&user_unregister_change_event);
    return v;
}();
ComSingleton kUserObj{kUserVtbl.data(), &kUserClsid, "IXUser"};

const std::array<const void*, 8> kUserGamertagVtbl = [] {
    auto v = make_default_vtbl<8>();
    v[3] = reinterpret_cast<const void*>(&user_get_gamertag);
    return v;
}();
ComSingleton kUserGamertagObj{kUserGamertagVtbl.data(), &kUserGamertagIid, "IXUserGamertag"};

// --- XStore ---

HRESULT WINAPI store_create_ctx(void*, void* user, void** out) {
    return XStoreCreateContext(user, out);
}
void WINAPI store_close_ctx(void*, void* ctx) {
    XStoreCloseContextHandle(ctx);
}
HRESULT WINAPI store_assoc_async(void*, void* ctx, uint32_t pf, uint32_t max_items, void* async) {
    return XStoreQueryAssociatedProductsAsync(ctx, pf, max_items, async);
}
HRESULT WINAPI store_assoc_result(void*, void* async, void** out) {
    return XStoreQueryAssociatedProductsResult(async, out);
}
HRESULT WINAPI store_entitled_async(void*, void* ctx, uint32_t pf, uint32_t max_items, void* async) {
    return XStoreQueryEntitledProductsAsync(ctx, pf, max_items, async);
}
HRESULT WINAPI store_entitled_result(void*, void* async, void** out) {
    return XStoreQueryEntitledProductsResult(async, out);
}
HRESULT WINAPI store_pkg_lic_async(void*, void* ctx, const char* id, void* async) {
    return XStoreAcquireLicenseForPackageAsync(ctx, id, async);
}
HRESULT WINAPI store_pkg_lic_result(void*, void* async, void** out) {
    return XStoreAcquireLicenseForPackageResult(async, out);
}
HRESULT WINAPI store_game_lic_async(void*, void* ctx, void* async) {
    return XStoreQueryGameLicenseAsync(ctx, async);
}
HRESULT WINAPI store_game_lic_result(void*, void* async, void* out) {
    return XStoreQueryGameLicenseResult(async, out);
}
HRESULT WINAPI store_dur_lic_async(void*, void* ctx, const char* id, void* async) {
    return XStoreAcquireLicenseForDurablesAsync(ctx, id, async);
}
HRESULT WINAPI store_dur_lic_result(void*, void* async, void** out) {
    return XStoreAcquireLicenseForDurablesResult(async, out);
}

const std::array<const void*, 40> kStoreVtbl = [] {
    auto v = make_default_vtbl<40>();
    v[3] = reinterpret_cast<const void*>(&store_create_ctx);
    v[4] = reinterpret_cast<const void*>(&store_close_ctx);
    v[5] = reinterpret_cast<const void*>(&store_assoc_async);
    v[6] = reinterpret_cast<const void*>(&store_assoc_result);
    v[9] = reinterpret_cast<const void*>(&store_entitled_async);
    v[10] = reinterpret_cast<const void*>(&store_entitled_result);
    v[20] = reinterpret_cast<const void*>(&store_pkg_lic_async);
    v[21] = reinterpret_cast<const void*>(&store_pkg_lic_result);
    v[28] = reinterpret_cast<const void*>(&store_game_lic_async);
    v[29] = reinterpret_cast<const void*>(&store_game_lic_result);
    return v;
}();
ComSingleton kStoreObj{kStoreVtbl.data(), &kStoreIid, "IXStore"};

const std::array<const void*, 80> kStoreDurablesVtbl = [] {
    auto v = make_default_vtbl<80>();
    v[71] = reinterpret_cast<const void*>(&store_dur_lic_async);
    v[72] = reinterpret_cast<const void*>(&store_dur_lic_result);
    return v;
}();
ComSingleton kStoreDurablesObj{kStoreDurablesVtbl.data(), &kStoreDurablesIid, "IXStoreDurables"};

// --- XGameUi ---

HRESULT WINAPI gameui_profile_async(void*, void* user, uint64_t xuid, void* async) {
    return XGameUiShowPlayerProfileCardAsync(user, xuid, async);
}
HRESULT WINAPI gameui_text_async(void*, const char* title, const char* desc, const char* def, uint32_t scope,
                                 uint32_t max_chars, void* async) {
    return XGameUiShowTextEntryAsync(title, desc, def, scope, max_chars, async);
}

const std::array<const void*, 32> kGameUiVtbl = [] {
    auto v = make_default_vtbl<32>();
    v[7] = reinterpret_cast<const void*>(&gameui_profile_async);
    v[17] = reinterpret_cast<const void*>(&gameui_text_async);
    return v;
}();
ComSingleton kGameUiObj{kGameUiVtbl.data(), &kGameUiIid, "IXGameUi"};

// --- XNetworking ---

struct ConnectivityHint {
    uint32_t connectivity_level;
    uint32_t connectivity_cost;
    uint32_t iana_interface_type;
    bool network_initialized;
    bool approaching_data_limit;
    bool over_data_limit;
    bool roaming;
};
static_assert(sizeof(ConnectivityHint) == 16);

HRESULT WINAPI net_udp_port_async(void*, void* async) {
    return XNetworkingQueryPreferredLocalUdpMultiplayerPortAsync(async);
}
HRESULT WINAPI net_sec_info_async(void*, const char* url, void* async) {
    return XNetworkingQuerySecurityInformationForUrlAsync(url, async);
}
HRESULT WINAPI net_get_connectivity_hint(void*, ConnectivityHint* hint) {
    if (!hint) return E_POINTER;
    // Offline profile: ConnectivityLevelHint::None (1), CostHint::Unrestricted (1).
    *hint = {1u, 1u, 6u, true, false, false, false};
    return S_OK;
}

const std::array<const void*, 24> kNetworkingVtbl = [] {
    auto v = make_default_vtbl<24>();
    v[4] = reinterpret_cast<const void*>(&net_udp_port_async);
    v[8] = reinterpret_cast<const void*>(&net_sec_info_async);
    v[15] = reinterpret_cast<const void*>(&net_get_connectivity_hint);
    return v;
}();
ComSingleton kNetworkingObj{kNetworkingVtbl.data(), &kNetworkingIid, "IXNetworking"};

// --- XGame ---

HRESULT WINAPI game_get_xbox_title_id(void*, uint32_t* title_id) {
    if (!title_id) return E_POINTER;
    // Synthetic offline title ID for BetterXen runtime.
    *title_id = 0x42580001u;
    return S_OK;
}

const std::array<const void*, 8> kGameVtbl = [] {
    auto v = make_default_vtbl<8>();
    v[3] = reinterpret_cast<const void*>(&game_get_xbox_title_id);
    return v;
}();
ComSingleton kGameObj{kGameVtbl.data(), &kGameIid, "IXGame"};

// --- XGameSave / XPackage (#468) ---
// Ids and inheritance from xodus-gaming/xgameruntime-docs commit
// 7090008692569aa6081e30dfdd9de58237741e24.
// IXGameSaveImpl uses the class id and lists 27 methods in slots 3-29.
// IXGameSaveImpl2 adds 3 XGameSaveFiles methods in slots 30-32.
// IXGameSaveImpl3 adds none.
// IXPackageImpl lists 32 methods in slots 3-34.
// IXPackageImpl2 adds 4 methods in slots 35-38.
// IXPackageImpl3 adds 2 methods in slots 39-40.
// IXPackageImpl4 adds XPackageGetPackageKind at slot 41.

constexpr GUID kGameSaveClsid = {0x704C3F58, 0xE629, 0x4CC2, {0xB1, 0x97, 0x30, 0x51, 0x1B, 0x99, 0x6F, 0xE2}};
constexpr GUID kGameSaveIid2 = {0x704C3F58, 0xE629, 0x4CC2, {0xB1, 0x97, 0x30, 0x51, 0x1B, 0x99, 0x6E, 0xE2}};
constexpr GUID kGameSaveIid3 = {0x1BFFF3AF, 0xF14A, 0x40A3, {0x8E, 0x35, 0x9A, 0xDA, 0x90, 0x65, 0x93, 0xF9}};
constexpr GUID kPackageClsid = {0xAF406016, 0xE850, 0x4AA8, {0xA8, 0x8D, 0x2F, 0x3D, 0xCB, 0x9D, 0xAC, 0x7E}};
constexpr GUID kPackageIid = {0x3720DE07, 0xE8E4, 0x44A3, {0xAD, 0x32, 0xB3, 0x59, 0xE8, 0xAD, 0xBE, 0x55}};
constexpr GUID kPackageIid2 = {0xF92D8712, 0x2B27, 0x4D8A, {0xBF, 0x01, 0x11, 0xA6, 0xF8, 0xE3, 0xEB, 0x42}};
constexpr GUID kPackageIid3 = {0xE2A4734B, 0x2F4A, 0x456D, {0xAA, 0x8F, 0xD0, 0x65, 0xE0, 0x4F, 0xB2, 0x09}};
constexpr GUID kPackageIid4 = {0x0B9D556E, 0xAB70, 0x43F4, {0xBA, 0xA0, 0x99, 0x99, 0xAC, 0x4C, 0xB0, 0x40}};

HRESULT WINAPI game_save_query_interface(void* self, const GUID* riid, void** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!riid) return E_INVALIDARG;
    if (guid_eq(*riid, kIUnknown) || guid_eq(*riid, kGameSaveClsid) || guid_eq(*riid, kGameSaveIid2) ||
        guid_eq(*riid, kGameSaveIid3)) {
        *out = self;
        return S_OK;
    }
    return E_NOINTERFACE;
}

HRESULT WINAPI package_query_interface(void* self, const GUID* riid, void** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!riid) return E_INVALIDARG;
    if (guid_eq(*riid, kIUnknown) || guid_eq(*riid, kPackageIid) || guid_eq(*riid, kPackageIid2) ||
        guid_eq(*riid, kPackageIid3) || guid_eq(*riid, kPackageIid4)) {
        *out = self;
        return S_OK;
    }
    return E_NOINTERFACE;
}

#define BX_HR(fn, iface, slot, method) \
    HRESULT WINAPI fn(void*) { \
        BX_VTABLE_STUB_LOG(iface, static_cast<uint32_t>(slot), method); \
        return E_NOTIMPL; \
    }
#define BX_VOID(fn, iface, slot, method) \
    void WINAPI fn(void*) { BX_VTABLE_STUB_LOG(iface, static_cast<uint32_t>(slot), method); }
// Bool slots log and return false. The HRESULT bit pattern would look like true to a caller.
#define BX_BOOL(fn, iface, slot, method) \
    bool WINAPI fn(void*) { \
        BX_VTABLE_STUB_LOG(iface, static_cast<uint32_t>(slot), method); \
        return false; \
    }

BX_HR(gs_quota, "IXGameSaveImpl", 7, "XGameSaveGetRemainingQuota")
BX_HR(gs_quota_async, "IXGameSaveImpl", 8, "XGameSaveGetRemainingQuotaAsync")
BX_HR(gs_quota_result, "IXGameSaveImpl", 9, "XGameSaveGetRemainingQuotaResult")
BX_HR(gs_delete, "IXGameSaveImpl", 10, "XGameSaveDeleteContainer")
BX_HR(gs_delete_async, "IXGameSaveImpl", 11, "XGameSaveDeleteContainerAsync")
BX_HR(gs_delete_result, "IXGameSaveImpl", 12, "XGameSaveDeleteContainerResult")
BX_HR(gs_container_info, "IXGameSaveImpl", 13, "XGameSaveGetContainerInfo")
BX_HR(gs_enum_by_name, "IXGameSaveImpl", 15, "XGameSaveEnumerateContainerInfoByName")
BX_HR(gs_enum_blobs, "IXGameSaveImpl", 18, "XGameSaveEnumerateBlobInfo")
BX_HR(gs_enum_blobs_by_name, "IXGameSaveImpl", 19, "XGameSaveEnumerateBlobInfoByName")
BX_HR(gs_blob_delete, "IXGameSaveImpl", 26, "XGameSaveSubmitBlobDelete")
BX_HR(gs_files_folder_async, "IXGameSaveImpl2", 30, "XGameSaveFilesGetFolderWithUiAsync")
BX_HR(gs_files_folder_result, "IXGameSaveImpl2", 31, "XGameSaveFilesGetFolderWithUiResult")
BX_HR(gs_files_quota, "IXGameSaveImpl2", 32, "XGameSaveFilesGetRemainingQuota")

BX_HR(pk_create_monitor, "IXPackageImpl", 5, "XPackageCreateInstallationMonitor")
BX_VOID(pk_close_monitor, "IXPackageImpl", 6, "XPackageCloseInstallationMonitorHandle")
BX_VOID(pk_get_progress, "IXPackageImpl", 7, "XPackageGetInstallationProgress")
BX_BOOL(pk_update_monitor, "IXPackageImpl", 8, "XPackageUpdateInstallationMonitor")
BX_HR(pk_reg_progress, "IXPackageImpl", 9, "XPackageRegisterInstallationProgressChanged")
BX_BOOL(pk_unreg_progress, "IXPackageImpl", 10, "XPackageUnregisterInstallationProgressChanged")
BX_HR(pk_locale, "IXPackageImpl", 11, "XPackageGetUserLocale")
BX_HR(pk_find_chunk, "IXPackageImpl", 12, "XPackageFindChunkAvailability")
BX_HR(pk_enum_chunk, "IXPackageImpl", 13, "XPackageEnumerateChunkAvailability")
BX_HR(pk_change_order, "IXPackageImpl", 14, "XPackageChangeChunkInstallOrder")
BX_HR(pk_install, "IXPackageImpl", 15, "XPackageInstallChunks")
BX_HR(pk_install_async, "IXPackageImpl", 16, "XPackageInstallChunksAsync")
BX_HR(pk_install_result, "IXPackageImpl", 17, "XPackageInstallChunksResult")
BX_HR(pk_estimate, "IXPackageImpl", 18, "XPackageEstimateDownloadSize")
BX_HR(pk_uninstall_chunks, "IXPackageImpl", 19, "XPackageUninstallChunks")
BX_HR(pk_unknown_20, "IXPackageImpl", 20, "unknown")
BX_HR(pk_unknown_21, "IXPackageImpl", 21, "unknown")
BX_BOOL(pk_unreg_installed, "IXPackageImpl", 22, "XPackageUnregisterPackageInstalled")
BX_HR(pk_unknown_27, "IXPackageImpl", 27, "unknown")
BX_HR(pk_enum_packages, "IXPackageImpl", 28, "XPackageEnumeratePackages")
BX_HR(pk_reg_installed, "IXPackageImpl", 29, "XPackageRegisterPackageInstalled")
BX_HR(pk_write_stats, "IXPackageImpl", 30, "XPackageGetWriteStats")
BX_HR(pk_unknown_31, "IXPackageImpl", 31, "unknown")
BX_HR(pk_uninstall_uwp, "IXPackageImpl", 32, "XPackageUninstallUWPInstance")
BX_HR(pk_enum_features, "IXPackageImpl", 33, "XPackageEnumerateFeatures")
BX_HR(pk_uninstall, "IXPackageImpl", 34, "XPackageUninstallPackage")
BX_HR(pk_enum_packages_2, "IXPackageImpl2", 35, "XPackageEnumeratePackages")
BX_HR(pk_reg_installed_2, "IXPackageImpl2", 36, "XPackageRegisterPackageInstalled")
BX_HR(pk_enum_packages_3, "IXPackageImpl3", 39, "XPackageEnumeratePackages")
BX_HR(pk_reg_installed_3, "IXPackageImpl3", 40, "XPackageRegisterPackageInstalled")
#undef BX_HR
#undef BX_VOID
#undef BX_BOOL

const std::array<const void*, 33> kGameSaveVtbl = [] {
    auto v = make_default_vtbl<33>();
    v[0] = reinterpret_cast<const void*>(&game_save_query_interface);
    v[3] = reinterpret_cast<const void*>(&initialize_provider);
    v[4] = reinterpret_cast<const void*>(&initialize_provider_async);
    v[5] = reinterpret_cast<const void*>(&initialize_provider_result);
    v[6] = reinterpret_cast<const void*>(&close_provider);
    v[7] = reinterpret_cast<const void*>(&gs_quota);
    v[8] = reinterpret_cast<const void*>(&gs_quota_async);
    v[9] = reinterpret_cast<const void*>(&gs_quota_result);
    v[10] = reinterpret_cast<const void*>(&gs_delete);
    v[11] = reinterpret_cast<const void*>(&gs_delete_async);
    v[12] = reinterpret_cast<const void*>(&gs_delete_result);
    v[13] = reinterpret_cast<const void*>(&gs_container_info);
    v[14] = reinterpret_cast<const void*>(&enumerate_container_info);
    v[15] = reinterpret_cast<const void*>(&gs_enum_by_name);
    v[16] = reinterpret_cast<const void*>(&create_container);
    v[17] = reinterpret_cast<const void*>(&close_container);
    v[18] = reinterpret_cast<const void*>(&gs_enum_blobs);
    v[19] = reinterpret_cast<const void*>(&gs_enum_blobs_by_name);
    v[20] = reinterpret_cast<const void*>(&read_blob_data);
    v[21] = reinterpret_cast<const void*>(&read_blob_data_async);
    v[22] = reinterpret_cast<const void*>(&read_blob_data_result);
    v[23] = reinterpret_cast<const void*>(&create_update);
    v[24] = reinterpret_cast<const void*>(&close_update);
    v[25] = reinterpret_cast<const void*>(&submit_blob_write);
    v[26] = reinterpret_cast<const void*>(&gs_blob_delete);
    v[27] = reinterpret_cast<const void*>(&submit_update);
    v[28] = reinterpret_cast<const void*>(&submit_update_async);
    v[29] = reinterpret_cast<const void*>(&submit_update_result);
    v[30] = reinterpret_cast<const void*>(&gs_files_folder_async);
    v[31] = reinterpret_cast<const void*>(&gs_files_folder_result);
    v[32] = reinterpret_cast<const void*>(&gs_files_quota);
    return v;
}();
ComSingleton kGameSaveObj{kGameSaveVtbl.data(), &kGameSaveClsid, "IXGameSaveImpl"};

const std::array<const void*, 42> kPackageVtbl = [] {
    auto v = make_default_vtbl<42>();
    v[0] = reinterpret_cast<const void*>(&package_query_interface);
    v[3] = reinterpret_cast<const void*>(&get_current_process_package_identifier);
    v[4] = reinterpret_cast<const void*>(&is_packaged_process);
    v[5] = reinterpret_cast<const void*>(&pk_create_monitor);
    v[6] = reinterpret_cast<const void*>(&pk_close_monitor);
    v[7] = reinterpret_cast<const void*>(&pk_get_progress);
    v[8] = reinterpret_cast<const void*>(&pk_update_monitor);
    v[9] = reinterpret_cast<const void*>(&pk_reg_progress);
    v[10] = reinterpret_cast<const void*>(&pk_unreg_progress);
    v[11] = reinterpret_cast<const void*>(&pk_locale);
    v[12] = reinterpret_cast<const void*>(&pk_find_chunk);
    v[13] = reinterpret_cast<const void*>(&pk_enum_chunk);
    v[14] = reinterpret_cast<const void*>(&pk_change_order);
    v[15] = reinterpret_cast<const void*>(&pk_install);
    v[16] = reinterpret_cast<const void*>(&pk_install_async);
    v[17] = reinterpret_cast<const void*>(&pk_install_result);
    v[18] = reinterpret_cast<const void*>(&pk_estimate);
    v[19] = reinterpret_cast<const void*>(&pk_uninstall_chunks);
    v[20] = reinterpret_cast<const void*>(&pk_unknown_20);
    v[21] = reinterpret_cast<const void*>(&pk_unknown_21);
    v[22] = reinterpret_cast<const void*>(&pk_unreg_installed);
    v[23] = reinterpret_cast<const void*>(&mount);
    v[24] = reinterpret_cast<const void*>(&get_mount_path_size);
    v[25] = reinterpret_cast<const void*>(&get_mount_path);
    v[26] = reinterpret_cast<const void*>(&close_mount_handle);
    v[27] = reinterpret_cast<const void*>(&pk_unknown_27);
    v[28] = reinterpret_cast<const void*>(&pk_enum_packages);
    v[29] = reinterpret_cast<const void*>(&pk_reg_installed);
    v[30] = reinterpret_cast<const void*>(&pk_write_stats);
    v[31] = reinterpret_cast<const void*>(&pk_unknown_31);
    v[32] = reinterpret_cast<const void*>(&pk_uninstall_uwp);
    v[33] = reinterpret_cast<const void*>(&pk_enum_features);
    v[34] = reinterpret_cast<const void*>(&pk_uninstall);
    v[35] = reinterpret_cast<const void*>(&pk_enum_packages_2);
    v[36] = reinterpret_cast<const void*>(&pk_reg_installed_2);
    v[37] = reinterpret_cast<const void*>(&mount_with_ui_async);
    v[38] = reinterpret_cast<const void*>(&mount_with_ui_result);
    v[39] = reinterpret_cast<const void*>(&pk_enum_packages_3);
    v[40] = reinterpret_cast<const void*>(&pk_reg_installed_3);
    v[41] = reinterpret_cast<const void*>(&get_package_kind);
    return v;
}();
ComSingleton kPackageObj{kPackageVtbl.data(), &kPackageIid4, "IXPackageImpl"};

struct PublicEntry {
    const GUID* clsid;
    const GUID* iid;
    ComSingleton* obj;
};

const PublicEntry kPublicEntries[] = {
    {&kCoreGuid, &kCoreGuid, &kCoreObj},
    {&kFeatureGuid, &kFeatureGuid, &kFeatureObj},
    {&kSystemClsid, &kSystemIid, &kSystemObj},
    {&kAnalyticsGuid, &kAnalyticsGuid, &kAnalyticsObj},
    {&kDisplayClsid, &kDisplayIid, &kDisplayObj},
    {&kErrorGuid, &kErrorGuid, &kErrorObj},
    {&kPlsClsid, &kPlsIid, &kPlsObj},
    {&kUserClsid, &kUserClsid, &kUserObj},
    {&kUserClsid, &kUserGamertagIid, &kUserGamertagObj},
    {&kStoreClsid, &kStoreIid, &kStoreObj},
    {&kStoreClsid, &kStoreDurablesIid, &kStoreDurablesObj},
    {&kGameUiClsid, &kGameUiIid, &kGameUiObj},
    {&kNetworkingClsid, &kNetworkingIid, &kNetworkingObj},
    {&kGameClsid, &kGameIid, &kGameObj},
    {&kGameSaveClsid, &kGameSaveClsid, &kGameSaveObj},
    {&kGameSaveClsid, &kGameSaveIid2, &kGameSaveObj},
    {&kGameSaveClsid, &kGameSaveIid3, &kGameSaveObj},
    {&kPackageClsid, &kPackageIid, &kPackageObj},
    {&kPackageClsid, &kPackageIid2, &kPackageObj},
    {&kPackageClsid, &kPackageIid3, &kPackageObj},
    {&kPackageClsid, &kPackageIid4, &kPackageObj},
};

} // namespace

bool query_public_gdk_interface(const GUID* clsid, const GUID* iid, void** out, bool* clsid_known) {
    *clsid_known = false;
    for (const auto& entry : kPublicEntries) {
        if (guid_eq(*clsid, *entry.clsid)) {
            *clsid_known = true;
            if (guid_eq(*iid, *entry.iid) || guid_eq(*iid, kIUnknown)) {
                *out = entry.obj;
                char detail[96];
                std::snprintf(detail, sizeof(detail), "dispatch public_com=%s", entry.obj->name);
                bx_log_emit(BX_LOG_INFO, "xgameruntime", "QueryApiImpl", detail);
                return true;
            }
        }
    }
    return false;
}

} // namespace bx::gdk
