#include <windows.h>

#include <cstdint>
#include <limits>
#include <new>

#include "bx_log.h"
#include "bx_xmem_internal.h"
#include "xmem_accounting.h"

namespace {
constexpr SIZE_T small_page = 64 * 1024;

struct PageBlock;

struct Page {
    PageBlock* block;
    HANDLE section;
    bool releasing;
};

struct PageBlock {
    ULONG_PTR first_tag;
    SIZE_T count;
    SIZE_T size;
    SIZE_T live;
    Page* entries;
    PageBlock* next;
};

struct View {
    Page* page;
    BYTE* address;
    View* next;
    // Mapped into an XMEM_MAPPABLE reservation: unmapping restores the placeholder so the
    // range stays reserved and can be mapped again.
    bool preserve;
};

// An XMemVirtualAlloc(MEM_RESERVE, XMEM_MAPPABLE) range, held as a placeholder.
struct Reservation {
    uintptr_t begin;
    uintptr_t end;
    SIZE_T page;
    DWORD protect;
    Reservation* next;
};

SRWLOCK memory_lock = SRWLOCK_INIT;
PageBlock* blocks = nullptr;
View* views = nullptr;
Reservation* reservations = nullptr;
ULONG_PTR next_tag = 1;

// XMemAllocatePhysicalPages flags (authorized GDK documentation): 2 MB clusters, and the four
// memory-bandwidth values, which only the Series X Advanced memory mode uses.
constexpr uint64_t xmem_2mb_clusters = 0x20000000;
constexpr uint64_t xmem_bandwidth_bits = 0x380;

struct Lock {
    Lock() { AcquireSRWLockExclusive(&memory_lock); }
    ~Lock() { ReleaseSRWLockExclusive(&memory_lock); }
};

using ReservePlaceholder = PVOID (WINAPI*)(HANDLE, PVOID, SIZE_T, ULONG, ULONG,
                                           MEM_EXTENDED_PARAMETER*, ULONG);
using ReplacePlaceholder = PVOID (WINAPI*)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG,
                                           ULONG, MEM_EXTENDED_PARAMETER*, ULONG);
using UnmapPreserving = BOOL (WINAPI*)(HANDLE, PVOID, ULONG);

struct PlaceholderApi {
    HMODULE kernel = GetModuleHandleW(L"kernelbase.dll");
    ReservePlaceholder reserve = resolve<ReservePlaceholder>("VirtualAlloc2");
    ReplacePlaceholder replace = resolve<ReplacePlaceholder>("MapViewOfFile3");
    UnmapPreserving unmap = resolve<UnmapPreserving>("UnmapViewOfFile2");

    template<class Function>
    Function resolve(const char* name) const {
        return reinterpret_cast<Function>(reinterpret_cast<void*>(GetProcAddress(kernel, name)));
    }
};

const PlaceholderApi& placeholder_api() {
    static const PlaceholderApi api;
    return api;
}

// The reservation holding all of [begin, end), or nullptr.
Reservation* find_reservation(uintptr_t begin, uintptr_t end) {
    for (Reservation* r = reservations; r; r = r->next) {
        if (begin >= r->begin && end <= r->end && begin < end) return r;
    }
    return nullptr;
}

Page* find_page(ULONG_PTR tag) {
    for (PageBlock* block = blocks; block; block = block->next) {
        if (tag >= block->first_tag && tag - block->first_tag < block->count) {
            Page* page = &block->entries[tag - block->first_tag];
            return page->section ? page : nullptr;
        }
    }
    return nullptr;
}

ULONG_PTR page_tag(const Page* page) {
    return page->block->first_tag + (page - page->block->entries);
}

void delete_block(PageBlock* block) {
    for (SIZE_T i = 0; i < block->count; ++i) {
        if (block->entries[i].section) CloseHandle(block->entries[i].section);
    }
    delete[] block->entries;
    delete block;
}

void destroy_views(View* first) {
    while (first) {
        View* next = first->next;
        delete first;
        first = next;
    }
}

bool valid_span(PVOID base, SIZE_T count, SIZE_T size) {
    const auto address = reinterpret_cast<uintptr_t>(base);
    return size && count && address % size == 0
        && count <= (std::numeric_limits<uintptr_t>::max() - address) / size;
}

bool remove_view(View** link) {
    View* view = *link;
    const bool unmapped = view->preserve
        ? placeholder_api().unmap && placeholder_api().unmap(GetCurrentProcess(), view->address,
                                                             MEM_PRESERVE_PLACEHOLDER)
        : UnmapViewOfFile(view->address) != 0;
    if (!unmapped) return false;
    *link = view->next;
    delete view;
    return true;
}

