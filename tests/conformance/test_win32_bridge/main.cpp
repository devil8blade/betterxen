#include <windows.h>
#include <array>
#include <atomic>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <unordered_set>

namespace {

constexpr UINT kMessage = WM_APP + 1;
constexpr WPARAM kPayload = 42;
decltype(&DefWindowProcA) g_def_window_proc;
bool g_received_message = false;
std::atomic<bool> g_callback_called{false};

struct ScopedModule {
    HMODULE handle = nullptr;
    ScopedModule() = default;
    explicit ScopedModule(HMODULE h) : handle(h) {}
    ~ScopedModule() {
        if (handle) {
            FreeLibrary(handle);
        }
    }
    ScopedModule(const ScopedModule&) = delete;
    ScopedModule& operator=(const ScopedModule&) = delete;
    explicit operator bool() const { return handle != nullptr; }
    HMODULE get() const { return handle; }
};

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == kMessage) {
        g_received_message = wparam == kPayload;
        return 0;
    }
    return g_def_window_proc(window, message, wparam, lparam);
}

void CALLBACK notification_callback(BOOLEAN, PVOID) {
    g_callback_called.store(true);
}

HMODULE load_bridge(const wchar_t* dll) {
    wchar_t executable[MAX_PATH];
    const DWORD length = GetModuleFileNameW(nullptr, executable, MAX_PATH);
    if (!length || length == MAX_PATH) {
        return nullptr;
    }
    const std::wstring path(executable, length);
    const auto expected = path.substr(0, path.find_last_of(L"\\/") + 1) + dll;
    HMODULE module = LoadLibraryW(expected.c_str());
    if (!module) {
        std::fwprintf(stderr, L"FAIL: could not load built bridge %ls (error %lu)\n",
                      expected.c_str(), GetLastError());
        return nullptr;
    }
    wchar_t actual[MAX_PATH];
    const DWORD actual_length = GetModuleFileNameW(module, actual, MAX_PATH);
    if (!actual_length || actual_length == MAX_PATH) {
        const DWORD err = GetLastError();
        FreeLibrary(module);
        std::fwprintf(stderr, L"FAIL: could not query path for bridge %ls (error %lu)\n",
                      expected.c_str(), err);
        return nullptr;
    }
    if (CompareStringOrdinal(expected.c_str(), -1, actual, -1, TRUE) != CSTR_EQUAL) {
        FreeLibrary(module);
        std::fwprintf(stderr, L"FAIL: bridge %ls resolved to unexpected path %ls\n",
                      expected.c_str(), actual);
        return nullptr;
    }
    return module;
}

template<typename Function>
Function lookup(HMODULE module, const char* name) {
    return std::bit_cast<Function>(GetProcAddress(module, name));
}

