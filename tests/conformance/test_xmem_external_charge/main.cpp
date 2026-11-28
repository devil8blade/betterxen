// Synthetic check for bx_xmem_try_charge_external / bx_xmem_release_charge_external
// (#550): an external charge counts against the same title cap as XMem's own
// allocations, shows up in the reported stats, and an over-release is refused.
#include <windows.h>

#include <cstdint>
#include <cstdio>

#include "bx_xmem_internal.h"

extern "C" {
PVOID WINAPI XMemVirtualAlloc(PVOID, SIZE_T, DWORD, DWORD);
BOOL WINAPI XMemGetWorkingSetStatistics(PVOID, SIZE_T);
}

#define CHECK(condition)                                                          \
    do {                                                                          \
        if (!(condition)) {                                                       \
            std::fprintf(stderr, "xmem_external:%d: %s (Win32 error %lu)\n",      \
                         __LINE__, #condition,                                    \
                         static_cast<unsigned long>(GetLastError()));             \
            return 1;                                                             \
        }                                                                         \
    } while (false)

int main() {
    SYSTEM_INFO sys{};
    GetSystemInfo(&sys);
    const SIZE_T page = sys.dwPageSize;

    bx_xmem_stats_t stats{};
    CHECK(XMemGetWorkingSetStatistics(&stats, sizeof(stats)));
    const uint64_t used = stats.committed_virtual_bytes + stats.allocated_physical_bytes;
    CHECK(used < stats.title_limit_bytes);
    // Only bookkeeping is charged; nothing of this size is allocated on the host.
    const uint64_t headroom = stats.title_limit_bytes - used;

    // Graphics-first order: an external charge fills the cap...
    CHECK(bx_xmem_try_charge_external(headroom) == 1);
    CHECK(bx_xmem_try_charge_external(1) == 0);

    // ...is visible in the reported usage, and peak never exceeds it...
    CHECK(XMemGetWorkingSetStatistics(&stats, sizeof(stats)));
    CHECK(stats.committed_virtual_bytes + stats.allocated_physical_bytes == stats.title_limit_bytes);
    CHECK(stats.peak_committed_bytes <= stats.title_limit_bytes);
    bx_xmem_stats_t internal{};
    bx_xmem_get_internal_stats(&internal);
    CHECK(internal.committed_virtual_bytes == stats.committed_virtual_bytes);

    // ...and a later XMem commit is refused against the same cap.
    SetLastError(0);
    CHECK(!XMemVirtualAlloc(nullptr, page, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    CHECK(GetLastError() == ERROR_NOT_ENOUGH_MEMORY);

    // An over-release is refused and leaves the outstanding charge in place.
    bx_xmem_release_charge_external(headroom + 1);
    CHECK(bx_xmem_try_charge_external(1) == 0);
    CHECK(XMemGetWorkingSetStatistics(&stats, sizeof(stats)));
    CHECK(stats.committed_virtual_bytes + stats.allocated_physical_bytes == stats.title_limit_bytes);

    // Releasing the real charge admits the same XMem commit again.
    bx_xmem_release_charge_external(headroom);
    CHECK(XMemGetWorkingSetStatistics(&stats, sizeof(stats)));
    CHECK(stats.committed_virtual_bytes + stats.allocated_physical_bytes == used);
    void* block = XMemVirtualAlloc(nullptr, page, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    CHECK(block != nullptr);
    CHECK(VirtualFree(block, 0, MEM_RELEASE));

    std::printf("PASS: test_xmem_external_charge\n");
    return 0;
}