// Require every page slot in [base, base + count * size) to be actively mapped
// with matching page size before unmapping; empty or partial spans fail without
// modifying any existing views.
PVOID unmap_span(BYTE* base, SIZE_T count, SIZE_T size) {
    if (!base) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    const auto begin = reinterpret_cast<uintptr_t>(base);
    const auto end = begin + count * size;

    SIZE_T matched = 0;
    for (const View* view = views; view; view = view->next) {
        const auto address = reinterpret_cast<uintptr_t>(view->address);
        if (address >= begin && address < end && view->page->block->size == size) {
            ++matched;
        }
    }
    if (matched != count) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }

    for (View** link = &views; *link;) {
        View* view = *link;
        const auto address = reinterpret_cast<uintptr_t>(view->address);
        if (address >= begin && address < end && view->page->block->size == size) {
            if (!remove_view(link)) return nullptr;
        } else {
            link = &view->next;
        }
    }
    return base;
}

// Builds one unlinked View per tag, in order, after checking that every tag names a live page.
bool build_views(const ULONG_PTR* tags, SIZE_T count, bool preserve, View** out) {
    View* first = nullptr;
    View** tail = &first;
    for (SIZE_T i = 0; i < count; ++i) {
        Page* page = find_page(tags[i]);
        if (!page) {
            destroy_views(first);
            SetLastError(ERROR_INVALID_PARAMETER);
            return false;
        }
        auto* view = new (std::nothrow) View{page, nullptr, nullptr, preserve};
        if (!view) {
            destroy_views(first);
            SetLastError(ERROR_NOT_ENOUGH_MEMORY);
            return false;
        }
        *tail = view;
        tail = &view->next;
    }
    *out = first;
    return true;
}

// Removes every view that starts in [begin, end). Views in a reservation keep its placeholder.
bool unmap_range(uintptr_t begin, uintptr_t end) {
    for (View** link = &views; *link;) {
        const auto address = reinterpret_cast<uintptr_t>((*link)->address);
        if (address >= begin && address < end) {
            if (!remove_view(link)) return false;
        } else {
            link = &(*link)->next;
        }
    }
    return true;
}

