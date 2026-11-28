#include <windows.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <utility>

#include "bx_log.h"
#include "bx_xmem_internal.h"
#include "xmem_accounting.h"

namespace {
constexpr SIZE_T heap_alignment = 64;

struct VirtualCommit {
    uintptr_t begin;
    uintptr_t end;
    PVOID allocation_base;
};

// The shim build disables exceptions. Grow the table before committing pages
// so a bookkeeping allocation failure leaves the Win32 allocation untouched.
struct CommitTable {
    VirtualCommit* entries = nullptr;
    size_t count = 0;
    size_t capacity = 0;

    CommitTable() = default;
    CommitTable(const CommitTable&) = delete;
    CommitTable& operator=(const CommitTable&) = delete;
    ~CommitTable() { std::free(entries); }

    VirtualCommit* begin() const { return entries; }
    VirtualCommit* end() const { return count ? entries + count : entries; }

    bool append(VirtualCommit commit) {
        if (count == capacity) {
            const size_t max_entries = std::numeric_limits<size_t>::max() / sizeof(commit);
            if (capacity > max_entries / 2) return false;
            const size_t next_capacity = capacity ? capacity * 2 : 8;
            auto* next = static_cast<VirtualCommit*>(
                std::realloc(entries, next_capacity * sizeof(commit)));
            if (!next) return false;
            entries = next;
            capacity = next_capacity;
        }
        entries[count++] = commit;
        return true;
    }

    void swap(CommitTable& other) {
        std::swap(entries, other.entries);
        std::swap(count, other.count);
        std::swap(capacity, other.capacity);
    }
};

std::mutex usage_lock;
CommitTable virtual_commits;
uint64_t heap_bytes = 0;
uint64_t virtual_bytes = 0;
uint64_t physical_bytes = 0;
/* Bytes admitted through bx_xmem_try_charge_external() on behalf of other
 * DLLs (e.g. d3d12_xs private host heaps). Counted in can_commit so both
 * allocation orders (graphics-first or xmem-first) obey one title cap. */
uint64_t external_bytes = 0;
uint64_t peak_bytes = 0;

uint64_t title_limit() {
    const auto* config = bx_get_runtime_config();
    return config ? config->title_memory_limit_bytes : (8ULL << 30);
}

bool can_commit(uint64_t bytes) {
    const uint64_t limit = title_limit();
    const uint64_t used = heap_bytes + virtual_bytes + physical_bytes + external_bytes;
    return used <= limit && bytes <= limit - used;
}

void update_peak() {
    peak_bytes = std::max(peak_bytes, heap_bytes + virtual_bytes + physical_bytes + external_bytes);
}

uintptr_t region_end(const MEMORY_BASIC_INFORMATION& region) {
    return reinterpret_cast<uintptr_t>(region.BaseAddress) + region.RegionSize;
}

// The export table has no XMemVirtualFree. Reconcile tracked ranges with
// Win32 when the guest releases or decommits pages using VirtualFree.
bool refresh_virtual_commits() {
    CommitTable live;
    uint64_t bytes = 0;
    for (const auto& commit : virtual_commits) {
        uintptr_t cursor = commit.begin;
        while (cursor < commit.end) {
            MEMORY_BASIC_INFORMATION region{};
            if (!VirtualQuery(reinterpret_cast<PVOID>(cursor), &region, sizeof(region))) {
                break;
            }
            const uintptr_t end = std::min(commit.end, region_end(region));
            if (end <= cursor) break;
            if (region.State == MEM_COMMIT && region.AllocationBase == commit.allocation_base) {
                if (!live.append({cursor, end, commit.allocation_base})) return false;
                bytes += end - cursor;
            }
            cursor = end;
        }
    }
    virtual_commits.swap(live);
    virtual_bytes = bytes;
    return true;
}

void report_unsupported_host_hooks() {
    static const bool reported = [] {
        if (GetProcAddress(GetModuleHandleW(nullptr), "__XMemAllocationHooks__")) {
            BX_STUB_LOG("xmem.dll", "__XMemAllocationHooks__");
        }
        return true;
    }();
    (void)reported;
}

SIZE_T page_size() {
    static const SIZE_T size = [] {
        SYSTEM_INFO info{};
        GetSystemInfo(&info);
        return info.dwPageSize;
    }();
    return size;
}
} // namespace

