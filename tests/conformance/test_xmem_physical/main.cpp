/*
 * Physical pages in xmem.dll, against the signatures and rules of the authorized GDK
 * documentation: XMemAllocatePhysicalPages(XMemFlags, NumberOfPages, PageArray) in 64 KB pages,
 * XMemFreePhysicalPages(NumberOfPages, PageArray), XMemMapPhysicalPages(VirtualAddress,
 * NumberOfPages, PageArray) and XMemMapPhysicalPagesScatter(VirtualAddressArray, NumberOfPages,
 * PageArray), mapping into XMemVirtualAlloc(MEM_RESERVE | MEM_64K_PAGES, XMEM_MAPPABLE)
 * reservations. The constants below are written out from that documentation.
 */
#include <windows.h>

#include <cstdint>
#include <cstdio>

#include "bx_xmem_internal.h"

extern "C" {
PVOID WINAPI XMemAllocDefault(SIZE_T, uint64_t);
VOID WINAPI XMemFreeDefault(PVOID, uint64_t);
PVOID WINAPI XMemVirtualAlloc(PVOID, SIZE_T, DWORD, uint64_t, DWORD);
BOOL WINAPI XMemAllocatePhysicalPages(uint64_t, PULONG_PTR, PULONG_PTR);
BOOL WINAPI XMemFreePhysicalPages(ULONG_PTR, PULONG_PTR);
PVOID WINAPI XMemMapPhysicalPages(PVOID, ULONG_PTR, PULONG_PTR);
BOOL WINAPI XMemMapPhysicalPagesScatter(PVOID*, ULONG_PTR, PULONG_PTR);
BOOL WINAPI XMemGetWorkingSetStatistics(PVOID, SIZE_T);
}

namespace {

constexpr SIZE_T kPage = 64 * 1024;
constexpr DWORD kMem64KPages = 0x20400000;
constexpr DWORD kMem2MBPages = 0x20000000;
constexpr uint64_t kXMemCpu = 0x1;
constexpr uint64_t kXMemMappable = 0x20;
constexpr uint64_t kXMem2MBClusters = 0x20000000;
constexpr uint64_t kXMemGpuOptimalBandwidthRequired = 0x280;
constexpr uint64_t kXMemGraphics32BitAddress = 0x7;
constexpr DWORD kMemReservePlaceholder = 0x40000;
constexpr DWORD kMemReplacePlaceholder = 0x4000;
constexpr DWORD kPageGraphicsReadWrite = 0x2000;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            std::fprintf(stderr, "xmem physical:%d: %s (Win32 error %lu)\n",      \
                         __LINE__, #condition, static_cast<unsigned long>(GetLastError())); \
            return false;                                                       \
        }                                                                       \
    } while (false)

DWORD state_of(const void* address) {
    MEMORY_BASIC_INFORMATION region{};
    return VirtualQuery(address, &region, sizeof(region)) ? region.State : 0;
}

