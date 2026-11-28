#ifndef BETTERXEN_SHIMS_XMEM_ACCOUNTING_H
#define BETTERXEN_SHIMS_XMEM_ACCOUNTING_H

#include <windows.h>

#include <cstdint>

namespace bx_xmem {

// Reserve or release physical-page bytes against the shared xmem title memory
// cap and working-set statistics (heap + virtual + physical <= title limit).
bool try_reserve_physical(uint64_t bytes);
void release_physical(uint64_t bytes);

// Reserves an XMEM_MAPPABLE range as a placeholder that XMemMapPhysicalPages maps into, and
// records its page size and the protection mapped views get. Returns the base, or NULL with
// the Win32 error set.
// With `adopt`, `address` is already a placeholder the title split out of its own
// MEM_RESERVE_PLACEHOLDER reservation (MEM_REPLACE_PLACEHOLDER) and is only recorded. A non-zero
// `highest` keeps a new reservation at or below that address (XMEM_GRAPHICS_32BIT_ADDRESS).
PVOID reserve_mappable(PVOID address, SIZE_T size, SIZE_T page, DWORD protect, bool adopt, uintptr_t highest);

// A MEM_RESERVE_PLACEHOLDER reservation the title manages itself (split with VirtualFree and
// MEM_PRESERVE_PLACEHOLDER). It is not recorded.
PVOID reserve_placeholder(PVOID address, SIZE_T size, uintptr_t highest);

} // namespace bx_xmem

#endif // BETTERXEN_SHIMS_XMEM_ACCOUNTING_H