extern "C" {

PVOID WINAPI XMemAllocDefault(SIZE_T size, uint64_t /*attributes*/) {
    if (size == 0) size = 16;
    if (size > std::numeric_limits<SIZE_T>::max() - heap_alignment) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }

    std::lock_guard guard(usage_lock);
    if (!can_commit(size) && (!refresh_virtual_commits() || !can_commit(size))) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }

    auto* allocation = static_cast<unsigned char*>(_aligned_malloc(size + heap_alignment, heap_alignment));
    if (!allocation) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }
    std::memcpy(allocation, &size, sizeof(size));
    heap_bytes += size;
    update_peak();
    return allocation + heap_alignment;
}

VOID WINAPI XMemFreeDefault(PVOID address, uint64_t /*attributes*/) {
    if (!address) return;
    auto* allocation = static_cast<unsigned char*>(address) - heap_alignment;
    SIZE_T size;
    std::memcpy(&size, allocation, sizeof(size));

    std::lock_guard guard(usage_lock);
    _aligned_free(allocation);
    heap_bytes -= size;
}

PVOID WINAPI XMemAlloc(SIZE_T size, uint64_t attributes) {
    report_unsupported_host_hooks();
    return XMemAllocDefault(size, attributes);
}

VOID WINAPI XMemFree(PVOID address, uint64_t attributes) {
    report_unsupported_host_hooks();
    XMemFreeDefault(address, attributes);
}

PVOID WINAPI XMemVirtualAlloc(PVOID address, SIZE_T size, DWORD alloc_type, DWORD protect) {
    /* Strip console-only high bits before forwarding to standard Win32 VirtualAlloc. */
    DWORD win_alloc = alloc_type & (MEM_COMMIT | MEM_RESERVE | MEM_RESET | MEM_TOP_DOWN);
    if (win_alloc == 0) win_alloc = MEM_COMMIT | MEM_RESERVE;
    /* Console callers may pass multi-bit Xbox memory attributes (e.g. 0x21) in arg 4;
     * valid Win32 base page protections are a single power-of-two bit in 0x01..0x80. */
    DWORD low_prot = protect & 0xFFu;
    DWORD win_prot = (low_prot != 0 && (low_prot & (low_prot - 1)) == 0)
        ? (protect & (0xFFu | PAGE_GUARD | PAGE_NOCACHE | PAGE_WRITECOMBINE))
        : PAGE_READWRITE;
    /* MEM_RESET combined with MEM_COMMIT is rejected by Win32 VirtualAlloc, so returning
     * early here cannot commit pages outside the working-set budget. */
    if (!(win_alloc & MEM_COMMIT) || (win_alloc & MEM_RESET)) {
        return VirtualAlloc(address, size, win_alloc, win_prot);
    }

    const uintptr_t requested = reinterpret_cast<uintptr_t>(address);
    const SIZE_T page = page_size();
    // Explicit reservations round down to allocation granularity.
    SIZE_T alignment = page;
    if (win_alloc & MEM_RESERVE) {
        SYSTEM_INFO info{};
        GetSystemInfo(&info);
        alignment = info.dwAllocationGranularity;
    }
    const uintptr_t begin = requested - requested % alignment;
    constexpr uintptr_t max_address = std::numeric_limits<uintptr_t>::max();
    if (!size || requested > max_address - (page - 1)
        || size > max_address - requested - (page - 1)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    const uintptr_t end = (requested + size + page - 1) & ~(page - 1);

    std::lock_guard guard(usage_lock);
    if (!refresh_virtual_commits()) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }

    CommitTable next;
    for (const auto& commit : virtual_commits) {
        if (!next.append(commit)) {
            SetLastError(ERROR_NOT_ENOUGH_MEMORY);
            return nullptr;
        }
    }

    uint64_t charge = end - begin;
    uintptr_t cursor = begin;
    if (address && !(win_alloc & MEM_RESERVE)) {
        for (const auto& commit : virtual_commits) {
            if (commit.end <= cursor) continue;
            if (commit.begin >= end) break;
            if (cursor < commit.begin && !next.append({cursor, commit.begin, nullptr})) {
                SetLastError(ERROR_NOT_ENOUGH_MEMORY);
                return nullptr;
            }
            const uintptr_t overlap_end = std::min(end, commit.end);
            charge -= overlap_end - std::max(cursor, commit.begin);
            cursor = overlap_end;
        }
    }
    if ((cursor < end && !next.append({cursor, end, nullptr})) || !can_commit(charge)) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }

    PVOID allocation = VirtualAlloc(address, size, win_alloc, win_prot);
    if (!allocation || next.count == virtual_commits.count) return allocation;

    MEMORY_BASIC_INFORMATION region{};
    // A successful commit establishes a region containing the returned address.
    VirtualQuery(allocation, &region, sizeof(region));
    const uintptr_t relocation = address ? 0 : reinterpret_cast<uintptr_t>(allocation);
    for (size_t i = virtual_commits.count; i < next.count; ++i) {
        next.entries[i].begin += relocation;
        next.entries[i].end += relocation;
        next.entries[i].allocation_base = region.AllocationBase;
    }
    std::sort(next.begin(), next.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.begin < rhs.begin;
    });
    virtual_commits.swap(next);
    virtual_bytes += charge;
    update_peak();
    return allocation;
}