// Maps `view`'s page at `address` inside a reservation: splits the 64 KB slot out of the
// placeholder (a slot that is already its own placeholder needs no split) and replaces it.
bool map_into_placeholder(View* view, BYTE* address, DWORD protect) {
    const PlaceholderApi& api = placeholder_api();
    VirtualFree(address, small_page, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
    if (!api.replace(view->page->section, GetCurrentProcess(), address, 0, small_page,
                     MEM_REPLACE_PLACEHOLDER, protect, nullptr, 0)) {
        return false;
    }
    view->address = address;
    return true;
}

void publish(View* pending) {
    while (pending) {
        View* next = pending->next;
        pending->next = views;
        views = pending;
        pending = next;
    }
}

// XMemMapPhysicalPages into an XMEM_MAPPABLE reservation. Pages already mapped in the range are
// unmapped first; a NULL `tags` only unmaps. Duplicate tags alias one page at several offsets.
PVOID map_reserved(Reservation* reservation, BYTE* base, SIZE_T count, const ULONG_PTR* tags) {
    if (reservation->page != small_page) {
        BX_STUB_LOG("xmem.dll", "XMemMapPhysicalPages(MEM_2MB_PAGES)");
        SetLastError(ERROR_NOT_SUPPORTED);
        return nullptr;
    }
    const auto begin = reinterpret_cast<uintptr_t>(base);
    View* pending = nullptr;
    if (tags && !build_views(tags, count, true, &pending)) return nullptr;
    if (!unmap_range(begin, begin + count * small_page)) {
        const DWORD error = GetLastError();
        destroy_views(pending);
        SetLastError(error);
        return nullptr;
    }
    BYTE* cursor = base;
    for (View* view = pending; view; view = view->next, cursor += small_page) {
        if (!map_into_placeholder(view, cursor, reservation->protect)) {
            const DWORD error = GetLastError();
            for (View* done = pending; done != view; done = done->next) {
                placeholder_api().unmap(GetCurrentProcess(), done->address, MEM_PRESERVE_PLACEHOLDER);
            }
            destroy_views(pending);
            SetLastError(error);
            return nullptr;
        }
    }
    publish(pending);
    return base;
}

// BetterXen extension, used by synthetic fixtures: with no address, reserve a fresh
// placeholder of `count` pages and map `tags` into it. Unmapping such a span frees it.
PVOID map_unreserved(SIZE_T count, const ULONG_PTR* tags) {
    const PlaceholderApi& api = placeholder_api();
    View* pending = nullptr;
    if (!build_views(tags, count, false, &pending)) return nullptr;

    MEM_ADDRESS_REQUIREMENTS requirements{};
    requirements.Alignment = small_page;
    MEM_EXTENDED_PARAMETER alignment{};
    alignment.Type = MemExtendedParameterAddressRequirements;
    alignment.Pointer = &requirements;
    auto* base = static_cast<BYTE*>(api.reserve(nullptr, nullptr, count * small_page,
        MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, &alignment, 1));
    if (!base) {
        const DWORD error = GetLastError();
        destroy_views(pending);
        SetLastError(error);
        return nullptr;
    }

    BYTE* cursor = base;
    SIZE_T remaining = count;
    View* view = pending;
    for (; view; view = view->next, cursor += small_page, --remaining) {
        const bool split = remaining > 1;
        if (split && !VirtualFree(cursor, small_page, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) break;
        if (!api.replace(view->page->section, GetCurrentProcess(), cursor, 0, small_page,
                         MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0)) {
            const DWORD error = GetLastError();
            if (split) VirtualFree(cursor + small_page, 0, MEM_RELEASE);
            SetLastError(error);
            break;
        }
        view->address = cursor;
    }

    if (view) {
        const DWORD error = GetLastError();
        VirtualFree(cursor, 0, MEM_RELEASE);
        for (View* completed = pending; completed != view; completed = completed->next) {
            UnmapViewOfFile(completed->address);
        }
        destroy_views(pending);
        SetLastError(error);
        return nullptr;
    }
    publish(pending);
    return base;
}

bool have_placeholder_api() {
    const PlaceholderApi& api = placeholder_api();
    if (api.reserve && api.replace && api.unmap) return true;
    BX_STUB_LOG("xmem.dll", "MapViewOfFile3");
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return false;
}
} // namespace

namespace bx_xmem {

PVOID reserve_placeholder(PVOID address, SIZE_T size, uintptr_t highest) {
    const PlaceholderApi& api = placeholder_api();
    if (!have_placeholder_api()) return nullptr;
    const auto requested = reinterpret_cast<uintptr_t>(address);
    const uintptr_t begin = requested - requested % small_page;
    if (!size || size > std::numeric_limits<uintptr_t>::max() - requested - small_page) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    const uintptr_t end = (requested + size + small_page - 1) / small_page * small_page;
    MEM_ADDRESS_REQUIREMENTS requirements{};
    requirements.Alignment = small_page;
    requirements.HighestEndingAddress = highest ? reinterpret_cast<PVOID>(highest) : nullptr;
    MEM_EXTENDED_PARAMETER parameter{};
    parameter.Type = MemExtendedParameterAddressRequirements;
    parameter.Pointer = &requirements;
    return api.reserve(GetCurrentProcess(), begin ? reinterpret_cast<PVOID>(begin) : nullptr, end - begin,
                       MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, begin ? nullptr : &parameter,
                       begin ? 0 : 1);
}

PVOID reserve_mappable(PVOID address, SIZE_T size, SIZE_T page, DWORD protect, bool adopt, uintptr_t highest) {
    const PlaceholderApi& api = placeholder_api();
    const auto requested = reinterpret_cast<uintptr_t>(address);
    if (!have_placeholder_api()) return nullptr;
    if (adopt && (!address || requested % page != 0 || size % page != 0)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    if (!size || size > std::numeric_limits<uintptr_t>::max() - requested - page) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    // A reservation's base rounds down to its page size and its end rounds up.
    const uintptr_t begin = requested - requested % page;
    const uintptr_t end = (requested + size + page - 1) / page * page;

    MEM_ADDRESS_REQUIREMENTS requirements{};
    requirements.Alignment = page;
    requirements.HighestEndingAddress = highest ? reinterpret_cast<PVOID>(highest) : nullptr;
    MEM_EXTENDED_PARAMETER alignment{};
    alignment.Type = MemExtendedParameterAddressRequirements;
    alignment.Pointer = &requirements;
    auto* reservation = new (std::nothrow) Reservation{};
    if (!reservation) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }
    PVOID base = nullptr;
    if (adopt) {
        // MEM_REPLACE_PLACEHOLDER: the title split this range out of its own placeholder; it
        // stays as it is and becomes mappable.
        MEMORY_BASIC_INFORMATION region{};
        if (!VirtualQuery(address, &region, sizeof(region)) || region.State != MEM_RESERVE
            || reinterpret_cast<uintptr_t>(region.BaseAddress) != requested || region.RegionSize < size) {
            delete reservation;
            SetLastError(ERROR_INVALID_ADDRESS);
            return nullptr;
        }
        base = address;
    } else {
        base = api.reserve(GetCurrentProcess(), begin ? reinterpret_cast<PVOID>(begin) : nullptr,
                           end - begin, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS,
                           begin ? nullptr : &alignment, begin ? 0 : 1);
    }
    if (!base) {
        delete reservation;
        return nullptr;
    }

    Lock lock;
    const auto b = reinterpret_cast<uintptr_t>(base);
    // A range the title released with VirtualFree may come back: drop stale records over it.
    for (Reservation** link = &reservations; *link;) {
        if ((*link)->begin < b + (end - begin) && b < (*link)->end) {
            Reservation* stale = *link;
            *link = stale->next;
            delete stale;
        } else {
            link = &(*link)->next;
        }
    }
    *reservation = {b, b + (end - begin), page, protect, reservations};
    reservations = reservation;
    return base;
}

} // namespace bx_xmem

// Authorized GDK documentation: XMemAllocatePhysicalPages(XMemFlags, NumberOfPages,
// PageArray), in 64 KB pages. XMEM_2MB_CLUSTERS is not implemented.
extern "C" BOOL WINAPI XMemAllocatePhysicalPages(uint64_t flags, PULONG_PTR num_pages,
                                               PULONG_PTR page_array) {
    if ((flags & ~(xmem_2mb_clusters | xmem_bandwidth_bits)) != 0 || !num_pages || !page_array
        || !valid_span(nullptr, *num_pages, small_page)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (flags & xmem_2mb_clusters) {
        BX_STUB_LOG("xmem.dll", "XMemAllocatePhysicalPages(XMEM_2MB_CLUSTERS)");
        SetLastError(ERROR_NOT_SUPPORTED);
        return FALSE;
    }
    const SIZE_T size = small_page;

    Lock lock;
    const SIZE_T count = *num_pages;
    if (count > std::numeric_limits<ULONG_PTR>::max() - next_tag
        || count > std::numeric_limits<SIZE_T>::max() / sizeof(Page)
        || count > std::numeric_limits<uint64_t>::max() / size) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }
    const uint64_t total_bytes = static_cast<uint64_t>(count) * size;
    if (!bx_xmem::try_reserve_physical(total_bytes)) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }

    auto* entries = new (std::nothrow) Page[count]{};
    if (!entries) {
        bx_xmem::release_physical(total_bytes);
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }
    auto* block = new (std::nothrow) PageBlock{next_tag, count, size, count, entries, blocks};
    if (!block) {
        delete[] entries;
        bx_xmem::release_physical(total_bytes);
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }

    for (SIZE_T i = 0; i < count; ++i) {
        HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                            0, static_cast<DWORD>(size), nullptr);
        if (!section) {
            const DWORD error = GetLastError();
            delete_block(block);
            bx_xmem::release_physical(total_bytes);
            SetLastError(error);
            return FALSE;
        }
        entries[i] = {block, section, false};
    }

    blocks = block;
    for (SIZE_T i = 0; i < count; ++i) page_array[i] = next_tag + i;
    next_tag += count;
    return TRUE;
}

