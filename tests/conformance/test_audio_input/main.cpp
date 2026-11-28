#include <windows.h>
#include <unknwn.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

extern "C" {
HRESULT WINAPI AcpHalCreate(void**);
HRESULT WINAPI AcpHalAllocateShapeContexts(void*);
HRESULT WINAPI AcpHalReleaseShapeContexts(void);
HRESULT WINAPI ApuCreateHeap(uint32_t, uint32_t);
HRESULT WINAPI ApuAlloc(void**, uint32_t*, uint32_t, uint32_t);
HRESULT WINAPI ApuFree(void*);
BOOL WINAPI ApuIsVirtualAddressValid(const void*, uint32_t);
void* WINAPI ApuMapApuAddress(uint32_t);
uint32_t WINAPI ApuMapVirtualAddress(const void*);
HRESULT WINAPI XApuConnect(void**);
HRESULT WINAPI XApuDisconnect(void*);
HRESULT WINAPI XApuEnqueueCommand(void*, const void*, uint32_t);
HRESULT WINAPI XApuDequeueResult(void*, void*, uint32_t);
uint32_t WINAPI XApuGetResultCount(void*);
HRESULT WINAPI XDspConnectWithMaximumStreamLimit(uint32_t, void**);
HRESULT WINAPI XDspActivate(void*);
HRESULT WINAPI XDspDeactivate(void*);
HRESULT WINAPI XDspSubmitCommand(void*, const void*, uint32_t);
HRESULT WINAPI XDspDisconnect(void*);
}

#define CHECK(expr) do { if (!(expr)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #expr); return 1; } } while (0)

namespace {
void WINAPI dummy_device_callback(uint64_t, void*, void*, uint64_t, uint32_t, uint32_t) {}

bool file_contains(const char* path, uint64_t offset, const char* needle_a, const char* needle_b) {
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        return false;
    }
    bool found = false;
    LARGE_INTEGER size = {};
    if (GetFileSizeEx(f, &size) && size.QuadPart >= 0 &&
        offset <= static_cast<uint64_t>(size.QuadPart) &&
        static_cast<uint64_t>(size.QuadPart) - offset < (64ull << 20)) {
        LARGE_INTEGER pos{};
        pos.QuadPart = static_cast<LONGLONG>(offset);
        if (!SetFilePointerEx(f, pos, nullptr, FILE_BEGIN)) {
            CloseHandle(f);
            return false;
        }
        const DWORD remaining = static_cast<DWORD>(static_cast<uint64_t>(size.QuadPart) - offset);
        auto* buf = static_cast<char*>(HeapAlloc(GetProcessHeap(), 0, static_cast<SIZE_T>(remaining) + 1));
        DWORD total = 0;
        while (buf && total < remaining) {
            DWORD got = 0;
            if (!ReadFile(f, buf + total, remaining - total, &got, nullptr) || got == 0) break;
            total += got;
        }
        if (buf && total == remaining) {
            buf[total] = '\0';
            /* Both needles must sit on the same JSONL line. */
            for (char* line = buf; line && *line && !found;) {
                char* end = std::strchr(line, '\n');
                if (end) *end = '\0';
                found = std::strstr(line, needle_a) && std::strstr(line, needle_b);
                line = end ? end + 1 : nullptr;
            }
        }
        if (buf) HeapFree(GetProcessHeap(), 0, buf);
    }
    CloseHandle(f);
    return found;
}
} // namespace


// Read the JSONL log the shim wrote, so the dispatcher's live-reference
// count can be checked rather than assumed. The log is the only place the
// count is observable: `Release` returning 0 proves the reference count
// reached zero, and the `dispatch_mode` lines prove the live-dispatcher
// counter followed it. They do not prove the object was freed: `Release`
// updates the counter before `delete this`, so a leak would not show here.
// `bx-cli run` appends to the supplied BX_LOG_FILE, so a re-run with the
// same log can pass from stale lines even if this run emits none. Capture
// the file's size before the run and assert only on the bytes appended by
// this run.
static uint64_t log_file_size(const char* path) {
    if (!path || !*path) return 0;
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return 0;
    LARGE_INTEGER size{};
    const BOOL ok = GetFileSizeEx(f, &size);
    CloseHandle(f);
    return ok ? static_cast<uint64_t>(size.QuadPart) : 0;
}

