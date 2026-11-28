#ifndef BETTERXEN_D3D12_XS_VMEM_H
#define BETTERXEN_D3D12_XS_VMEM_H

#include <stddef.h>
#include <stdint.h>
#include <windows.h>

/*
 * BetterXen-defined parameter block for D3D12ConfigureVirtualMemoryX.
 *
 * The export is PVOID and the frozen ABI has no size argument (abi/ is out of
 * scope). The first 8 bytes are therefore the initial probe: a magic word and
 * the following 32-bit value. Magic is 'BXVM' (0x4D565842); a matching header
 * and struct size select the full BetterXen block. A zero first word selects
 * the Xbox decoder, which checks the full block and may write its two address
 * outputs. Other headers (including unrecognized nonzero Xbox flags) fall
 * back with S_OK and no writes. A null pointer is not read.
 *
 * A block whose magic and size match is then checked in full. version is 1.
 * flags may be 0 or BX_D3D12_VMEM_FLAG_BACKING_HEAP. Any other flag bit, or a
 * non-zero reserved word, is not this struct either (logged S_OK, no write).
 * A recognised block with a bad size or a base_va that bx_xmem_query_region
 * does not resolve returns E_INVALIDARG even if its output is read-only. Valid
 * inputs with an initially unwritable output fall back with S_OK and a stub
 * log. A failed write after the accessibility check returns E_INVALIDARG.
 *
 * base_va is 0, or an address bx_xmem_query_region resolves. It is not a GPU pointer.
 * size_bytes is 0 or a 64 KiB multiple that does not exceed the active title limit.
 *
 * The call copies what it needs. The caller's pointer is not kept. cpu_pointer
 * is written only before this call returns: the persistent Map address of the
 * WRITE_BACK buffer if that backing heap already exists, otherwise 0. CreateHeap
 * does not write the block. A caller that needs the address after the backing
 * heap has been created calls D3D12ConfigureVirtualMemoryX again with the same
 * region (same base_va and size). That second call does not reset or recreate
 * the backing heap. It writes the current map into the new block. A nonzero
 * cpu_pointer is valid only while that backing buffer remains cached. Changing
 * the region or replacing the cache for another host device unmaps it, even if
 * the caller retains the heap. Callers must synchronize those operations with
 * access through the pointer and obtain a fresh pointer after replacement.
 *
 * CreateHeap returns that backing heap only when all of the following hold:
 * the block opted in with BX_D3D12_VMEM_FLAG_BACKING_HEAP, the region size is
 * non-zero, and the desc is the heap this file creates for the region:
 * Type CUSTOM, CPUPageProperty WRITE_BACK, MemoryPoolPreference L0,
 * SizeInBytes equal to the configured region, Alignment 0 or 64 KiB,
 * Flags exactly D3D12_HEAP_FLAG_NONE (the flag set used when the backing heap
 * is created), and CreationNodeMask and VisibleNodeMask each 0 or 1.
 * If the backing configuration changes during a backing request, CreateHeap
 * returns HRESULT_FROM_WIN32(ERROR_RETRY) and discards any newly allocated heap.
 * Any other CreateHeap is the host CreateHeap. The desc is not modified.
 * The live-heap budget charge is taken on create and dropped on the wrapper's
 * final Release. A resource created by CreatePlacedResource holds the wrapper
 * for its lifetime.
 * GetDevice on every wrapper returns the BxDeviceProxy. An ordinary heap AddRefs
 * that proxy until the wrapper's final Release, matching BxChildProxy, so the
 * caller can release the device and keep the heap. The process-wide backing heap
 * does not AddRef. The cache would keep the device and its charge alive. That
 * pointer is weak. It is cleared when the heap leaves the cache (a new region
 * or a different host device) and when that proxy's last reference drops.
 * CreatePlacedResource1 and CreatePlacedResource2 unwrap the wrapper to the
 * host heap before the host call. The cache records the host ID3D12Device
 * that built the heap. CreateHeap for a different host device releases the
 * heap and its buffer, credits the charge, and builds a new one. CreateHeap
 * for the same host device returns the cached heap. The check, the retarget,
 * and the caller's reference happen in one critical section.
 *
 * The layout must match tests/conformance/test_d3d12_placed/main.cpp.
 */
constexpr uint32_t BX_D3D12_VMEM_MAGIC = 0x4D565842u;
constexpr uint32_t BX_D3D12_VMEM_FLAG_BACKING_HEAP = 0x1u;

