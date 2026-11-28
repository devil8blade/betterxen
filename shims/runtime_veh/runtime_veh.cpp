#include <windows.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <intrin.h>

#include "bx_config.h"
#include "bx_log.h"

namespace {

CRITICAL_SECTION g_log_lock;
bool g_log_lock_ready = false;
HANDLE g_log_handle = INVALID_HANDLE_VALUE;
PVOID g_veh_handle = nullptr;
LPTOP_LEVEL_EXCEPTION_FILTER g_previous_filter = nullptr;
std::atomic_flag g_crash_logged{};
std::atomic<bool> g_crash_in_flight{false};
std::atomic<uint64_t> g_clwb_traps{0};
std::atomic<uint32_t> g_breakpoint_records{0};

// Breakpoint and assertion-failure records per process, so a guest that uses int3 on purpose
// cannot flood the log.
constexpr uint32_t kMaxBreakpointRecords = 16;
constexpr DWORD kStatusAssertionFailure = 0xC0000420;

// Other AMD-only instructions Rosetta 2 (and Intel CPUs) raise #UD on. Their
// counts reach the JSONL trace only: the frozen .def has no export for them.
enum AmdKind { kClzero, kMonitorx, kMwaitx, kRdpru, kExtrq, kInsertq, kAmdKinds };
constexpr const char* kAmdEvents[kAmdKinds] = {
    "clzero_trapped", "monitorx_trapped", "mwaitx_trapped",
    "rdpru_trapped", "extrq_trapped", "insertq_trapped"};
std::atomic<uint64_t> g_amd_traps[kAmdKinds]{};
bx_runtime_config_t g_config = {
    BX_ABI_VERSION,
    BX_PROFILE_LOCKHART,
    BX_LOG_INFO,
    8ULL * 1024ULL * 1024ULL * 1024ULL, /* Default 8 GiB Lockhart title budget */
    0u,
    1u,
    1u,
    0u
};

void write_log_line(const char* line, size_t len) {
    // Pause regular logging while a terminal crash record is being emitted so
    // background threads cannot interleave with the unlocked crash WriteFile.
    if (g_crash_in_flight.load(std::memory_order_acquire)) {
        return;
    }
    if (g_log_lock_ready) {
        EnterCriticalSection(&g_log_lock);
    }
    if (g_log_handle != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(g_log_handle, line, static_cast<DWORD>(len), &written, nullptr);
    } else {
        DWORD written = 0;
        HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
        if (err && err != INVALID_HANDLE_VALUE) {
            WriteFile(err, line, static_cast<DWORD>(len), &written, nullptr);
        }
    }
    OutputDebugStringA(line);
    if (g_log_lock_ready) {
        LeaveCriticalSection(&g_log_lock);
    }
}

constexpr size_t kMaxInstructionLength = 15;

size_t read_instruction(uint64_t rip, uint8_t (&bytes)[kMaxInstructionLength]) {
    HANDLE process = GetCurrentProcess();
    if (ReadProcessMemory(process, reinterpret_cast<LPCVOID>(rip),
                          bytes, sizeof(bytes), nullptr)) {
        return sizeof(bytes);
    }

    // A short instruction can end immediately before an inaccessible page.
    size_t size = 0;
    while (size < sizeof(bytes) &&
           ReadProcessMemory(process, reinterpret_cast<LPCVOID>(rip + size),
                             &bytes[size], 1, nullptr)) {
        ++size;
    }
    return size;
}

// CLWB is 66 0F AE /6 with a memory operand. The address-size override
// selects 32-bit addressing but leaves the ModRM/SIB displacement lengths intact.
size_t decode_clwb_length(const uint8_t* ip, size_t size) {
    size_t idx = 0;
    bool has_66 = false;
    while (idx < size) {
        const uint8_t byte = ip[idx];
        if (byte == 0x66) {
            has_66 = true;
        } else if (byte != 0x67 && byte != 0x26 && byte != 0x2E &&
                   byte != 0x36 && byte != 0x3E && byte != 0x64 && byte != 0x65) {
            break;
        }
        ++idx;
    }

    // x86-64 allows at most one REX prefix immediately before the opcode.
    if (idx < size && (ip[idx] & 0xF0) == 0x40) {
        ++idx;
    }
    if (!has_66 || size - idx < 3 || ip[idx] != 0x0F || ip[idx + 1] != 0xAE) {
        return 0;
    }
    idx += 2;

    const uint8_t modrm = ip[idx++];
    const uint8_t mod = modrm >> 6;
    const uint8_t reg = (modrm >> 3) & 7;
    const uint8_t rm = modrm & 7;
    if (mod == 3 || reg != 6) {
        return 0;
    }

    uint8_t base = rm;
    if (rm == 4) {
        if (idx == size) {
            return 0;
        }
        base = ip[idx++] & 7;
    }
    if (mod == 1) {
        idx += 1;
    } else if (mod == 2 || (mod == 0 && base == 5)) {
        idx += 4;
    }
    return idx <= size ? idx : 0;
}

bool is_adx_opcode(const uint8_t* ip, size_t size) {
    size_t idx = 0;
    while (idx < size && (ip[idx] == 0x66 || ip[idx] == 0xF3)) {
        ++idx;
    }
    if (idx == 0) {
        return false;
    }
    if (idx < size && (ip[idx] & 0xF0) == 0x40) {
        ++idx;
    }
    // Require the 3-byte ADCX/ADOX opcode (0F 38 F6) plus at least 1 ModRM byte.
    return size - idx >= 4 && ip[idx] == 0x0F && ip[idx + 1] == 0x38 && ip[idx + 2] == 0xF6;
}

// Logs the first trap of a kind and then every power of two, as for clwb.
void log_sampled_trap(const char* event, uint64_t count, uint64_t rip, size_t len) {
    if (count == 1 || (count & (count - 1)) == 0) {
        char msg[160];
        std::snprintf(msg, sizeof(msg),
                      "rip=0x%016llx len=%zu total_traps=%llu",
                      static_cast<unsigned long long>(rip), len,
                      static_cast<unsigned long long>(count));
        bx_log_emit(BX_LOG_INFO, "runtime_veh", event, msg);
    }
}

// The instruction encodings and architectural effects below are derived from
// AMD APM vol. 3 (24594, rev. 3.36), Instruction Prefixes and CLZERO, MONITORX,
// MWAITX, RDPRU, and vol. 4 (26568, rev. 3.25), EXTRQ and INSERTQ.
struct AmdInstruction {
    AmdKind kind = kAmdKinds;
    size_t length = 0;
    bool address32 = false;
    size_t destination = 0;
    size_t source = 0;
    bool immediate = false;
    uint16_t field = 0;
};

AmdInstruction decode_amd_instruction(const uint8_t* bytes, size_t size) {
    if (size < 3) {
        return {};
    }

    const bool address32 = bytes[0] == 0x67;
    const bool extract = bytes[0] == 0x66;
    const bool insert = bytes[0] == 0xF2;
    size_t offset = (address32 || extract || insert) ? 1 : 0;
    uint8_t rex = 0;
    if ((bytes[offset] & 0xF0) == 0x40) {
        rex = bytes[offset++];
    }
    if (size - offset < 3 || bytes[offset] != 0x0F) {
        return {};
    }

    const uint8_t opcode = bytes[offset + 1];
    const uint8_t operand = bytes[offset + 2];
    const size_t length = offset + 3;
    if (!extract && !insert && opcode == 0x01) {
        switch (operand) {
        case 0xFC: return {kClzero, length, address32};
        case 0xFA: return {kMonitorx, length, address32};
        case 0xFB: return address32 ? AmdInstruction{} : AmdInstruction{kMwaitx, length};
        case 0xFD: return address32 ? AmdInstruction{} : AmdInstruction{kRdpru, length};
        default: return {};
        }
    }

    if ((!extract && !insert) || (opcode != 0x78 && opcode != 0x79) ||
        (operand & 0xC0) != 0xC0) {
        return {};
    }
    const bool immediate = opcode == 0x78;
    const unsigned reg = (operand & 0x38) >> 3;
    if (immediate && (size - length < 2 || (extract && reg != 0))) {
        return {};
    }

    const size_t rm = (operand & 7) + ((rex & 1) ? 8 : 0);
    AmdInstruction result{extract ? kExtrq : kInsertq, length};
    result.destination = extract && immediate ? rm : reg + ((rex & 4) ? 8 : 0);
    result.source = rm;
    result.immediate = immediate;
    if (immediate) {
        result.length += 2;
        result.field = static_cast<uint16_t>(bytes[length + 1] * 256u + bytes[length]);
    }
    return result;
}

void apply_sse4a(const AmdInstruction& instruction, CONTEXT& context) {
    M128A* registers = context.FltSave.XmmRegisters;
    M128A& destination = registers[instruction.destination];
    const M128A source = registers[instruction.source];
    const bool extract = instruction.kind == kExtrq;

    // EXTRQ takes register controls from bits 13:0 and INSERTQ from 77:64.
    uint64_t field = instruction.field;
    if (!instruction.immediate) {
        field = extract ? source.Low : static_cast<uint64_t>(source.High);
    }
    const unsigned index = (field & 0x3F00) >> 8;
    const unsigned width = field & 0x3F;
    const uint64_t low_mask = width ? UINT64_MAX >> (64 - width) : UINT64_MAX;
    if (extract) {
        destination.Low = (destination.Low >> index) & low_mask;
    } else {
        const uint64_t positioned_mask = low_mask << index;
        destination.Low = (destination.Low & ~positioned_mask) |
                          ((source.Low << index) & positioned_mask);
    }
    // The APM leaves the upper quadword and out-of-range fields undefined.
}

bool zero_guest_cache_line(uint64_t address, EXCEPTION_RECORD& exception) {
    // The supported guest has 64-byte cache lines. An aligned line fits
    // in one Windows page, including when the address is at the end of it.
    constexpr uintptr_t line_size = 64;
    const uintptr_t start = address & ~(line_size - 1);
    constexpr DWORD write_protections = PAGE_READWRITE | PAGE_WRITECOPY |
                                       PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    MEMORY_BASIC_INFORMATION page{};
    if (VirtualQuery(reinterpret_cast<const void*>(start), &page, sizeof(page)) &&
        page.State == MEM_COMMIT && (page.Protect & write_protections) &&
        !(page.Protect & PAGE_GUARD)) {
        std::memset(reinterpret_cast<void*>(start), 0, line_size);
        return true;
    }

    // Pass the write fault on at the original guest RIP. Guard pages use
    // the same fallback, without Windows' one-shot guard-page semantics.
    exception.ExceptionCode = EXCEPTION_ACCESS_VIOLATION;
    exception.NumberParameters = 2;
    exception.ExceptionInformation[0] = 1; // Write access.
    exception.ExceptionInformation[1] = start;
    return false;
}

bool apply_amd_instruction(const AmdInstruction& instruction, CONTEXT& context,
                           EXCEPTION_RECORD& exception) {
    switch (instruction.kind) {
    case kClzero:
        return zero_guest_cache_line(instruction.address32 ? static_cast<uint32_t>(context.Rax)
                                                           : context.Rax, exception);
    case kMonitorx:
        // There is no host monitor state. MWAITX may complete immediately.
        return true;
    case kMwaitx:
        // Approximate an early wakeup with one scheduler yield.
        SwitchToThread();
        return true;
    case kRdpru: {
        const uint32_t selector = static_cast<uint32_t>(context.Rcx);
        const uint64_t counter = selector < 2 ? __rdtsc() : 0;
        context.Rax = static_cast<uint32_t>(counter);
        context.Rdx = static_cast<uint32_t>(counter >> 32);

        // The APM specifies zero for these five flags and marks CF modified.
        // BetterXen chooses a clear CF until its native behavior is verified.
        constexpr DWORD arithmetic_flags = (1u << 11) | (1u << 7) | (1u << 6) |
                                           (1u << 4) | (1u << 2) | (1u << 0);
        context.EFlags &= ~arithmetic_flags;
        return true;
    }
    case kExtrq:
    case kInsertq:
        apply_sse4a(instruction, context);
        return true;
    case kAmdKinds:
        return false;
    }
    return false;
}

LONG WINAPI log_unhandled_exception(PEXCEPTION_POINTERS ep) {
    // Wine can invoke the top-level filter again for the same terminal fault.
    if (ep && ep->ExceptionRecord && ep->ContextRecord &&
        !g_crash_logged.test_and_set(std::memory_order_relaxed)) {
        g_crash_in_flight.store(true, std::memory_order_release);
        const CONTEXT& ctx = *ep->ContextRecord;
        MEMORY_BASIC_INFORMATION region{};
        unsigned long long module_base = 0;
        if (VirtualQuery(reinterpret_cast<LPCVOID>(ctx.Rip), &region, sizeof(region)) &&
            region.Type == MEM_IMAGE) {
            module_base = reinterpret_cast<uintptr_t>(region.AllocationBase);
        }

        char line[1024];
        int length = std::snprintf(
            line, sizeof(line),
            "{\"ts_ms\":%llu,\"tid\":%lu,\"level\":\"ERROR\",\"sub\":\"runtime_veh\","
            "\"event\":\"unhandled_exception\",\"exception_code\":\"0x%08lx\","
            "\"module_base\":\"0x%016llx\",\"rva\":\"0x%016llx\","
            "\"rip\":\"0x%016llx\",\"rsp\":\"0x%016llx\","
            "\"registers\":{\"rax\":\"0x%016llx\",\"rbx\":\"0x%016llx\","
            "\"rcx\":\"0x%016llx\",\"rdx\":\"0x%016llx\",\"rsi\":\"0x%016llx\","
            "\"rdi\":\"0x%016llx\",\"rbp\":\"0x%016llx\",\"r8\":\"0x%016llx\","
            "\"r9\":\"0x%016llx\",\"r10\":\"0x%016llx\",\"r11\":\"0x%016llx\","
            "\"r12\":\"0x%016llx\",\"r13\":\"0x%016llx\",\"r14\":\"0x%016llx\","
            "\"r15\":\"0x%016llx\"}}\n",
            static_cast<unsigned long long>(GetTickCount64()), GetCurrentThreadId(),
            ep->ExceptionRecord->ExceptionCode, module_base,
            module_base ? ctx.Rip - module_base : 0ULL, ctx.Rip, ctx.Rsp,
            ctx.Rax, ctx.Rbx, ctx.Rcx, ctx.Rdx, ctx.Rsi, ctx.Rdi, ctx.Rbp,
            ctx.R8, ctx.R9, ctx.R10, ctx.R11, ctx.R12, ctx.R13, ctx.R14, ctx.R15);
        // A crashed thread may already own the regular logger's lock.
        HANDLE output = g_log_handle != INVALID_HANDLE_VALUE
            ? g_log_handle : GetStdHandle(STD_ERROR_HANDLE);
        if (length > 0 && length < static_cast<int>(sizeof(line)) &&
            output && output != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(output, line, static_cast<DWORD>(length), &written, nullptr);
        }
    }

    const LONG disposition = g_previous_filter ? g_previous_filter(ep) : EXCEPTION_CONTINUE_SEARCH;
    if (disposition == EXCEPTION_CONTINUE_EXECUTION) {
        g_crash_in_flight.store(false, std::memory_order_release);
        g_crash_logged.clear(std::memory_order_relaxed);
    }
    return disposition;
}

// A guest with its own crash reporter replaces the top-level filter installed at attach, so
// log_unhandled_exception never sees its assertions. The vectored handler runs before the
// guest's top-level filter and before any vectored handler the guest registers last; a handler
// the guest registers first can resume the exception before this one sees it. It records
// breakpoints and assertion failures with their module offset. They are first-chance, so the
// guest may still handle them; handling is left unchanged.
void log_breakpoint(const EXCEPTION_RECORD& record) {
    const uint32_t index = g_breakpoint_records.fetch_add(1, std::memory_order_relaxed) + 1;
    if (index > kMaxBreakpointRecords) {
        return;
    }
    const uintptr_t address = reinterpret_cast<uintptr_t>(record.ExceptionAddress);
    MEMORY_BASIC_INFORMATION region{};
    uintptr_t module_base = 0;
    char module[MAX_PATH] = "?";
    if (VirtualQuery(record.ExceptionAddress, &region, sizeof(region)) &&
        region.Type == MEM_IMAGE) {
        module_base = reinterpret_cast<uintptr_t>(region.AllocationBase);
        // Wide API plus an explicit UTF-8 conversion: the ANSI form returns the name in the
        // active code page, which would make a non-ASCII module name invalid UTF-8 in the JSON.
        wchar_t path[MAX_PATH];
        const DWORD length =
            GetModuleFileNameW(reinterpret_cast<HMODULE>(module_base), path, MAX_PATH);
        if (length > 0 && length < MAX_PATH) {
            const wchar_t* slash = std::wcsrchr(path, L'\\');
            const wchar_t* name = slash ? slash + 1 : path;
            if (WideCharToMultiByte(CP_UTF8, 0, name, -1, module, sizeof(module), nullptr, nullptr) == 0) {
                std::snprintf(module, sizeof(module), "?");
            }
        }
    }

    char line[1024];
    const int length = std::snprintf(
        line, sizeof(line),
        "{\"ts_ms\":%llu,\"tid\":%lu,\"level\":\"WARN\",\"sub\":\"runtime_veh\","
        "\"event\":\"breakpoint_exception\",\"exception_code\":\"0x%08lx\","
        "\"module\":\"%s\",\"module_base\":\"0x%016llx\",\"rva\":\"0x%016llx\","
        "\"address\":\"0x%016llx\",\"record\":%u,\"limit\":%u}\n",
        static_cast<unsigned long long>(GetTickCount64()), GetCurrentThreadId(),
        record.ExceptionCode, module, static_cast<unsigned long long>(module_base),
        static_cast<unsigned long long>(module_base ? address - module_base : 0),
        static_cast<unsigned long long>(address), index, kMaxBreakpointRecords);
    if (length > 0 && length < static_cast<int>(sizeof(line))) {
        write_log_line(line, static_cast<size_t>(length));
    }
}

LONG CALLBACK bx_veh_handler(PEXCEPTION_POINTERS ep) {
    if (!ep || !ep->ExceptionRecord || !ep->ContextRecord) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_BREAKPOINT || code == kStatusAssertionFailure) {
        log_breakpoint(*ep->ExceptionRecord);
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (code != EXCEPTION_ILLEGAL_INSTRUCTION) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    uint8_t ip[kMaxInstructionLength]{};
    const size_t size = read_instruction(ep->ContextRecord->Rip, ip);
    if (g_config.enable_clwb_veh) {
        size_t len = decode_clwb_length(ip, size);
        if (len > 0) {
            uint64_t count = g_clwb_traps.fetch_add(1, std::memory_order_relaxed) + 1;
            log_sampled_trap("clwb_trapped", count, ep->ContextRecord->Rip, len);
            ep->ContextRecord->Rip += len;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }

    const AmdInstruction instruction = decode_amd_instruction(ip, size);
    if (instruction.length != 0) {
        if (!apply_amd_instruction(instruction, *ep->ContextRecord, *ep->ExceptionRecord)) {
            return EXCEPTION_CONTINUE_SEARCH;
        }
        const uint64_t count =
            g_amd_traps[instruction.kind].fetch_add(1, std::memory_order_relaxed) + 1;
        log_sampled_trap(kAmdEvents[instruction.kind], count, ep->ContextRecord->Rip,
                         instruction.length);
        ep->ContextRecord->Rip += instruction.length;
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    if (g_config.enable_adx_tripwire && is_adx_opcode(ip, size)) {
        char msg[160];
        std::snprintf(msg, sizeof(msg),
                      "rip=0x%016llx bytes=%02x %02x %02x %02x %02x",
                      static_cast<unsigned long long>(ep->ContextRecord->Rip),
                      ip[0], ip[1], ip[2], ip[3], ip[4]);
        bx_log_emit(BX_LOG_ERROR, "runtime_veh", "adx_illegal_instruction", msg);
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

void init_config_and_logging() {
    InitializeCriticalSection(&g_log_lock);
    g_log_lock_ready = true;

    char buf[512];
    if (GetEnvironmentVariableA(BX_ENV_PROFILE, buf, sizeof(buf)) > 0) {
        if (_stricmp(buf, "anaconda") == 0) {
            g_config.profile = BX_PROFILE_ANACONDA;
            g_config.title_memory_limit_bytes = 13500ULL * 1024ULL * 1024ULL;
        }
    }
    if (GetEnvironmentVariableA(BX_ENV_MEM_LIMIT_MB, buf, sizeof(buf)) > 0) {
        unsigned long long mb = std::strtoull(buf, nullptr, 10);
        if (mb >= 512) {
            g_config.title_memory_limit_bytes = mb * 1024ULL * 1024ULL;
        }
    }
    if (GetEnvironmentVariableA(BX_ENV_LOG_LEVEL, buf, sizeof(buf)) > 0) {
        if (_stricmp(buf, "error") == 0) g_config.log_level = BX_LOG_ERROR;
        else if (_stricmp(buf, "warn") == 0) g_config.log_level = BX_LOG_WARN;
        else if (_stricmp(buf, "trace") == 0) g_config.log_level = BX_LOG_TRACE;
    }
    if (GetEnvironmentVariableA(BX_ENV_TRACE_D3D, buf, sizeof(buf)) > 0 && buf[0] == '1') {
        g_config.trace_d3d = 1u;
    }
    if (GetEnvironmentVariableA(BX_ENV_LOG_FILE, buf, sizeof(buf)) > 0) {
        g_log_handle = CreateFileA(buf, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
}

} // namespace

extern "C" {

void bx_log_emit(bx_log_level_t level, const char* subsystem, const char* event, const char* detail) {
    if (level > g_config.log_level) {
        return;
    }
    static const char* kLevels[] = {"ERROR", "WARN", "INFO", "TRACE"};
    const char* lvl = (level <= BX_LOG_TRACE) ? kLevels[level] : "INFO";
    char line[768];
    int n = std::snprintf(
        line, sizeof(line),
        "{\"ts_ms\":%llu,\"tid\":%lu,\"level\":\"%s\",\"sub\":\"%s\",\"event\":\"%s\",\"detail\":\"%s\"}\n",
        static_cast<unsigned long long>(GetTickCount64()),
        static_cast<unsigned long>(GetCurrentThreadId()),
        lvl,
        subsystem ? subsystem : "unknown",
        event ? event : "event",
        detail ? detail : "");
    if (n > 0) {
        write_log_line(line, static_cast<size_t>(n < static_cast<int>(sizeof(line)) ? n : sizeof(line) - 1));
    }
}

void bx_log_unimpl_api(const char* dll_name, const char* symbol_name) {
    char detail[256];
    std::snprintf(detail, sizeof(detail), "%s!%s",
                  dll_name ? dll_name : "?",
                  symbol_name ? symbol_name : "?");
    bx_log_emit(BX_LOG_WARN, "stub", "unimpl_api", detail);
}

void bx_log_unimpl_vtable(const char* iface_name, uint32_t slot_index, const char* method_name) {
    char detail[256];
    std::snprintf(detail, sizeof(detail), "%s[%u]::%s",
                  iface_name ? iface_name : "?",
                  slot_index,
                  method_name ? method_name : "?");
    bx_log_emit(BX_LOG_WARN, "com_vtable", "unimpl_vtable", detail);
}

const bx_runtime_config_t* bx_get_runtime_config(void) {
    return &g_config;
}

int bx_veh_install(void) {
    if (!g_veh_handle) {
        g_veh_handle = AddVectoredExceptionHandler(1, bx_veh_handler);
    }
    return g_veh_handle != nullptr ? 1 : 0;
}

uint64_t bx_veh_get_clwb_trap_count(void) {
    return g_clwb_traps.load(std::memory_order_relaxed);
}

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        init_config_and_logging();
        bx_veh_install();
        g_previous_filter = SetUnhandledExceptionFilter(log_unhandled_exception);
        bx_log_emit(BX_LOG_INFO, "runtime_veh", "attached",
                    g_config.profile == BX_PROFILE_ANACONDA ? "profile=anaconda" : "profile=lockhart");
    } else if (reason == DLL_PROCESS_DETACH) {
        // Restore g_previous_filter only when our filter is still the active
        // top-level filter; if the guest installed its own filter after attach,
        // put the guest filter back unchanged. (The frozen .def has no separate
        // uninstall export, so detach performs this bounded pointer swap.)
        const auto current_filter = SetUnhandledExceptionFilter(g_previous_filter);
        if (current_filter != log_unhandled_exception) {
            SetUnhandledExceptionFilter(current_filter);
        }
        if (g_veh_handle) {
            RemoveVectoredExceptionHandler(g_veh_handle);
            g_veh_handle = nullptr;
        }
        if (g_log_handle != INVALID_HANDLE_VALUE) {
            CloseHandle(g_log_handle);
            g_log_handle = INVALID_HANDLE_VALUE;
        }
        if (g_log_lock_ready) {
            DeleteCriticalSection(&g_log_lock);
            g_log_lock_ready = false;
        }
    }
    return TRUE;
}

} // extern "C"