bool test_window() {
    const ScopedModule window(load_bridge(L"ext-ms-win-rtcore-ntuser-window-l1-1-0.dll"));
    const ScopedModule window_ansi(load_bridge(L"ext-ms-win-rtcore-ntuser-window-ansi-l1-1-0.dll"));
    const ScopedModule message(load_bridge(L"ext-ms-win-rtcore-ntuser-message-l1-1-0.dll"));
    const ScopedModule message_ansi(load_bridge(L"ext-ms-win-rtcore-ntuser-message-ansi-l1-1-0.dll"));
    if (!window || !window_ansi || !message || !message_ansi) {
        return false;
    }

    const auto show = lookup<decltype(&ShowWindow)>(window.get(), "ShowWindow");
    const auto create = lookup<decltype(&CreateWindowExA)>(window_ansi.get(), "CreateWindowExA");
    const auto register_class = lookup<decltype(&RegisterClassExA)>(window_ansi.get(), "RegisterClassExA");
    g_def_window_proc = lookup<decltype(&DefWindowProcA)>(window_ansi.get(), "DefWindowProcA");
    const auto translate = lookup<decltype(&TranslateMessage)>(message.get(), "TranslateMessage");
    const auto peek = lookup<decltype(&PeekMessageA)>(message_ansi.get(), "PeekMessageA");
    const auto dispatch = lookup<decltype(&DispatchMessageA)>(message_ansi.get(), "DispatchMessageA");
    if (!show || !create || !register_class || !g_def_window_proc || !translate || !peek || !dispatch) {
        return false;
    }

    WNDCLASSEXA cls{};
    cls.cbSize = sizeof(cls);
    cls.lpfnWndProc = window_proc;
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpszClassName = "BetterXenBridgeConformance";
    if (!register_class(&cls)) {
        return false;
    }
    const HWND hwnd = create(0, cls.lpszClassName, "BetterXen Win32 Bridge", WS_OVERLAPPEDWINDOW,
                             0, 0, 100, 100, nullptr, nullptr, cls.hInstance, nullptr);
    if (hwnd) {
        show(hwnd, SW_HIDE);
        if (!IsWindowVisible(hwnd) && PostMessageA(hwnd, kMessage, kPayload, 0)) {
            MSG msg{};
            while (!g_received_message && peek(&msg, hwnd, 0, 0, PM_REMOVE)) {
                translate(&msg);
                dispatch(&msg);
            }
        }
    }
    const bool destroyed = hwnd && DestroyWindow(hwnd);
    const bool unregistered = UnregisterClassA(cls.lpszClassName, cls.hInstance);
    return g_received_message && destroyed && unregistered;
}

using RegisterNotification = ULONG (WINAPI*)(decltype(&notification_callback), PVOID, PVOID*);
using UnregisterNotification = VOID (WINAPI*)(PVOID);

struct PsmLogCounts {
    size_t state_registered = 0;
    size_t constrained_registered = 0;
    size_t state_unregistered = 0;
    size_t state_foreign = 0;
    size_t state_unknown = 0;
    size_t constrained_unregistered = 0;
    size_t constrained_foreign = 0;
    size_t constrained_unknown = 0;
    size_t state_callback_unfired = 0;
    size_t constrained_callback_unfired = 0;
};

// Observed `context=0x...` values from the JSONL registration records. The
// shim logs the context pointer it actually stored for each token, so these
// are evidence of the round-trip, not of the arguments the test passed.
struct PsmContextObservations {
    std::unordered_set<uintptr_t> state_contexts;
    std::unordered_set<uintptr_t> constrained_contexts;
    bool saw_context_field = false;
    bool parse_error = false;
    // The current shim's unregister records only carry a detail string, so no
    // context is observable there. Record it instead of asserting the invisible.
    bool unregister_carries_context = false;
};

// Extract the `context=0x<hex>` value from a single JSONL line. `%llx` prints
// lowercase hex with a `0x` prefix and no leading zeros, so strtoull base 16
// (which accepts the optional `0x`) parses it exactly. Returns false when the
// field is absent or malformed; `value` is only written on success.
bool parse_context_field(const std::string& line, uintptr_t& value) {
    const size_t key = line.find("context=0x");
    if (key == std::string::npos) {
        return false;
    }
    // Parse from the value, after "context=". Starting at the key would make
    // strtoull read the hex digit 'c' from "context" and stop at 'o' (#520).
    const char* start = line.c_str() + key + std::strlen("context=");
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(start, &end, 16);
    if (end == start) {
        return false;
    }
    value = static_cast<uintptr_t>(parsed);
    return true;
}

int64_t query_file_size(const char* path) {
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        return 0;
    }
    LARGE_INTEGER size{};
    const bool ok = GetFileSizeEx(f, &size) != 0;
    CloseHandle(f);
    return ok && size.QuadPart > 0 ? size.QuadPart : 0;
}