// XMemFreePhysicalPages(NumberOfPages, PageArray). Unknown or duplicate tags in `page_array`
// fail with ERROR_INVALID_PARAMETER without changing `page_array` or live views.
extern "C" BOOL WINAPI XMemFreePhysicalPages(ULONG_PTR count, PULONG_PTR page_array) {
    if (!page_array || !valid_span(nullptr, count, small_page)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    Lock lock;
    SIZE_T validated = 0;
    for (; validated < count; ++validated) {
        Page* page = find_page(page_array[validated]);
        if (!page || page->releasing) break;
        page->releasing = true;
    }
    if (validated != count) {
        for (SIZE_T i = 0; i < validated; ++i) find_page(page_array[i])->releasing = false;
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    for (View** link = &views; *link;) {
        if ((*link)->page->releasing) {
            if (!remove_view(link)) {
                const DWORD error = GetLastError();
                for (SIZE_T i = 0; i < count; ++i) find_page(page_array[i])->releasing = false;
                SetLastError(error);
                return FALSE;
            }
        } else {
            link = &(*link)->next;
        }
    }

    for (SIZE_T i = 0; i < count; ++i) {
        Page* page = find_page(page_array[i]);
        CloseHandle(page->section);
        page->section = nullptr;
        --page->block->live;
        page_array[i] = 0;
    }
    for (PageBlock** link = &blocks; *link;) {
        PageBlock* block = *link;
        if (block->live) {
            link = &block->next;
        } else {
            *link = block->next;
            delete_block(block);
        }
    }
    bx_xmem::release_physical(static_cast<uint64_t>(count) * small_page);
    return TRUE;
}

// XMemMapPhysicalPages(VirtualAddress, NumberOfPages, PageArray). The address must lie in an
// XMemVirtualAlloc(MEM_RESERVE | MEM_64K_PAGES, XMEM_MAPPABLE) reservation. A NULL address is a
// BetterXen extension (see map_unreserved); a span mapped that way is unmapped as a whole.
extern "C" PVOID WINAPI XMemMapPhysicalPages(PVOID address, ULONG_PTR count, PULONG_PTR page_array) {
    if (!valid_span(address, count, small_page)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    if (!have_placeholder_api()) return nullptr;
    Lock lock;
    auto* base = static_cast<BYTE*>(address);
    const auto begin = reinterpret_cast<uintptr_t>(address);
    if (address) {
        if (Reservation* reservation = find_reservation(begin, begin + count * small_page)) {
            return map_reserved(reservation, base, count, page_array);
        }
        if (!page_array) return unmap_span(base, count, small_page);
        SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    if (!page_array) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    return map_unreserved(count, page_array);
}

// XMemMapPhysicalPagesScatter(VirtualAddressArray, NumberOfPages, PageArray): parallel arrays of
// 64 KB slots in XMEM_MAPPABLE reservations and page tags. A NULL `page_array`, or a 0 tag,
// unmaps the slot. All entries are checked before any is changed.
extern "C" BOOL WINAPI XMemMapPhysicalPagesScatter(PVOID* addresses, ULONG_PTR count,
                                                  PULONG_PTR page_array) {
    if (!addresses || !count) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (!have_placeholder_api()) return FALSE;
    Lock lock;
    for (ULONG_PTR i = 0; i < count; ++i) {
        const auto slot = reinterpret_cast<uintptr_t>(addresses[i]);
        Reservation* reservation = slot % small_page == 0 ? find_reservation(slot, slot + small_page) : nullptr;
        if (!reservation || (page_array && page_array[i] && !find_page(page_array[i]))) {
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }
        if (reservation->page != small_page) {
            BX_STUB_LOG("xmem.dll", "XMemMapPhysicalPagesScatter(MEM_2MB_PAGES)");
            SetLastError(ERROR_NOT_SUPPORTED);
            return FALSE;
        }
    }
    for (ULONG_PTR i = 0; i < count; ++i) {
        auto* slot = static_cast<BYTE*>(addresses[i]);
        const auto begin = reinterpret_cast<uintptr_t>(slot);
        if (!unmap_range(begin, begin + small_page)) return FALSE;
        if (!page_array || !page_array[i]) continue;
        View* view = nullptr;
        if (!build_views(&page_array[i], 1, true, &view)) return FALSE;
        if (!map_into_placeholder(view, slot,
                                  find_reservation(begin, begin + small_page)->protect)) {
            const DWORD error = GetLastError();
            destroy_views(view);
            SetLastError(error);
            return FALSE;
        }
        publish(view);
    }
    return TRUE;
}

extern "C" int bx_xmem_query_region(uint64_t va, bx_xmem_region_info_t* out_info) {
    if (!out_info) return 0;

    Lock lock;
    for (const View* view = views; view; view = view->next) {
        const auto base = reinterpret_cast<uintptr_t>(view->address);
        const SIZE_T size = view->page->block->size;
        if (va >= base && va - base < size) {
            MEMORY_BASIC_INFORMATION region{};
            if (!VirtualQuery(view->address, &region, sizeof(region))
                || region.AllocationBase != view->address || region.Type != MEM_MAPPED) {
                return 0;
            }
            *out_info = {base, size, size, region.Protect, region.Type, page_tag(view->page)};
            return 1;
        }
    }
    return 0;
}