struct BxD3d12VirtualMemoryConfig {
    uint32_t magic;
    uint32_t struct_size;
    uint32_t version;
    uint32_t flags;
    uint64_t base_va;
    uint64_t size_bytes;
    uint64_t reserved;
    uint64_t cpu_pointer;
};
static_assert(sizeof(BxD3d12VirtualMemoryConfig) == 48, "config block is 48 bytes");
static_assert(offsetof(BxD3d12VirtualMemoryConfig, magic) == 0, "magic offset");
static_assert(offsetof(BxD3d12VirtualMemoryConfig, struct_size) == 4, "struct_size offset");
static_assert(offsetof(BxD3d12VirtualMemoryConfig, version) == 8, "version offset");
static_assert(offsetof(BxD3d12VirtualMemoryConfig, flags) == 12, "flags offset");
static_assert(offsetof(BxD3d12VirtualMemoryConfig, base_va) == 16, "base_va offset");
static_assert(offsetof(BxD3d12VirtualMemoryConfig, size_bytes) == 24, "size_bytes offset");
static_assert(offsetof(BxD3d12VirtualMemoryConfig, reserved) == 32, "reserved offset");
static_assert(offsetof(BxD3d12VirtualMemoryConfig, cpu_pointer) == 40, "cpu_pointer offset");

/*
 * The Xbox virtual-memory configuration (D3D12XBOX_VIRTUAL_MEMORY_CONFIGURATION, d3d12_xs.h).
 *
 * Source: the authorized GDK documentation of D3D12ConfigureVirtualMemoryX and the
 * structure. Call-site observation of a project-authorized target build agrees with it: a
 * zero-initialized block with 3 or 4 in the page-table field and no flags.
 *
 * Documented: flags must be none (0); the GPU page table is sized in 4 MB units, at least 1
 * and clamped to 33 (default 5, 20 MB); the frame-buffer preserve size is 0 (none), -1 (the
 * legacy size, about 10 MB) or a byte count clamped to 64 MB; the two addresses are outputs,
 * the minimum always 2 GB; and the call fails with E_FAIL once a D3D12 device exists.
 *
 * The host does not size GPU page tables or keep a cross-process frame buffer, so the
 * decoder records both, writes the outputs and still logs BX_STUB_LOG. The maximum address
 * is documented only for the default size; for other sizes it is scaled linearly from that
 * value, an inference the log marks as max_estimated=1. A page count of 0 is refused: the
 * value of the header's DEFAULT constant is not documented.
 */
struct BxXboxVirtualMemoryConfig {
    uint32_t flags;
    uint32_t page_table_memory_4mb_page_count;
    uint32_t frame_buffer_preserve_memory_size_bytes;
    uint32_t padding;
    uint64_t minimum_valid_graphics_address;
    uint64_t maximum_valid_graphics_address;
};
static_assert(sizeof(BxXboxVirtualMemoryConfig) == 32, "Xbox config block is 32 bytes");
static_assert(offsetof(BxXboxVirtualMemoryConfig, flags) == 0, "flags offset");
static_assert(offsetof(BxXboxVirtualMemoryConfig, page_table_memory_4mb_page_count) == 4,
              "page_table_memory_4mb_page_count offset");
static_assert(offsetof(BxXboxVirtualMemoryConfig, frame_buffer_preserve_memory_size_bytes) == 8,
              "frame_buffer_preserve_memory_size_bytes offset");
static_assert(offsetof(BxXboxVirtualMemoryConfig, minimum_valid_graphics_address) == 16,
              "minimum_valid_graphics_address offset");
static_assert(offsetof(BxXboxVirtualMemoryConfig, maximum_valid_graphics_address) == 24,
              "maximum_valid_graphics_address offset");

constexpr uint32_t BX_D3D12_XBOX_PAGE_TABLE_DEFAULT_4MB_PAGES = 5u;
constexpr uint32_t BX_D3D12_XBOX_PAGE_TABLE_MAX_4MB_PAGES = 33u;
constexpr uint32_t BX_D3D12_XBOX_FRAME_BUFFER_PRESERVE_LEGACY = 0xFFFFFFFFu;
constexpr uint32_t BX_D3D12_XBOX_FRAME_BUFFER_PRESERVE_MAX_BYTES = 64u * 1024u * 1024u;
constexpr uint64_t BX_D3D12_XBOX_MIN_GRAPHICS_ADDRESS = 2ull * 1024ull * 1024ull * 1024ull;
/* 159.375 GiB at the default 5 pages (20 MB), so 31.875 GiB per 4 MB page. */
constexpr uint64_t BX_D3D12_XBOX_GRAPHICS_BYTES_PER_4MB_PAGE = 0x7F8000000ull;

extern "C" HRESULT bx_d3d12_configure_virtual_memory(void* config);

/* Patches the device proxy vtable so CreateHeap and CreatePlacedResource run the code in vmem.cpp. */
bool bx_vmem_install_device_hooks(void* device);

/* 1->0 Release of a device proxy, under g_section. Compare-exchanges the count from
   1 to 0 and clears every weak backing-heap pointer aimed at that proxy. Returns false
   when the count was no longer 1, so the caller retries. A true return is the only
   path into final cleanup. */
bool bx_vmem_final_release(void* device_proxy);

/* AddRefs the host ID3D12Device that created the cached backing heap, or returns null.
   The test sets BX_D3D12_REUSE_BACKING_HOST=1 so a second proxy wraps this same host
   while the cache still holds it. */
void* bx_vmem_addref_backing_host();

#endif /* BETTERXEN_D3D12_XS_VMEM_H */