bool count_psm_log_events(const char* path, int64_t start_offset, PsmLogCounts& out,
                          PsmContextObservations& contexts) {
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        std::fprintf(stderr, "FAIL: could not open PSM log %s (error %lu)\n", path, GetLastError());
        return false;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(f, &size) || size.QuadPart < start_offset) {
        CloseHandle(f);
        std::fprintf(stderr, "FAIL: invalid PSM log size for %s\n", path);
        return false;
    }
    const int64_t delta = size.QuadPart - start_offset;
    if (delta <= 0 || delta > (64LL << 20)) {
        CloseHandle(f);
        std::fprintf(stderr, "FAIL: unexpected PSM log delta (%lld bytes)\n",
                     static_cast<long long>(delta));
        return false;
    }
    LARGE_INTEGER seek{};
    seek.QuadPart = start_offset;
    if (!SetFilePointerEx(f, seek, nullptr, FILE_BEGIN)) {
        CloseHandle(f);
        return false;
    }
    std::string data(static_cast<size_t>(delta), '\0');
    size_t total_read = 0;
    while (total_read < data.size()) {
        DWORD chunk = 0;
        const DWORD want = static_cast<DWORD>(data.size() - total_read);
        if (!ReadFile(f, data.data() + total_read, want, &chunk, nullptr) || chunk == 0) {
            break;
        }
        total_read += chunk;
    }
    CloseHandle(f);
    data.resize(total_read);

    size_t pos = 0;
    while (pos < data.size()) {
        size_t nl = data.find('\n', pos);
        if (nl == std::string::npos) {
            nl = data.size();
        }
        const std::string line = data.substr(pos, nl - pos);
        pos = nl + 1;

        if (line.find("\"sub\":\"win32_bridge\"") == std::string::npos) {
            continue;
        }
        if (line.find("\"event\":\"RegisterAppStateChangeNotification\"") != std::string::npos &&
            line.find("callback_unfired") != std::string::npos) {
            ++out.state_callback_unfired;
            ++out.state_registered;
            uintptr_t ctx = 0;
            if (parse_context_field(line, ctx)) {
                contexts.saw_context_field = true;
                contexts.state_contexts.insert(ctx);
            } else {
                contexts.parse_error = true;
            }
        } else if (line.find("\"event\":\"RegisterAppConstrainedChangeNotification\"") != std::string::npos &&
                   line.find("callback_unfired") != std::string::npos) {
            ++out.constrained_callback_unfired;
            ++out.constrained_registered;
            uintptr_t ctx = 0;
            if (parse_context_field(line, ctx)) {
                contexts.saw_context_field = true;
                contexts.constrained_contexts.insert(ctx);
            } else {
                contexts.parse_error = true;
            }
        } else if (line.find("\"event\":\"RegisterAppStateChangeNotification\"") != std::string::npos &&
            line.find("\"detail\":\"registered_offline\"") != std::string::npos) {
            ++out.state_registered;
        } else if (line.find("\"event\":\"RegisterAppConstrainedChangeNotification\"") != std::string::npos &&
                   line.find("\"detail\":\"registered_offline\"") != std::string::npos) {
            ++out.constrained_registered;
        } else if (line.find("\"event\":\"UnregisterAppStateChangeNotification\"") != std::string::npos) {
            if (line.find("context=0x") != std::string::npos) {
                contexts.unregister_carries_context = true;
            }
            if (line.find("\"detail\":\"unregistered\"") != std::string::npos) {
                ++out.state_unregistered;
            } else if (line.find("\"detail\":\"foreign_token\"") != std::string::npos) {
                ++out.state_foreign;
            } else if (line.find("\"detail\":\"unknown_token\"") != std::string::npos) {
                ++out.state_unknown;
            }
        } else if (line.find("\"event\":\"UnregisterAppConstrainedChangeNotification\"") != std::string::npos) {
            if (line.find("context=0x") != std::string::npos) {
                contexts.unregister_carries_context = true;
            }
            if (line.find("\"detail\":\"unregistered\"") != std::string::npos) {
                ++out.constrained_unregistered;
            } else if (line.find("\"detail\":\"foreign_token\"") != std::string::npos) {
                ++out.constrained_foreign;
            } else if (line.find("\"detail\":\"unknown_token\"") != std::string::npos) {
                ++out.constrained_unknown;
            }
        }
    }
    return true;
}

