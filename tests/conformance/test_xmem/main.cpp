#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "bx_log.h"
#include "bx_xmem_internal.h"

extern "C" {
PVOID WINAPI XMemAlloc(SIZE_T, uint64_t);
PVOID WINAPI XMemAllocDefault(SIZE_T, uint64_t);
VOID WINAPI XMemFree(PVOID, uint64_t);
VOID WINAPI XMemFreeDefault(PVOID, uint64_t);
PVOID WINAPI XMemVirtualAlloc(PVOID, SIZE_T, DWORD, uint64_t, DWORD);
SIZE_T WINAPI XMemVirtualQuery(LPCVOID, PMEMORY_BASIC_INFORMATION, SIZE_T);
BOOL WINAPI XMemTransferMemory(PVOID, LPCVOID, SIZE_T, uint64_t);
BOOL WINAPI XMemGetWorkingSetStatistics(PVOID, SIZE_T);
BOOL WINAPI XMemIsToolsMemAvailable();
}

/* XMEM_CPU from the authorized GDK documentation of XMemVirtualAlloc. */
constexpr uint64_t kXMemCpu = 0x1;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            std::fprintf(stderr, "xmem:%d: %s (Win32 error %lu)\n", __LINE__,       \
                         #condition, static_cast<unsigned long>(GetLastError())); \
            return 1;                                                           \
        }                                                                       \
    } while (false)

int main() {
    SYSTEM_INFO sys{};
    GetSystemInfo(&sys);
    const SIZE_T page = sys.dwPageSize;
    const auto* config = bx_get_runtime_config();

    bx_xmem_stats_t stats{};
    CHECK(config && XMemGetWorkingSetStatistics(&stats, sizeof(stats)));
    char profile[16]{};
    GetEnvironmentVariableA("BX_PROFILE", profile, sizeof(profile));
    const uint64_t expected_limit = std::strcmp(profile, "anaconda") == 0
        ? 13500ULL * 1024 * 1024
        : 8ULL * 1024 * 1024 * 1024;
    CHECK(stats.title_limit_bytes == expected_limit);
    CHECK(stats.title_limit_bytes == config->title_memory_limit_bytes);
    CHECK(stats.committed_virtual_bytes == 0 && stats.allocated_physical_bytes == 0);
    CHECK(!XMemIsToolsMemAvailable());
    CHECK(!XMemGetWorkingSetStatistics(&stats, sizeof(stats) - 1));
    CHECK(GetLastError() == ERROR_INVALID_PARAMETER);

    unsigned char large_stats[sizeof(bx_xmem_stats_t) + 16];
    std::memset(large_stats, 0xCC, sizeof(large_stats));
    CHECK(XMemGetWorkingSetStatistics(large_stats, sizeof(large_stats)));
    for (size_t i = sizeof(bx_xmem_stats_t); i < sizeof(large_stats); ++i) {
        CHECK(large_stats[i] == 0);
    }

    auto* ordinary = XMemAlloc(73, 0);
    auto* empty = XMemAllocDefault(0, 0);
    CHECK(ordinary && empty);
    CHECK(reinterpret_cast<uintptr_t>(ordinary) % 64 == 0);
    CHECK(reinterpret_cast<uintptr_t>(empty) % 64 == 0);
    bx_xmem_get_internal_stats(&stats);
    CHECK(stats.committed_virtual_bytes == 89);

    // The existing heap charge should reject requests at the full cap
    // without allocating gigabytes on the test host.
    CHECK(!XMemAllocDefault(stats.title_limit_bytes, 0));
    CHECK(GetLastError() == ERROR_NOT_ENOUGH_MEMORY);
    CHECK(!XMemVirtualAlloc(nullptr, stats.title_limit_bytes,
                            MEM_COMMIT | MEM_RESERVE, kXMemCpu, PAGE_READWRITE));
    CHECK(GetLastError() == ERROR_NOT_ENOUGH_MEMORY);

    XMemFreeDefault(empty, 0);
    XMemFree(ordinary, 0);

    auto* reserved = static_cast<unsigned char*>(
        XMemVirtualAlloc(nullptr, page * 3, MEM_RESERVE, kXMemCpu, PAGE_READWRITE));
    CHECK(reserved && reinterpret_cast<uintptr_t>(reserved) % page == 0);
    CHECK(XMemVirtualAlloc(reserved, page * 2, MEM_COMMIT, kXMemCpu, PAGE_READWRITE) == reserved);
    CHECK(XMemVirtualAlloc(reserved + page + 1, page * 2 - 1, MEM_COMMIT, kXMemCpu, PAGE_READWRITE)
          == reserved + page);

    MEMORY_BASIC_INFORMATION region{};
    CHECK(XMemVirtualQuery(reserved + page, &region, sizeof(region)) == sizeof(region));
    CHECK(region.AllocationBase == reserved && region.State == MEM_COMMIT);
    CHECK(region.Protect == PAGE_READWRITE && region.RegionSize == page * 2);

    constexpr char sample[] = "betterxen";
    CHECK(XMemTransferMemory(reserved, sample, sizeof(sample), 0));
    CHECK(XMemTransferMemory(reserved + 1, reserved, sizeof(sample), 0));
    CHECK(std::memcmp(reserved + 1, sample, sizeof(sample)) == 0);

    bx_xmem_get_internal_stats(&stats);
    CHECK(stats.committed_virtual_bytes == page * 3 && stats.peak_committed_bytes == page * 3);
    CHECK(VirtualFree(reserved + page, page, MEM_DECOMMIT));
    bx_xmem_get_internal_stats(&stats);
    CHECK(stats.committed_virtual_bytes == page * 2 && stats.peak_committed_bytes == page * 3);
    CHECK(VirtualFree(reserved, 0, MEM_RELEASE));
    bx_xmem_get_internal_stats(&stats);
    CHECK(stats.committed_virtual_bytes == 0 && stats.peak_committed_bytes == page * 3);

    // Documented form: XMem flags in argument 4, page protection in argument 5, and a page-size
    // bit (MEM_64K_PAGES, 0x20400000) in the allocation type.
    auto* console_res = static_cast<unsigned char*>(
        XMemVirtualAlloc(nullptr, page * 2, MEM_RESERVE | 0x20400000u, kXMemCpu, PAGE_READWRITE));
    CHECK(console_res != nullptr);
    CHECK(XMemVirtualAlloc(console_res, page, MEM_COMMIT, kXMemCpu, PAGE_READWRITE) == console_res);
    MEMORY_BASIC_INFORMATION console_region{};
    CHECK(VirtualQuery(console_res, &console_region, sizeof(console_region)));
    CHECK(console_region.Protect == PAGE_READWRITE);
    console_res[0] = 0x5Au;
    CHECK(console_res[0] == 0x5Au);
    CHECK(VirtualFree(console_res, 0, MEM_RELEASE));
    bx_xmem_get_internal_stats(&stats);
    CHECK(stats.committed_virtual_bytes == 0);
    std::puts("test_xmem: passed");
}