SIZE_T WINAPI XMemVirtualQuery(LPCVOID address, PMEMORY_BASIC_INFORMATION buf, SIZE_T len) {
    return VirtualQuery(address, buf, len);
}

BOOL WINAPI XMemTransferMemory(PVOID dst, LPCVOID src, SIZE_T bytes, uint64_t /*flags*/) {
    if (bytes == 0) return TRUE;
    if (!dst || !src) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    std::memmove(dst, src, bytes);
    return TRUE;
}

BOOL WINAPI XMemGetWorkingSetStatistics(PVOID out_stats, SIZE_T stats_size) {
    if (!out_stats || stats_size < sizeof(bx_xmem_stats_t)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    std::lock_guard guard(usage_lock);
    if (!refresh_virtual_commits()) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }
    const bx_xmem_stats_t stats{title_limit(), heap_bytes + virtual_bytes + external_bytes, physical_bytes, peak_bytes};
    std::memset(out_stats, 0, stats_size);
    std::memcpy(out_stats, &stats, sizeof(stats));
    return TRUE;
}

BOOL WINAPI XMemIsToolsMemAvailable(void) {
    return FALSE;
}

void bx_xmem_get_internal_stats(bx_xmem_stats_t* out_stats) {
    if (!out_stats) return;
    if (!XMemGetWorkingSetStatistics(out_stats, sizeof(*out_stats))) {
        // The internal ABI has no error return. Preserve the most recent
        // accounting snapshot if refreshing it runs out of bookkeeping memory.
        std::lock_guard guard(usage_lock);
        *out_stats = {title_limit(), heap_bytes + virtual_bytes + external_bytes, physical_bytes, peak_bytes};
    }
}

int bx_xmem_try_charge_external(uint64_t bytes) {
    std::lock_guard guard(usage_lock);
    if (!can_commit(bytes)) {
        if (!refresh_virtual_commits()) {
            // Bookkeeping ran out of memory: not a budget decision, so say so.
            bx_log_emit(BX_LOG_ERROR, "xmem", "external_charge",
                        "refresh_virtual_commits failed; charge refused without a budget check");
            return 0;
        }
        if (!can_commit(bytes)) {
            return 0;
        }
    }
    external_bytes += bytes;
    update_peak();
    return 1;
}

void bx_xmem_release_charge_external(uint64_t bytes) {
    std::lock_guard guard(usage_lock);
    if (bytes > external_bytes) {
        // Refuse: zeroing would also drop other owners' outstanding charges.
        bx_log_emit(BX_LOG_ERROR, "xmem", "external_charge",
                    "release exceeds outstanding external charge; counter left unchanged");
        return;
    }
    external_bytes -= bytes;
}

} // extern "C"

namespace bx_xmem {

bool try_reserve_physical(uint64_t bytes) {
    std::lock_guard guard(usage_lock);
    if (!can_commit(bytes) && (!refresh_virtual_commits() || !can_commit(bytes))) {
        return false;
    }
    physical_bytes += bytes;
    update_peak();
    return true;
}

void release_physical(uint64_t bytes) {
    std::lock_guard guard(usage_lock);
    physical_bytes = physical_bytes >= bytes ? physical_bytes - bytes : 0;
}

} // namespace bx_xmem