bool test_psm() {
    char log_path[MAX_PATH]{};
    bool created_temp_log = false;
    const DWORD existing_len = GetEnvironmentVariableA("BX_LOG_FILE", log_path, sizeof(log_path));
    if (existing_len == 0 || existing_len >= sizeof(log_path)) {
        char temp_dir[MAX_PATH]{};
        const DWORD dir_len = GetTempPathA(sizeof(temp_dir), temp_dir);
        if (dir_len == 0 || dir_len >= sizeof(temp_dir) ||
            GetTempFileNameA(temp_dir, "bxp", 0, log_path) == 0) {
            std::fprintf(stderr, "FAIL: could not create temporary PSM log file\n");
            return false;
        }
        if (!SetEnvironmentVariableA("BX_LOG_FILE", log_path)) {
            DeleteFileA(log_path);
            return false;
        }
        created_temp_log = true;
    }
    const int64_t log_start_offset = query_file_size(log_path);

    bool ok = false;
    {
        const ScopedModule state_dll(load_bridge(L"api-ms-win-core-psm-appnotify-l1-1-0.dll"));
        const ScopedModule constrained_dll(load_bridge(L"api-ms-win-core-psm-appnotify-l1-1-1.dll"));
        if (state_dll && constrained_dll) {
            const auto register_state = lookup<RegisterNotification>(
                state_dll.get(), "RegisterAppStateChangeNotification");
            const auto unregister_state = lookup<UnregisterNotification>(
                state_dll.get(), "UnregisterAppStateChangeNotification");
            const auto register_constrained = lookup<RegisterNotification>(
                constrained_dll.get(), "RegisterAppConstrainedChangeNotification");
            const auto unregister_constrained = lookup<UnregisterNotification>(
                constrained_dll.get(), "UnregisterAppConstrainedChangeNotification");
            if (register_state && unregister_state && register_constrained && unregister_constrained) {
                PVOID token = reinterpret_cast<PVOID>(1);
                PVOID token2 = reinterpret_cast<PVOID>(1);
                const bool args_ok =
                    register_state(nullptr, nullptr, &token) == ERROR_INVALID_PARAMETER && !token &&
                    register_state(notification_callback, nullptr, nullptr) == ERROR_INVALID_PARAMETER &&
                    register_constrained(nullptr, nullptr, &token2) == ERROR_INVALID_PARAMETER && !token2 &&
                    register_constrained(notification_callback, nullptr, nullptr) == ERROR_INVALID_PARAMETER;

                constexpr size_t kThreads = 4;
                constexpr size_t kRegistrationsPerThread = 16;
                constexpr size_t kPerFamily = kThreads * kRegistrationsPerThread;
                struct Tokens {
                    PVOID state{};
                    PVOID constrained{};
                };
                std::array<std::array<Tokens, kRegistrationsPerThread>, kThreads> tokens{};
                std::array<std::thread, kThreads> workers;
                std::atomic<bool> success{args_ok};
                for (size_t i = 0; i < workers.size(); ++i) {
                    workers[i] = std::thread([&, i] {
                        for (auto& entry : tokens[i]) {
                            const auto state_status =
                                register_state(notification_callback, &entry, &entry.state);
                            const auto constrained_status =
                                register_constrained(notification_callback, &entry, &entry.constrained);
                            if (state_status != ERROR_SUCCESS || constrained_status != ERROR_SUCCESS ||
                                !entry.state || !entry.constrained) {
                                success.store(false);
                            }
                            // Cross-family unregister calls must log foreign_token and leave live tokens intact.
                            unregister_constrained(entry.state);
                            unregister_state(entry.constrained);
                            // Valid unregister followed by duplicate unregister on each family.
                            unregister_state(entry.state);
                            unregister_state(entry.state);
                            unregister_constrained(entry.constrained);
                            unregister_constrained(entry.constrained);
                        }
                    });
                }
                for (auto& worker : workers) {
                    worker.join();
                }

                // Null and arbitrary invalid tokens must classify as unknown_token, not foreign_token.
                unregister_state(nullptr);
                unregister_constrained(reinterpret_cast<PVOID>(0x1234));

                std::unordered_set<PVOID> seen;
                bool unique_ok = true;
                for (const auto& entries : tokens) {
                    for (const auto& entry : entries) {
                        if (!seen.insert(entry.state).second ||
                            !seen.insert(entry.constrained).second) {
                            unique_ok = false;
                        }
                    }
                }

                PsmLogCounts counts{};
                PsmContextObservations contexts{};
                const bool log_read_ok =
                    count_psm_log_events(log_path, log_start_offset, counts, contexts);
                const bool counts_ok = log_read_ok &&
                    counts.state_registered == kPerFamily &&
                    counts.constrained_registered == kPerFamily &&
                    counts.state_unregistered == kPerFamily &&
                    counts.state_foreign == kPerFamily &&
                    counts.state_unknown == kPerFamily + 1 &&
                    counts.constrained_unregistered == kPerFamily &&
                    counts.constrained_foreign == kPerFamily &&
                    counts.constrained_unknown == kPerFamily + 1;
                if (log_read_ok && !counts_ok) {
                    std::fprintf(
                        stderr,
                        "FAIL: PSM log count mismatch: state(reg=%zu unreg=%zu foreign=%zu unknown=%zu) "
                        "constrained(reg=%zu unreg=%zu foreign=%zu unknown=%zu)\n",
                        counts.state_registered, counts.state_unregistered,
                        counts.state_foreign, counts.state_unknown,
                        counts.constrained_registered, counts.constrained_unregistered,
                        counts.constrained_foreign, counts.constrained_unknown);
                }

                /* The callbacks are accepted and stored but never invoked, so the
                 * JSONL detail must say so: ERROR_SUCCESS is bookkeeping only. */
                const bool fired_never = !g_callback_called.load();
                if (!fired_never) {
                    std::fprintf(stderr, "FAIL: a PSM callback fired; the shim has no event source\n");
                }
                if (counts.state_callback_unfired != counts.state_registered) {
                    std::fprintf(stderr,
                                 "FAIL: %zu of %zu state registrations omit 'callback_unfired'\n",
                                 counts.state_registered - counts.state_callback_unfired,
                                 counts.state_registered);
                }
                if (counts.constrained_callback_unfired != counts.constrained_registered) {
                    std::fprintf(stderr,
                                 "FAIL: %zu of %zu constrained registrations omit 'callback_unfired'\n",
                                 counts.constrained_registered - counts.constrained_callback_unfired,
                                 counts.constrained_registered);
                }
                const bool limitation_logged = counts.state_callback_unfired == counts.state_registered &&
                                               counts.constrained_callback_unfired ==
                                                   counts.constrained_registered;

                /* Context round-trip: the JSONL registration record must print
                 * the exact context pointer the test passed (&entry), non-zero,
                 * on BOTH families. A dropped (context=0x0), swapped, or
                 * mis-tagged context would fail here. */
                std::unordered_set<uintptr_t> expected_contexts;
                for (const auto& entries : tokens) {
                    for (const auto& entry : entries) {
                        expected_contexts.insert(reinterpret_cast<uintptr_t>(&entry));
                    }
                }
                bool context_roundtrip_ok = contexts.saw_context_field && !contexts.parse_error;
                if (!contexts.saw_context_field) {
                    std::fprintf(stderr, "FAIL: no context=0x field observed in PSM registration records\n");
                }
                if (contexts.parse_error) {
                    std::fprintf(stderr, "FAIL: a PSM registration record had a missing/malformed context=\n");
                }
                // (1) Every context logged on the state family must be a real
                //     &entry the test passed, and non-zero.
                for (const uintptr_t ctx : contexts.state_contexts) {
                    if (ctx == 0) {
                        std::fprintf(stderr, "FAIL: state registration logged context=0x0 (dropped context)\n");
                        context_roundtrip_ok = false;
                    } else if (expected_contexts.find(ctx) == expected_contexts.end()) {
                        std::fprintf(stderr,
                                     "FAIL: state registration logged context=0x%llx that matches no &entry\n",
                                     static_cast<unsigned long long>(ctx));
                        context_roundtrip_ok = false;
                    }
                }
                // (1b) Same for the constrained family.
                for (const uintptr_t ctx : contexts.constrained_contexts) {
                    if (ctx == 0) {
                        std::fprintf(stderr, "FAIL: constrained registration logged context=0x0 (dropped context)\n");
                        context_roundtrip_ok = false;
                    } else if (expected_contexts.find(ctx) == expected_contexts.end()) {
                        std::fprintf(stderr,
                                     "FAIL: constrained registration logged context=0x%llx that matches no &entry\n",
                                     static_cast<unsigned long long>(ctx));
                        context_roundtrip_ok = false;
                    }
                }
                // (2) At least two distinct context pointers must appear per
                //     family, so one registration cannot overwrite another's.
                if (contexts.state_contexts.size() < 2) {
                    std::fprintf(stderr,
                                 "FAIL: only %zu distinct state context pointer(s) logged; expected >=2\n",
                                 contexts.state_contexts.size());
                    context_roundtrip_ok = false;
                }
                if (contexts.constrained_contexts.size() < 2) {
                    std::fprintf(stderr,
                                 "FAIL: only %zu distinct constrained context pointer(s) logged; expected >=2\n",
                                 contexts.constrained_contexts.size());
                    context_roundtrip_ok = false;
                }
                // (3) Family tagging: the test passes the SAME &entry to both
                //     families, so the constrained record for an entry must
                //     carry that entry's pointer too. With 64 registrations per
                //     family the full set of &entry pointers must round-trip on
                //     each family independently.
                if (contexts.state_contexts != expected_contexts) {
                    std::fprintf(stderr,
                                 "FAIL: state contexts (%zu) do not cover all %zu &entry pointers\n",
                                 contexts.state_contexts.size(), expected_contexts.size());
                    context_roundtrip_ok = false;
                }
                if (contexts.constrained_contexts != expected_contexts) {
                    std::fprintf(stderr,
                                 "FAIL: constrained contexts (%zu) do not cover all %zu &entry pointers\n",
                                 contexts.constrained_contexts.size(), expected_contexts.size());
                    context_roundtrip_ok = false;
                }
                // (4) Unregister context: the shim's unregister records carry
                //     only a detail string, so context-drop on unregister is
                //     not observable in the log. Assert nothing invisible; just
                //     document the limitation.
                if (contexts.unregister_carries_context) {
                    std::fprintf(stderr,
                                 "NOTE: unregister records unexpectedly carry context=; "
                                 "context-drop on unregister is now observable\n");
                }

                ok = success.load() && unique_ok && fired_never && counts_ok && limitation_logged &&
                     context_roundtrip_ok;
            }
        }
    }

    if (created_temp_log) {
        DeleteFileA(log_path);
    }
    return ok;
}

} // namespace

int main() {
    if (!test_window()) {
        std::fprintf(stderr, "FAIL: Win32 bridge window or message pump (error %lu)\n", GetLastError());
        return 1;
    }
    if (!test_psm()) {
        std::fprintf(stderr, "FAIL: PSM bridge registration lifecycle\n");
        return 2;
    }
    std::puts("PASS: test_win32_bridge (hidden window, message pump, 128 PSM registrations, "
              "per-event log + context round-trip verified)");
    return 0;
}
