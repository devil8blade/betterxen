#include <windows.h>
#include <immintrin.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "bx_config.h"
#include "bx_log.h"
#include "bx_xmem_internal.h"

extern "C" {
uint64_t bx_veh_get_clwb_trap_count(void);
PVOID WINAPI XMemAlloc(SIZE_T size, uint64_t attributes);
VOID WINAPI XMemFree(PVOID address, uint64_t attributes);
BOOL WINAPI XMemTransferMemory(PVOID dst, LPCVOID src, SIZE_T bytes, uint64_t flags);
}

// ---------------------------------------------------------------------------
// BX_LOG_FILE reader. BX_LOG_FILE must name the log before the process
// starts (`bx-cli run --log`): this EXE imports bx_runtime_veh.dll, which reads
// the variable once in DLL_PROCESS_ATTACH, before main runs, so setting it here
// would be too late and the checks would read an empty file.
// ---------------------------------------------------------------------------

struct LogReader {
    char path[MAX_PATH]{};
    int64_t start_offset = 0;

    bool init() {
        const DWORD existing_len =
            GetEnvironmentVariableA("BX_LOG_FILE", path, sizeof(path));
        if (existing_len == 0 || existing_len >= sizeof(path)) {
            std::fprintf(stderr, "FAIL: BX_LOG_FILE must be set before launch (run under bx-cli run --log)\n");
            return false;
        }
        start_offset = query_file_size(path);
        return true;
    }

    static int64_t query_file_size(const char* p) {
        HANDLE f = CreateFileA(p, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) {
            return 0;
        }
        LARGE_INTEGER size{};
        const bool ok = GetFileSizeEx(f, &size) != 0;
        CloseHandle(f);
        return ok && size.QuadPart > 0 ? size.QuadPart : 0;
    }

    // Read only the bytes appended after start_offset.
    bool read_new(std::string& out) const {
        HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) {
            std::fprintf(stderr, "FAIL: could not open CLWB log %s (error %lu)\n",
                         path, GetLastError());
            return false;
        }
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(f, &size) || size.QuadPart < start_offset) {
            CloseHandle(f);
            std::fprintf(stderr, "FAIL: invalid CLWB log size for %s\n", path);
            return false;
        }
        const int64_t delta = size.QuadPart - start_offset;
        if (delta < 0 || delta > (64LL << 20)) {
            CloseHandle(f);
            std::fprintf(stderr, "FAIL: unexpected CLWB log delta (%lld bytes)\n",
                         static_cast<long long>(delta));
            return false;
        }
        LARGE_INTEGER seek{};
        seek.QuadPart = start_offset;
        if (!SetFilePointerEx(f, seek, nullptr, FILE_BEGIN)) {
            CloseHandle(f);
            return false;
        }
        out.assign(static_cast<size_t>(delta), '\0');
        size_t total_read = 0;
        while (total_read < out.size()) {
            DWORD chunk = 0;
            const DWORD want = static_cast<DWORD>(out.size() - total_read);
            if (!ReadFile(f, out.data() + total_read, want, &chunk, nullptr) || chunk == 0) {
                break;
            }
            total_read += chunk;
        }
        CloseHandle(f);
        out.resize(total_read);
        return true;
    }
};

// One parsed `clwb_trapped` record.
struct ClwbRecord {
    uint64_t total_traps = 0;
    size_t len = 0;
};