bool check_physical_pages() {
    bx_xmem_stats_t stats{};
    CHECK(XMemGetWorkingSetStatistics(&stats, sizeof(stats)));
    CHECK(stats.allocated_physical_bytes == 0);

    // Flags outside 2 MB clusters and the bandwidth values are refused before anything changes.
    ULONG_PTR count = 4;
    ULONG_PTR pages[4]{};
    CHECK(!XMemAllocatePhysicalPages(0x12345, &count, pages));
    CHECK(GetLastError() == ERROR_INVALID_PARAMETER && count == 4 && !pages[0]);
    CHECK(!XMemAllocatePhysicalPages(kXMem2MBClusters, &count, pages));
    CHECK(GetLastError() == ERROR_NOT_SUPPORTED && !pages[0]);

    // A bandwidth value is accepted; pages are 64 KB and counted against the title budget.
    CHECK(XMemAllocatePhysicalPages(kXMemGpuOptimalBandwidthRequired, &count, pages));
    CHECK(count == 4 && pages[0] && pages[1] && pages[2] && pages[3]);
    CHECK(pages[0] != pages[1] && pages[1] != pages[2] && pages[2] != pages[3]);
    bx_xmem_get_internal_stats(&stats);
    CHECK(stats.allocated_physical_bytes == 4 * kPage);
    bx_xmem_stats_t ws_stats{};
    CHECK(XMemGetWorkingSetStatistics(&ws_stats, sizeof(ws_stats)));
    CHECK(ws_stats.allocated_physical_bytes == 4 * kPage);

    ULONG_PTR cap_pages = static_cast<ULONG_PTR>(stats.title_limit_bytes / kPage);
    CHECK(!XMemAllocatePhysicalPages(0, &cap_pages, pages));
    CHECK(GetLastError() == ERROR_NOT_ENOUGH_MEMORY);
    CHECK(!XMemAllocDefault(static_cast<SIZE_T>(stats.title_limit_bytes), 0));
    CHECK(GetLastError() == ERROR_NOT_ENOUGH_MEMORY);

    // A mappable reservation stays reserved, uncommitted, until pages are mapped into it.
    auto* base = static_cast<BYTE*>(XMemVirtualAlloc(nullptr, 8 * kPage, MEM_RESERVE | kMem64KPages,
                                                     kXMemMappable | kXMemCpu, PAGE_READWRITE));
    CHECK(base && reinterpret_cast<uintptr_t>(base) % kPage == 0);
    CHECK(state_of(base) == MEM_RESERVE && state_of(base + 7 * kPage) == MEM_RESERVE);
    CHECK(!XMemVirtualAlloc(nullptr, kPage, MEM_RESERVE | MEM_COMMIT | kMem64KPages, kXMemMappable,
                            PAGE_READWRITE));
    CHECK(GetLastError() == ERROR_INVALID_PARAMETER);

    // Map four pages, then alias them in reverse order in the next four slots.
    CHECK(XMemMapPhysicalPages(base, 4, pages) == base);
    for (int i = 0; i < 4; ++i) base[i * kPage + 9] = static_cast<BYTE>(0x40 + i);
    ULONG_PTR reversed[] = {pages[3], pages[2], pages[1], pages[0]};
    CHECK(XMemMapPhysicalPages(base + 4 * kPage, 4, reversed) == base + 4 * kPage);
    CHECK(base[4 * kPage + 9] == 0x43 && base[7 * kPage + 9] == 0x40);
    base[5 * kPage + 1] = 0x7e;
    CHECK(base[2 * kPage + 1] == 0x7e);

    bx_xmem_region_info_t info{};
    CHECK(bx_xmem_query_region(reinterpret_cast<uintptr_t>(base + 4 * kPage + 3), &info));
    CHECK(info.base_va == reinterpret_cast<uintptr_t>(base + 4 * kPage) && info.physical_tag == pages[3]);
    CHECK(info.size_bytes == kPage && info.protect_flags == PAGE_READWRITE && info.mem_type_flags == MEM_MAPPED);

    // Mapping over mapped pages replaces them.
    CHECK(XMemMapPhysicalPages(base, 4, reversed) == base);
    CHECK(base[9] == 0x43 && base[3 * kPage + 9] == 0x40);

    // A NULL page array unmaps; the range stays reserved and can be mapped again.
    CHECK(XMemMapPhysicalPages(base, 4, nullptr) == base);
    CHECK(state_of(base) == MEM_RESERVE && !bx_xmem_query_region(reinterpret_cast<uintptr_t>(base), &info));
    CHECK(base[4 * kPage + 9] == 0x43);
    CHECK(XMemMapPhysicalPages(base, 4, pages) == base);
    CHECK(base[9] == 0x40);

    // Mapping outside a mappable reservation is refused.
    void* plain = VirtualAlloc(nullptr, kPage, MEM_RESERVE, PAGE_NOACCESS);
    CHECK(plain && !XMemMapPhysicalPages(plain, 1, pages));
    CHECK(GetLastError() == ERROR_INVALID_PARAMETER);
    VirtualFree(plain, 0, MEM_RELEASE);

    // Scatter: parallel arrays of slots and tags; a 0 tag unmaps its slot.
    PVOID slots[] = {base + 7 * kPage, base + 6 * kPage};
    ULONG_PTR scatter_pages[] = {pages[0], 0};
    CHECK(XMemMapPhysicalPagesScatter(slots, 2, scatter_pages));
    CHECK(base[7 * kPage + 9] == 0x40 && state_of(base + 6 * kPage) == MEM_RESERVE);
    // One bad slot refuses the whole call before anything changes.
    PVOID bad_slots[] = {base + 6 * kPage, base + 1};
    ULONG_PTR bad_pages[] = {pages[1], pages[2]};
    CHECK(!XMemMapPhysicalPagesScatter(bad_slots, 2, bad_pages));
    CHECK(GetLastError() == ERROR_INVALID_PARAMETER && state_of(base + 6 * kPage) == MEM_RESERVE);
    CHECK(!XMemMapPhysicalPagesScatter(nullptr, 1, bad_pages));
    CHECK(XMemMapPhysicalPagesScatter(slots, 2, nullptr));
    CHECK(state_of(base + 7 * kPage) == MEM_RESERVE);

    // Frees with stale or duplicate tags fail without changing the array or live mappings.
    ULONG_PTR stale[] = {pages[0], 0};
    CHECK(!XMemFreePhysicalPages(2, stale));
    CHECK(GetLastError() == ERROR_INVALID_PARAMETER && stale[0] == pages[0] && stale[1] == 0);
    ULONG_PTR dup_free[] = {pages[0], pages[0]};
    CHECK(!XMemFreePhysicalPages(2, dup_free));
    CHECK(GetLastError() == ERROR_INVALID_PARAMETER && base[9] == 0x40);

    // Freeing unmaps every view of the pages and leaves the reservation reserved.
    CHECK(XMemFreePhysicalPages(4, pages));
    CHECK(!pages[0] && !pages[3]);
    CHECK(state_of(base) == MEM_RESERVE && state_of(base + 4 * kPage) == MEM_RESERVE);
    CHECK(!XMemMapPhysicalPages(base, 4, reversed));
    CHECK(GetLastError() == ERROR_INVALID_PARAMETER);
    bx_xmem_get_internal_stats(&stats);
    CHECK(stats.allocated_physical_bytes == 0);
    CHECK(VirtualFree(base, 0, MEM_RELEASE));

    // 2 MB-page reservations exist, but mapping into them is not implemented.
    ULONG_PTR one = 1;
    ULONG_PTR page[1]{};
    CHECK(XMemAllocatePhysicalPages(0, &one, page));
    auto* large = XMemVirtualAlloc(nullptr, 2 * 1024 * 1024, MEM_RESERVE | kMem2MBPages, kXMemMappable,
                                   PAGE_READWRITE);
    CHECK(large && reinterpret_cast<uintptr_t>(large) % (2 * 1024 * 1024) == 0);
    CHECK(!XMemMapPhysicalPages(large, 1, page));
    CHECK(GetLastError() == ERROR_NOT_SUPPORTED);
    VirtualFree(large, 0, MEM_RELEASE);

    // A title can manage the placeholder itself: reserve one below 4 GB, split it with VirtualFree
    // and MEM_PRESERVE_PLACEHOLDER, turn one part into a mappable range with MEM_REPLACE_PLACEHOLDER
    // (with GPU protection bits the host ignores) and map pages into it.
    auto* region = static_cast<BYTE*>(XMemVirtualAlloc(nullptr, 4 * kPage, MEM_RESERVE | kMemReservePlaceholder,
                                                       kXMemGraphics32BitAddress, PAGE_NOACCESS));
    CHECK(region && reinterpret_cast<uintptr_t>(region) + 4 * kPage <= 0x100000000ull);
    CHECK(VirtualFree(region, 2 * kPage, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER));
    auto* part = static_cast<BYTE*>(XMemVirtualAlloc(region, 2 * kPage,
                                                     MEM_RESERVE | kMemReplacePlaceholder | kMem64KPages,
                                                     kXMemMappable | kXMemGraphics32BitAddress,
                                                     PAGE_READWRITE | kPageGraphicsReadWrite));
    CHECK(part == region);
    ULONG_PTR two = 2;
    ULONG_PTR part_pages[2]{};
    CHECK(XMemAllocatePhysicalPages(kXMemGpuOptimalBandwidthRequired, &two, part_pages));
    CHECK(XMemMapPhysicalPages(part, 2, part_pages) == part);
    part[kPage + 5] = 0x6c;
    CHECK(part[kPage + 5] == 0x6c && state_of(part) == MEM_COMMIT);
    CHECK(state_of(region + 2 * kPage) == MEM_RESERVE);
    // Replacing a range that is not a placeholder fails.
    CHECK(!XMemVirtualAlloc(part, kPage, MEM_RESERVE | kMemReplacePlaceholder | kMem64KPages, kXMemMappable,
                            PAGE_READWRITE));
    CHECK(XMemFreePhysicalPages(2, part_pages));
    CHECK(state_of(part) == MEM_RESERVE);

    // BetterXen extension: a NULL address picks a span; unmapping it frees it.
    auto* picked = static_cast<BYTE*>(XMemMapPhysicalPages(nullptr, 1, page));
    CHECK(picked && reinterpret_cast<uintptr_t>(picked) % kPage == 0);
    picked[0] = 0x5a;
    CHECK(XMemMapPhysicalPages(picked, 1, nullptr) == picked);
    CHECK(state_of(picked) == MEM_FREE);
    CHECK(XMemFreePhysicalPages(1, page));
    return true;
}

} // namespace

int main() {
    if (!check_physical_pages()) {
        return 1;
    }
    std::puts("test_xmem_physical: passed");
}
