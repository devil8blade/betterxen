/*
 * Xbox frame pacing on the host (ADR 0009): ID3D12Device::SetFrameIntervalX, ScheduleFrameEventX
 * and WaitFrameEventX, as the authorized GDK documentation describes them.
 *
 * On the console, display hardware signals the frame events. Here one thread emulates them
 * from the recorded frame interval: at the start of each interval it signals the VBLANK and
 * FLIP events, and IntervalOffsetInMicroseconds later the ORIGIN event, which also produces a
 * new frame pipeline token. Each signal also sets the ancillary events given to
 * ScheduleFrameEventX. Timing follows the host's Sleep granularity; no display is involved.
 */
#include "xbox_facade.h"

#include <cstdio>
#include <mutex>
#include <thread>

namespace {

constexpr UINT kEventTypes = 3;                // ORIGIN, DISPLAY_VBLANK, DISPLAY_FLIP
constexpr UINT kOrigin = 0;
constexpr UINT kMaxAncillarySignals = 7;
constexpr UINT kMaxAncillaryWaits = 63;
constexpr UINT kScheduleDisableSignal = 0x1;
constexpr UINT kWaitAll = 0x1;
constexpr UINT kWaitProcessTokenOnTimeout = 0x2;
constexpr UINT kIntervalFlag60FpsPlus = 0x1;
constexpr UINT kSignaledNone = 0xFFFFFFFF;
constexpr UINT kInfinite = 0xFFFFFFFF;

struct Scheduled {
    bool active = false;
    UINT offset_us = 0;
    HANDLE ancillary[kMaxAncillarySignals] = {};
    UINT ancillary_count = 0;
};

std::mutex g_lock;
UINT g_interval_us = 0;
UINT g_period = 0;
Scheduled g_events[kEventTypes];
HANDLE g_signal[kEventTypes] = {};             // auto-reset, set at each occurrence
UINT64 g_next_token = 1;
UINT64 g_last_origin_token = 0;
bool g_thread_started = false;

void close_ancillary(Scheduled& e) {
    for (UINT i = 0; i < e.ancillary_count; ++i) CloseHandle(e.ancillary[i]);
    e.ancillary_count = 0;
}

void signal(UINT type) {
    std::lock_guard<std::mutex> guard(g_lock);
    const Scheduled& e = g_events[type];
    if (!e.active) return;
    if (type == kOrigin) g_last_origin_token = g_next_token++;
    SetEvent(g_signal[type]);
    for (UINT i = 0; i < e.ancillary_count; ++i) SetEvent(e.ancillary[i]);
}

void pacing_thread() {
    LARGE_INTEGER frequency, start;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start);
    const auto now_us = [&] {
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        return static_cast<UINT64>((t.QuadPart - start.QuadPart) * 1000000 / frequency.QuadPart);
    };
    const auto sleep_until = [&](UINT64 target_us) {
        for (UINT64 t = now_us(); t < target_us; t = now_us()) {
            const UINT64 ms = (target_us - t) / 1000;
            Sleep(ms ? static_cast<DWORD>(ms) : 0);
        }
    };
    UINT64 interval_start = 0;
    for (;;) {
        UINT interval, origin_offset;
        {
            std::lock_guard<std::mutex> guard(g_lock);
            interval = g_interval_us;
            origin_offset = g_events[kOrigin].offset_us;
        }
        signal(1);
        signal(2);
        sleep_until(interval_start + (origin_offset < interval ? origin_offset : 0));
        signal(kOrigin);
        interval_start += interval;
        sleep_until(interval_start);
    }
}

void log_pacing(const char* event, const char* fmt, UINT a, UINT b, UINT c) {
    char detail[160];
    std::snprintf(detail, sizeof(detail), fmt, a, b, c);
    bx_log_emit(BX_LOG_INFO, "d3d12_xs", event, detail);
}

} // namespace