// Parse every `"event":"clwb_trapped"` line for `len=` and `total_traps=`.
static std::vector<ClwbRecord> parse_clwb_records(const std::string& data) {
    std::vector<ClwbRecord> records;
    size_t pos = 0;
    while (pos < data.size()) {
        size_t nl = data.find('\n', pos);
        if (nl == std::string::npos) {
            nl = data.size();
        }
        const std::string line = data.substr(pos, nl - pos);
        pos = nl + 1;

        if (line.find("\"event\":\"clwb_trapped\"") == std::string::npos) {
            continue;
        }
        ClwbRecord rec{};
        // len=%zu
        const size_t len_key = line.find("len=");
        if (len_key == std::string::npos) {
            continue;
        }
        // strtoull, not stoull: a malformed record must not throw past the FAIL message.
        char* len_end = nullptr;
        rec.len = std::strtoull(line.c_str() + len_key + 4, &len_end, 10);
        if (len_end == line.c_str() + len_key + 4) {
            continue;
        }
        // total_traps=%llu
        const size_t tt_key = line.find("total_traps=");
        if (tt_key == std::string::npos) {
            continue;
        }
        char* tt_end = nullptr;
        rec.total_traps = std::strtoull(line.c_str() + tt_key + 12, &tt_end, 10);
        if (tt_end == line.c_str() + tt_key + 12) {
            continue;
        }
        records.push_back(rec);
    }
    return records;
}

