#ifndef BETTERXEN_BX_XMEM_INTERNAL_H
#define BETTERXEN_BX_XMEM_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Internal cross-DLL contract between Track B (xmem.dll) and Track C (d3d12_xs.dll).
 * Allows d3d12_xs placed-resource and virtual-memory translation to query allocations
 * created through XMemVirtualAlloc / XMemAllocatePhysicalPages / XMemMapPhysicalPages.
 */

typedef struct bx_xmem_region_info {
    uint64_t base_va;
    uint64_t size_bytes;
    uint64_t page_size_bytes;
    uint32_t protect_flags;
    uint32_t mem_type_flags;
    uint64_t physical_tag;
} bx_xmem_region_info_t;

typedef struct bx_xmem_stats {
    uint64_t title_limit_bytes;
    uint64_t committed_virtual_bytes;
    uint64_t allocated_physical_bytes;
    uint64_t peak_committed_bytes;
} bx_xmem_stats_t;

/* Query the XMem region covering `va`. Returns 1 if tracked, 0 otherwise. */
int bx_xmem_query_region(uint64_t va, bx_xmem_region_info_t* out_info);

/* Snapshot current XMem usage against the active Lockhart/Anaconda cap.
 * committed_virtual_bytes includes bytes admitted through
 * bx_xmem_try_charge_external(), so committed + physical is everything the
 * cap counts and peak_committed_bytes never exceeds what was reported. */
void bx_xmem_get_internal_stats(bx_xmem_stats_t* out_stats);

/* Admit `bytes` against the combined title cap, counting every owner.
 * Returns 1 on success, 0 when the cap would be exceeded or xmem's own
 * bookkeeping could not be refreshed (logged as external_charge). On success the
 * caller owns the charge and must release it exactly once with
 * bx_xmem_release_charge_external(). Never silently succeeds on failure. */
int bx_xmem_try_charge_external(uint64_t bytes);

/* Release a charge previously admitted by bx_xmem_try_charge_external().
 * Releasing more than is outstanding is refused: the counter is left
 * unchanged and an external_charge error is logged. */
void bx_xmem_release_charge_external(uint64_t bytes);

#ifdef __cplusplus
}
#endif

#endif /* BETTERXEN_BX_XMEM_INTERNAL_H */