extern "C" {

HRESULT STDMETHODCALLTYPE bx_xbox_device_set_frame_interval_x(void*, IDXGIOutput*, UINT length_us, UINT period,
                                                              UINT flags) {
    BX_D3D_TRACE("XboxDevice", "SetFrameIntervalX");
    if (length_us == 0 || period < 1 || period > 4 || (flags & ~kIntervalFlag60FpsPlus) != 0) return E_INVALIDARG;
    std::lock_guard<std::mutex> guard(g_lock);
    g_interval_us = length_us;
    g_period = period;
    log_pacing("frame_interval", "length_us=%u period=%u flags=0x%x (host timer, no display sync)", length_us, period, flags);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE bx_xbox_device_schedule_frame_event_x(void*, UINT type, UINT offset_us,
                                                                const BxXboxFrameObjectList* signals, UINT flags) {
    BX_D3D_TRACE("XboxDevice", "ScheduleFrameEventX");
    if (type >= kEventTypes || (flags & ~kScheduleDisableSignal) != 0) return E_INVALIDARG;
    if (signals && (signals->Count > kMaxAncillarySignals || (signals->Count && !signals->pObjects))) return E_INVALIDARG;
    std::lock_guard<std::mutex> guard(g_lock);
    if (g_interval_us == 0) return E_INVALIDARG;   // documented: an interval must be established first
    if (offset_us >= g_interval_us) return E_INVALIDARG;
    for (UINT i = 0; i < kEventTypes; ++i) {
        if (!g_signal[i] && !(g_signal[i] = CreateEventW(nullptr, FALSE, FALSE, nullptr))) return E_OUTOFMEMORY;
    }
    Scheduled& e = g_events[type];
    close_ancillary(e);
    e.active = !(flags & kScheduleDisableSignal);
    e.offset_us = offset_us;
    for (UINT i = 0; signals && i < signals->Count; ++i) {
        // The title may close its handle; the scheduled signal keeps its own copy.
        if (!DuplicateHandle(GetCurrentProcess(), signals->pObjects[i], GetCurrentProcess(), &e.ancillary[e.ancillary_count],
                             0, FALSE, DUPLICATE_SAME_ACCESS)) {
            close_ancillary(e);
            return E_INVALIDARG;
        }
        ++e.ancillary_count;
    }
    log_pacing("frame_event_scheduled", "type=%u offset_us=%u ancillary=%u", type, offset_us, e.ancillary_count);
    if (!g_thread_started) {
        std::thread(pacing_thread).detach();
        g_thread_started = true;
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE bx_xbox_device_wait_frame_event_x(void*, UINT type, UINT timeout_ms,
                                                            const BxXboxFrameWaitList* waits, UINT flags,
                                                            UINT64* token) {
    BX_D3D_TRACE("XboxDevice", "WaitFrameEventX");
    if (type >= kEventTypes || (flags & ~(kWaitAll | kWaitProcessTokenOnTimeout)) != 0) return E_INVALIDARG;
    if (waits && (waits->Count > kMaxAncillaryWaits || (waits->Count && !waits->pObjects))) return E_INVALIDARG;
    HANDLE handles[1 + kMaxAncillaryWaits];
    {
        std::lock_guard<std::mutex> guard(g_lock);
        if (!g_events[type].active) return E_INVALIDARG;   // documented: the event must be scheduled
        handles[0] = g_signal[type];
    }
    const UINT ancillary = waits ? waits->Count : 0;
    for (UINT i = 0; i < ancillary; ++i) handles[1 + i] = waits->pObjects[i];
    const DWORD result = WaitForMultipleObjects(1 + ancillary, handles, (flags & kWaitAll) != 0,
                                                timeout_ms == kInfinite ? INFINITE : timeout_ms);
    const bool satisfied = result < WAIT_OBJECT_0 + 1 + ancillary;
    if (waits && waits->pSignaledObjectIndex) {
        *waits->pSignaledObjectIndex = satisfied && !(flags & kWaitAll) && result > WAIT_OBJECT_0
            ? result - WAIT_OBJECT_0 - 1 : kSignaledNone;
    }
    if (type == kOrigin && token && (satisfied || (flags & kWaitProcessTokenOnTimeout))) {
        std::lock_guard<std::mutex> guard(g_lock);
        *token = satisfied ? g_last_origin_token : g_next_token++;
    }
    return satisfied ? S_OK : S_FALSE;
}

} // extern "C"