int main() {
    /* 1. Verify AVX2 + FMA execution under Rosetta 2 */
    __m256 a = _mm256_set1_ps(3.0f);
    __m256 b = _mm256_set1_ps(4.0f);
    __m256 c = _mm256_fmadd_ps(a, b, a); /* 3*4 + 3 = 15.0 */
    alignas(32) float out[8] = {};
    _mm256_store_ps(out, c);
    for (int i = 0; i < 8; ++i) {
        if (out[i] != 15.0f) {
            std::fprintf(stderr, "FAIL: AVX2/FMA mismatch lane %d: %f\n", i, out[i]);
            return 1;
        }
    }

    // Set up the log reader BEFORE the clwb block so we capture every
    // clwb_trapped record the VEH emits.  The AVX2/FMA block above runs
    // first but does not trap (bx_veh_get_clwb_trap_count counts only
    // clwb), so the first clwb below is trap #1.
    LogReader log;
    if (!log.init()) {
        return 5;
    }

    // Fixed registers ensure the assembler emits the intended ModRM/REX forms.
    alignas(64) uint8_t buf[512] = {0x42};
    const uint64_t before = bx_veh_get_clwb_trap_count();
    __asm__ volatile(
        "clwb (%%rax)\n\t"
        "clwb 64(%%rax)\n\t"
        "clwb 256(%%rax)\n\t"
        "mov %%rax, %%r8\n\t"
        "clwb (%%r8)\n\t"
        "clwb 64(%%rax,%%rcx,4)\n\t"
        "clwb (%%rip)"
        :: "a"(buf), "c"(8) : "r8", "memory");
    const uint64_t delta = bx_veh_get_clwb_trap_count() - before;
    if (delta != 6) {
        std::fprintf(stderr, "FAIL: expected 6 CLWB traps, observed %llu\n",
                     static_cast<unsigned long long>(delta));
        return 4;
    }
    std::printf("CLWB traps observed: %llu\n", static_cast<unsigned long long>(delta));

    // -----------------------------------------------------------------------
    // Verify the decoded instruction length (`len=`) that the VEH logs and
    // uses to advance RIP.  A wrong `len` lands the guest mid-instruction
    // while `delta` still reads 6, so this is the field that decides whether
    // the guest resumes correctly.
    //
    // log_sampled_trap() logs only when count == 1 or count is a power of
    // two, i.e. traps 1, 2, 4, 8, ...  Six traps therefore produce exactly
    // three records with total_traps = 1, 2, 4.  We assert on those three
    // sampled records only (option (a)); we do NOT see a `len=` for forms
    // 3, 5, 6 and make no claim about them here.
    //
    // Expected lengths, re-verified by assembling with
    // x86_64-w64-mingw32-as and disassembling with
    // x86_64-w64-mingw32-objdump -d:
    //
    //   0:  66 0f ae 30              clwb (%rax)               -> 4
    //   4:  66 0f ae 70 40           clwb 0x40(%rax)           -> 5
    //   9:  66 0f ae b0 00 01 00 00  clwb 0x100(%rax)          -> 8
    //   11: 66 41 0f ae 30           clwb (%r8)                -> 5  (REX.B, ModRM 0x30)
    //   16: 66 0f ae 74 88 40        clwb 0x40(%rax,%rcx,4)    -> 6  (SIB + disp8)
    //   1c: 66 0f ae 35 00 00 00 00  clwb 0x0(%rip)            -> 7  (disp32)
    //
    // Mapping sampled count -> ordinal -> expected len:
    //   total_traps=1 -> ordinal 1 -> clwb (%rax)    -> len 4
    //   total_traps=2 -> ordinal 2 -> clwb 0x40(%rax)-> len 5
    //   total_traps=4 -> ordinal 4 -> clwb (%r8)     -> len 5
    // -----------------------------------------------------------------------
    {
        std::string log_data;
        if (!log.read_new(log_data)) {
            return 6;
        }
        const std::vector<ClwbRecord> records = parse_clwb_records(log_data);
        if (records.empty()) {
            std::fprintf(stderr,
                         "FAIL: no clwb_trapped records found in log %s "
                         "(BX_LOG_FILE must be readable)\n", log.path);
            return 7;
        }

        // Every observed record must have len >= 4 (the shortest form,
        // 66 0F AE /6 with ModRM mod=00 rm=000, is 4 bytes).
        for (const auto& r : records) {
            if (r.len < 4) {
                std::fprintf(stderr,
                             "FAIL: clwb_trapped total_traps=%llu has len=%zu < 4\n",
                             static_cast<unsigned long long>(r.total_traps), r.len);
                return 8;
            }
        }

        // Exact expectations for the three sampled counts.
        struct Expected { uint64_t total_traps; size_t len; const char* form; };
        static const Expected kExpected[] = {
            {1, 4, "clwb (%rax)"},
            {2, 5, "clwb 0x40(%rax)"},
            {4, 5, "clwb (%r8)"},
        };
        for (const auto& exp : kExpected) {
            bool found = false;
            for (const auto& r : records) {
                if (r.total_traps == exp.total_traps) {
                    found = true;
                    if (r.len != exp.len) {
                        std::fprintf(stderr,
                                     "FAIL: clwb_trapped total_traps=%llu (%s): "
                                     "expected len=%zu, got len=%zu\n",
                                     static_cast<unsigned long long>(exp.total_traps),
                                     exp.form, exp.len, r.len);
                        return 9;
                    }
                    break;
                }
            }
            if (!found) {
                std::fprintf(stderr,
                             "FAIL: missing clwb_trapped record with total_traps=%llu (%s)\n",
                             static_cast<unsigned long long>(exp.total_traps), exp.form);
                return 10;
            }
        }
        std::printf("CLWB decoded lengths verified for sampled traps 1, 2, 4 "
                    "(%zu records)\n", records.size());
    }

    /* 3. Verify basic XMem allocation + transfer + stats contract */
    void* src = XMemAlloc(4096, 0);
    void* dst = XMemAlloc(4096, 0);
    if (!src || !dst) {
        std::fprintf(stderr, "FAIL: XMemAlloc returned null\n");
        return 2;
    }
    std::memset(src, 0x5A, 4096);
    if (!XMemTransferMemory(dst, src, 4096, 0) || std::memcmp(src, dst, 4096) != 0) {
        std::fprintf(stderr, "FAIL: XMemTransferMemory mismatch\n");
        return 3;
    }
    bx_xmem_stats_t stats = {};
    bx_xmem_get_internal_stats(&stats);
    std::printf("xmem stats: limit=%llu MB committed=%llu bytes\n",
                static_cast<unsigned long long>(stats.title_limit_bytes / (1024ULL * 1024ULL)),
                static_cast<unsigned long long>(stats.committed_virtual_bytes));
    XMemFree(src, 0);
    XMemFree(dst, 0);

    std::printf("PASS: test_veh_clwb\n");
    return 0;
}