static std::string read_log_file_from(const char* path, uint64_t offset) {
    std::string text;
    if (!path || !*path) return text;
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return text;
    LARGE_INTEGER pos{};
    pos.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(f, pos, nullptr, FILE_BEGIN)) {
        CloseHandle(f);
        return text;
    }
    char buf[4096];
    DWORD got = 0;
    while (ReadFile(f, buf, sizeof(buf), &got, nullptr) && got > 0) {
        text.append(buf, got);
    }
    CloseHandle(f);
    return text;
}

int main() {
    // The dispatcher checks below read the shim's JSONL log, so BX_LOG_FILE
    // must name it before the process starts (`bx-cli run --log`). Setting it
    // here would be too late: AcpHal.dll imports bx_runtime_veh.dll, which
    // reads BX_LOG_FILE once in DLL_PROCESS_ATTACH, before main runs.
    char dispatcher_log_path[MAX_PATH]{};
    const DWORD n = GetEnvironmentVariableA("BX_LOG_FILE", dispatcher_log_path, sizeof(dispatcher_log_path));
    if (n == 0 || n >= sizeof(dispatcher_log_path)) {
        std::fprintf(stderr, "FAIL: BX_LOG_FILE must be set before launch (run under bx-cli run --log)\n");
        return 1;
    }
    const uint64_t log_start = log_file_size(dispatcher_log_path);

    void* object = nullptr;
    CHECK(AcpHalCreate(&object) == S_OK && object);
    auto* hal = static_cast<IUnknown*>(object);
    CHECK(hal->QueryInterface(IID_IUnknown, &object) == S_OK && object == hal);
    CHECK(hal->Release() == 1);
    void *va = nullptr, *second = nullptr;
    uint32_t base = 0, other = 0;
    CHECK(ApuAlloc(&va, &base, 64, 64) == E_OUTOFMEMORY && !va && !base);
    // A zero-capacity heap must not report S_OK: it can never satisfy an
    // allocation (#580). ApuAlloc still refuses with E_OUTOFMEMORY.
    CHECK(ApuCreateHeap(0, 0) == E_INVALIDARG);
    CHECK(ApuAlloc(&va, &base, 64, 64) == E_OUTOFMEMORY && !va && !base);
    CHECK(ApuCreateHeap(64, 64) == S_OK);
    CHECK(ApuAlloc(&va, nullptr, 64, 64) == S_OK && va);
    CHECK(ApuFree(va) == S_OK);
    va = nullptr;
    CHECK(ApuAlloc(nullptr, &base, 64, 64) == S_OK && base);
    va = ApuMapApuAddress(base);
    CHECK(va && ApuFree(va) == S_OK);
    va = nullptr;
    base = 0;
    CHECK(ApuAlloc(&va, &base, 64, 256) == S_OK && va && base);
    CHECK(reinterpret_cast<uintptr_t>(va) % 256 == 0 && base % 256 == 0);
    CHECK(ApuAlloc(&second, &other, 64, 64) == S_OK && other != base);
    CHECK(ApuCreateHeap(0, 0) == HRESULT_FROM_WIN32(ERROR_BUSY));
    void* excess = nullptr;
    CHECK(ApuAlloc(&excess, nullptr, 1, 64) == E_OUTOFMEMORY);
    auto* tail = static_cast<unsigned char*>(va) + 63;
    CHECK(ApuMapApuAddress(base + 63) == tail && ApuMapVirtualAddress(tail) == base + 63);
    CHECK(ApuIsVirtualAddressValid(tail, 1) && !ApuIsVirtualAddressValid(tail, 2));
    CHECK(ApuFree(tail) == E_INVALIDARG);
    CHECK(ApuFree(va) == S_OK && !ApuMapApuAddress(base) && !ApuMapVirtualAddress(va));
    CHECK(ApuFree(va) == E_INVALIDARG && ApuMapApuAddress(other) == second);
    CHECK(ApuFree(second) == S_OK && hal->Release() == 0);

    // AcpHal shape-context stubs: NDA-gated contract, so BetterXen returns
    // E_NOTIMPL and logs the symbol. Null and sentinel pointers must not crash.
    CHECK(AcpHalAllocateShapeContexts(nullptr) == E_NOTIMPL);
    CHECK(AcpHalAllocateShapeContexts(reinterpret_cast<void*>(uintptr_t{0xDEADBEEF})) == E_NOTIMPL);
    CHECK(AcpHalReleaseShapeContexts() == E_NOTIMPL);
    CHECK(AcpHalReleaseShapeContexts() == E_NOTIMPL);

    char log_path[MAX_PATH];
    DWORD log_len = GetEnvironmentVariableA("BX_LOG_FILE", log_path, sizeof(log_path));
    if (log_len > 0 && log_len < sizeof(log_path)) {
        CHECK(file_contains(log_path, log_start, "\"event\":\"unimpl_api\"", "AcpHal.dll!AcpHalAllocateShapeContexts"));
        CHECK(file_contains(log_path, log_start, "\"event\":\"unimpl_api\"", "AcpHal.dll!AcpHalReleaseShapeContexts"));
    } else {
        // Unreachable under `bx-cli run --log`. A SKIP that still exits 0 would
        // contradict the exit-77 protocol, so a missing log is a failure.
        std::fprintf(stderr, "FAIL: BX_LOG_FILE must be set before launch (run under bx-cli run --log)\n");
        return 1;
    }

    void *apu = nullptr, *peer = nullptr, *dsp = nullptr;
    CHECK(XApuConnect(&apu) == S_OK && XApuConnect(&peer) == S_OK && peer != apu);
    unsigned result = 0;
    CHECK(XApuDequeueResult(apu, &result, sizeof(result)) == S_FALSE);
    for (unsigned cycle = 0; cycle < 3; ++cycle) {
        HRESULT status = S_OK;
        unsigned accepted = 0;
        std::thread producer([&] {
            for (; accepted < 1024; ++accepted) {
                unsigned command = cycle * 1024 + accepted;
                status = XApuEnqueueCommand(apu, &command, sizeof(command));
                if (status != S_OK) break;
            }
        });
        producer.join();
        CHECK(accepted && status == HRESULT_FROM_WIN32(ERROR_BUSY));
        CHECK(XApuGetResultCount(apu) == accepted && XApuGetResultCount(peer) == 0);
        CHECK(XApuDequeueResult(apu, &result, 1) == HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER));
        for (unsigned i = 0; i < accepted; ++i) {
            CHECK(XApuDequeueResult(apu, &result, sizeof(result)) == S_OK);
            CHECK(result == cycle * 1024 + i);
        }
        CHECK(XApuGetResultCount(apu) == 0);
    }
    // A stalled peer must fail this test rather than wait forever.
    constexpr unsigned messages = 4096;
    const auto deadline = GetTickCount64() + 10000;
    std::atomic<bool> failed{false};
    std::thread worker([&] {
        for (unsigned i = 0; i < messages && !failed;) {
            const auto hr = XApuEnqueueCommand(apu, &i, sizeof(i));
            if (hr == S_OK) ++i;
            else if (hr != HRESULT_FROM_WIN32(ERROR_BUSY) || GetTickCount64() >= deadline)
                failed = true;
            else std::this_thread::yield();
        }
    });
    for (unsigned i = 0; i < messages && !failed;) {
        const auto hr = XApuDequeueResult(apu, &result, sizeof(result));
        if (hr == S_OK && result == i) ++i;
        else if (hr != S_FALSE || GetTickCount64() >= deadline) failed = true;
        else std::this_thread::yield();
    }
    worker.join();
    CHECK(!failed && XApuGetResultCount(apu) == 0);
    CHECK(XApuDisconnect(apu) == S_OK && XApuDisconnect(peer) == S_OK);
    CHECK(XApuEnqueueCommand(apu, &result, sizeof(result)) == E_HANDLE);

    CHECK(XDspConnectWithMaximumStreamLimit(0, &dsp) == E_INVALIDARG && !dsp);
    CHECK(XDspConnectWithMaximumStreamLimit(2, &dsp) == S_OK && dsp);
    for (unsigned cycle = 0; cycle < 2; ++cycle) {
        unsigned queued = 0;
        HRESULT hr = S_OK;
        while (queued < 1024 && (hr = XDspSubmitCommand(dsp, &result, sizeof(result))) == S_OK)
            ++queued;
        CHECK(queued && hr == HRESULT_FROM_WIN32(ERROR_BUSY));
        CHECK(XDspActivate(dsp) == S_OK);
        for (unsigned i = 0; i <= queued; ++i)
            CHECK(XDspSubmitCommand(dsp, &result, sizeof(result)) == S_OK);
        CHECK(XDspDeactivate(dsp) == S_OK);
    }
    CHECK(XDspDisconnect(dsp) == S_OK && XDspActivate(dsp) == E_HANDLE);

    HMODULE dll = LoadLibraryW(L"GameInput.dll");
    CHECK(dll);
    auto proc = GetProcAddress(dll, MAKEINTRESOURCEA(1));
    CHECK(proc && proc == GetProcAddress(dll, "GameInputCreate"));
    using Factory = HRESULT (WINAPI*)(void**);
    auto create = reinterpret_cast<Factory>(reinterpret_cast<void*>(proc));
    CHECK(create(&object) == S_OK && object);
    auto* input = static_cast<IUnknown*>(object);
    CHECK(create(&object) == S_OK && object == input && input->Release() == 1);
    CHECK(input->QueryInterface(IID_IUnknown, &object) == S_OK && object == input);
    CHECK(input->Release() == 1);
    constexpr GUID kGameInputV0 = {0x11be2a7e, 0x4254, 0x445a,
                                   {0x9c, 0x09, 0xff, 0xc4, 0x0f, 0x00, 0x69, 0x18}};
    constexpr GUID kUnknownIid = {0x00000001, 0x0002, 0x0003,
                                  {0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b}};
    void* unknown = reinterpret_cast<void*>(uintptr_t{1});
    CHECK(input->QueryInterface(kUnknownIid, &unknown) == E_NOINTERFACE && !unknown);
    void* v0_obj = nullptr;
    CHECK(input->QueryInterface(kGameInputV0, &v0_obj) == S_OK && v0_obj == input);
    auto* v0 = static_cast<IUnknown*>(v0_obj);
    auto** table = *reinterpret_cast<void***>(v0);
    using Timestamp = uint64_t (WINAPI*)(IUnknown*);
    CHECK(reinterpret_cast<Timestamp>(table[3])(v0) > 0);
    using Reading = HRESULT (WINAPI*)(IUnknown*, uint32_t, void*, void**);
    void* reading = reinterpret_cast<void*>(uintptr_t{1});
    CHECK(reinterpret_cast<Reading>(table[4])(v0, 0, nullptr, &reading) == E_NOTIMPL && !reading);

    using RegisterDeviceCb = HRESULT (WINAPI*)(
        IUnknown*, void*, uint32_t, uint32_t, uint32_t, void*, void*, uint64_t*);
    using StopCb = void (WINAPI*)(IUnknown*, uint64_t);
    using UnregisterCb = bool (WINAPI*)(IUnknown*, uint64_t, uint64_t);
    using SetFocusPolicyFn = void (WINAPI*)(IUnknown*, uint32_t);
    auto reg_dev = reinterpret_cast<RegisterDeviceCb>(table[9]);
    auto stop_cb = reinterpret_cast<StopCb>(table[12]);
    auto unreg_cb = reinterpret_cast<UnregisterCb>(table[13]);
    auto set_focus = reinterpret_cast<SetFocusPolicyFn>(table[21]);
    void* cb_fn = reinterpret_cast<void*>(&dummy_device_callback);

    uint64_t tok1 = 99, tok2 = 0;
    CHECK(reg_dev(v0, nullptr, 1, 1, 0, nullptr, nullptr, &tok1) == E_INVALIDARG && tok1 == 0);
    tok1 = 99;
    CHECK(reg_dev(v0, nullptr, 0, 1, 0, nullptr, cb_fn, &tok1) == E_INVALIDARG && tok1 == 0);
    tok1 = 99;
    CHECK(reg_dev(v0, nullptr, 1, 0, 0, nullptr, cb_fn, &tok1) == E_INVALIDARG && tok1 == 0);
    tok1 = 99;
    CHECK(reg_dev(v0, nullptr, 1, 1, 3, nullptr, cb_fn, &tok1) == E_INVALIDARG && tok1 == 0);

    CHECK(reg_dev(v0, nullptr, 0x10000, 0x1, 2, &tok1, cb_fn, &tok1) == S_OK && tok1 != 0);
    CHECK(reg_dev(v0, nullptr, 0x10000, 0x1, 1, nullptr, cb_fn, &tok2) == S_OK && tok2 > tok1);
    CHECK(reg_dev(v0, nullptr, 0x10000, 0x1, 0, nullptr, cb_fn, nullptr) == S_OK);

    CHECK(!unreg_cb(v0, 0, 5000));
    CHECK(!unreg_cb(v0, 0xdeadbeefULL, 5000));
    stop_cb(v0, tok1);
    CHECK(unreg_cb(v0, tok1, 5000));
    CHECK(!unreg_cb(v0, tok1, 5000));
    CHECK(unreg_cb(v0, tok2, 0));
    CHECK(!unreg_cb(v0, tok2, 0));
    set_focus(v0, 0x2);

    // Slot 14: IGameInput::CreateDispatcher. The singleton refcount is checked
    // below via the pre-existing GameInput release sequence, which must be
    // unchanged by dispatcher creation.
    using CreateDispatcherFn = HRESULT (WINAPI*)(IUnknown*, void**);
    using DispatchFn = bool (WINAPI*)(IUnknown*, uint64_t);
    using OpenWaitHandleFn = HRESULT (WINAPI*)(IUnknown*, HANDLE*);
    auto create_dispatcher = reinterpret_cast<CreateDispatcherFn>(table[14]);
    CHECK(create_dispatcher(v0, nullptr) == E_POINTER);
    void* dispatcher = reinterpret_cast<void*>(uintptr_t{1});
    CHECK(create_dispatcher(v0, &dispatcher) == S_OK && dispatcher);
    auto* disp_unknown = static_cast<IUnknown*>(dispatcher);
    auto** disp_table = *reinterpret_cast<void***>(disp_unknown);
    void* disp_qi = nullptr;
    CHECK(reinterpret_cast<HRESULT (WINAPI*)(IUnknown*, REFIID, void**)>(disp_table[0])(
              disp_unknown, IID_IUnknown, &disp_qi) == S_OK && disp_qi == dispatcher);
    CHECK(disp_unknown->Release() == 1);
    void* disp_other = reinterpret_cast<void*>(uintptr_t{1});
    CHECK(reinterpret_cast<HRESULT (WINAPI*)(IUnknown*, REFIID, void**)>(disp_table[0])(
              disp_unknown, kUnknownIid, &disp_other) == E_NOINTERFACE && !disp_other);
    auto dispatch = reinterpret_cast<DispatchFn>(disp_table[3]);
    CHECK(!dispatch(disp_unknown, 0));
    CHECK(!dispatch(disp_unknown, 1000000));
    auto open_wait = reinterpret_cast<OpenWaitHandleFn>(disp_table[4]);
    CHECK(open_wait(disp_unknown, nullptr) == E_POINTER);
    HANDLE wait1 = nullptr, wait2 = nullptr;
    CHECK(open_wait(disp_unknown, &wait1) == S_OK && wait1);
    CHECK(WaitForSingleObject(wait1, 0) == WAIT_TIMEOUT);
    CHECK(open_wait(disp_unknown, &wait2) == S_OK && wait2 && wait2 != wait1);
    CHECK(CloseHandle(wait1) && CloseHandle(wait2));
    CHECK(disp_unknown->Release() == 0);

    // `dispatch_mode` logs each manual/automatic transition with the live
    // dispatcher count. These checks catch a Release that never calls
    // `on_dispatcher_released` (no `live_dispatchers=0` line). They cannot
    // catch a missing `delete this`, which runs after the counter update.
    {
        const std::string log = read_log_file_from(dispatcher_log_path, log_start);
        CHECK(log.find("live_dispatchers=1") != std::string::npos);
        CHECK(log.find("live_dispatchers=0") != std::string::npos);
        CHECK(log.find("mode=manual") != std::string::npos);
        CHECK(log.find("mode=automatic") != std::string::npos);
    }

    CHECK(v0->Release() == 1);
    CHECK(input->Release() == 0);
    FreeLibrary(dll);
    std::puts("PASS: audio mappings, APU and DSP queues, GameInput ordinal 1, device callbacks, dispatcher");
}
