#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "bx_log.h"

namespace {

static_assert(sizeof(uintptr_t) == 8, "BetterXen Win32 shims target x86-64");

constexpr uintptr_t kPrefixMask = 0xFFFFFFFF00000000ULL;
constexpr uintptr_t kSequenceMask = 0x00000000FFFFFFFFULL;
constexpr uintptr_t kStatePrefix = 0x50534D3100000000ULL;       // "PSM1"
constexpr uintptr_t kConstrainedPrefix = 0x50534D3200000000ULL; // "PSM2"
constexpr uintptr_t kEmptySlot = 0;
constexpr uintptr_t kTombstoneSlot = ~uintptr_t{0};

struct SrwLockGuard {
    SRWLOCK& lock;
    explicit SrwLockGuard(SRWLOCK& l) : lock(l) { AcquireSRWLockExclusive(&lock); }
    ~SrwLockGuard() { ReleaseSRWLockExclusive(&lock); }
    SrwLockGuard(const SrwLockGuard&) = delete;
    SrwLockGuard& operator=(const SrwLockGuard&) = delete;
};

// One accepted registration, stored beside the token that names it.
//
// The callback and its context are kept together because the PSM contract is a
// callback plus a caller context: a caller passes `context` expecting the same
// value back as the callback's second argument if that callback ever fires.
// Dropping it would make even a future firing unable to deliver faithfully, so
// the shim owns both values for exactly as long as the token is live.
struct Entry {
    uintptr_t token; // kEmptySlot == unused, kTombstoneSlot == erased
    PVOID callback;
    PVOID context;
};

// Exception-free open-addressed hash table backed by the Win32 process heap so
// both PSM DLLs stay on -fno-exceptions and map OOM to ERROR_NOT_ENOUGH_MEMORY.
// The record lives in the table slot, so an erase drops the callback with the
// token and a reset (DllMain detach) frees them together.
struct TokenSet {
    Entry* slots = nullptr;
    size_t capacity = 0;
    size_t count = 0;
    size_t used = 0; // active entries + tombstones

    static size_t hash(uintptr_t value) {
        uint64_t x = static_cast<uint64_t>(value);
        x ^= x >> 30;
        x *= 0xbf58476d1ce4e5b9ULL;
        x ^= x >> 27;
        x *= 0x94d049bb133111ebULL;
        x ^= x >> 31;
        return static_cast<size_t>(x);
    }

    bool rehash(size_t new_capacity) {
        if (new_capacity == 0 || new_capacity > SIZE_MAX / sizeof(Entry)) {
            return false;
        }
        HANDLE heap = GetProcessHeap();
        if (!heap) {
            return false;
        }
        auto* fresh = static_cast<Entry*>(
            HeapAlloc(heap, HEAP_ZERO_MEMORY, new_capacity * sizeof(Entry)));
        if (!fresh) {
            return false;
        }
        const size_t mask = new_capacity - 1;
        for (size_t i = 0; i < capacity; ++i) {
            const Entry entry = slots[i];
            if (entry.token != kEmptySlot && entry.token != kTombstoneSlot) {
                size_t idx = hash(entry.token) & mask;
                while (fresh[idx].token != kEmptySlot) {
                    idx = (idx + 1) & mask;
                }
                fresh[idx] = entry;
            }
        }
        if (slots) {
            HeapFree(heap, 0, slots);
        }
        slots = fresh;
        capacity = new_capacity;
        used = count;
        return true;
    }

    // Inserts the token with its callback/context and returns the stored slot so
    // the caller can log back exactly what was retained. nullptr means OOM.
    Entry* insert(uintptr_t token, PVOID callback, PVOID context) {
        if (capacity == 0 || (used + 1) * 4 >= capacity * 3) {
            size_t next_capacity = 64;
            if (capacity != 0) {
                if (count * 2 >= capacity) {
                    if (capacity > (SIZE_MAX / sizeof(Entry)) / 2) {
                        return nullptr;
                    }
                    next_capacity = capacity * 2;
                } else {
                    // Compact tombstones in place without growing the table.
                    next_capacity = capacity;
                }
            }
            if (!rehash(next_capacity)) {
                return nullptr;
            }
        }
        const size_t mask = capacity - 1;
        size_t idx = hash(token) & mask;
        size_t first_tombstone = capacity;
        while (true) {
            Entry& entry = slots[idx];
            if (entry.token == token) {
                entry.callback = callback;
                entry.context = context;
                return &entry;
            }
            if (entry.token == kEmptySlot) {
                if (first_tombstone != capacity) {
                    Entry& reused = slots[first_tombstone];
                    reused.token = token;
                    reused.callback = callback;
                    reused.context = context;
                    ++count;
                    return &reused;
                }
                entry.token = token;
                entry.callback = callback;
                entry.context = context;
                ++used;
                ++count;
                return &entry;
            }
            if (entry.token == kTombstoneSlot && first_tombstone == capacity) {
                first_tombstone = idx;
            }
            idx = (idx + 1) & mask;
        }
    }

    bool erase(uintptr_t token) {
        if (capacity == 0 || count == 0 || token == kEmptySlot || token == kTombstoneSlot) {
            return false;
        }
        const size_t mask = capacity - 1;
        size_t idx = hash(token) & mask;
        for (size_t probed = 0; probed < capacity; ++probed) {
            Entry& entry = slots[idx];
            if (entry.token == kEmptySlot) {
                return false;
            }
            if (entry.token == token) {
                // Drop the stored callback and context with the token, so a
                // later firing can never reach a retired registration.
                entry.token = kTombstoneSlot;
                entry.callback = nullptr;
                entry.context = nullptr;
                --count;
                return true;
            }
            idx = (idx + 1) & mask;
        }
        return false;
    }

    void reset() {
        if (slots) {
            HANDLE heap = GetProcessHeap();
            if (heap) {
                HeapFree(heap, 0, slots);
            }
            slots = nullptr;
        }
        capacity = 0;
        count = 0;
        used = 0;
    }
};

struct Registrations {
    SRWLOCK lock;
    TokenSet active;
    uintptr_t own_prefix;
    uintptr_t foreign_prefix;
    uint64_t next_seq;
};

// Each DLL exposes one family. Disjoint tagged token spaces prevent an
// unregister call on the wrong DLL from removing another registration and let
// logs distinguish foreign-family tokens from retired or invalid ones.
Registrations g_state{SRWLOCK_INIT, {}, kStatePrefix, kConstrainedPrefix, 1};
Registrations g_constrained{SRWLOCK_INIT, {}, kConstrainedPrefix, kStatePrefix, 1};

// Accepts a registration and records the callback and context that came with it.
//
// LIMITATION: the stored callbacks are currently NEVER INVOKED. This shim has
// no source of app-state or constrained-change transitions of its own, so no
// firing policy is invented here rather than firing the callback spuriously.
// ERROR_SUCCESS therefore means "the registration was recorded (bookkeeping
// only)", not "you will be notified". The JSONL detail says `callback_unfired`
// for the same reason, so `bx-cli trace` states the limitation too.
ULONG register_notification(Registrations& registrations, PVOID callback, PVOID context,
                            PVOID* out_reg, const char* event) {
    if (!out_reg) {
        return ERROR_INVALID_PARAMETER;
    }
    *out_reg = nullptr;
    if (!callback) {
        return ERROR_INVALID_PARAMETER;
    }

    uintptr_t token = 0;
    PVOID stored_callback = nullptr;
    PVOID stored_context = nullptr;
    {
        const SrwLockGuard guard(registrations.lock);
        // Never recycle a token that a caller may still retain.
        if (registrations.next_seq > kSequenceMask) {
            return ERROR_NOT_ENOUGH_MEMORY;
        }
        token = registrations.own_prefix | static_cast<uintptr_t>(registrations.next_seq);
        const Entry* stored = registrations.active.insert(token, callback, context);
        if (!stored) {
            return ERROR_NOT_ENOUGH_MEMORY;
        }
        // Log what the table actually retained for this token, not the
        // arguments, so the record is evidence of the stored state.
        stored_callback = stored->callback;
        stored_context = stored->context;
        ++registrations.next_seq;
    }

    *out_reg = reinterpret_cast<PVOID>(token);
    char detail[192];
    std::snprintf(detail, sizeof(detail),
                  "registered_offline callback_unfired callback=0x%llx context=0x%llx",
                  static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(stored_callback)),
                  static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(stored_context)));
    bx_log_emit(BX_LOG_INFO, "win32_bridge", event, detail);
    return ERROR_SUCCESS;
}

void unregister_notification(Registrations& registrations, PVOID reg, const char* event) {
    const uintptr_t token = reinterpret_cast<uintptr_t>(reg);
    bool removed = false;
    {
        const SrwLockGuard guard(registrations.lock);
        removed = registrations.active.erase(token);
    }
    const char* detail = "unknown_token";
    if (removed) {
        detail = "unregistered";
    } else if ((token & kPrefixMask) == registrations.foreign_prefix &&
               (token & kSequenceMask) != 0) {
        detail = "foreign_token";
    }
    bx_log_emit(BX_LOG_INFO, "win32_bridge", event, detail);
}

} // namespace

extern "C" {

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_DETACH && reserved == nullptr) {
        // reset() frees the slot table, so no stored callback or context
        // outlives process teardown to be invoked against a dying image.
        {
            const SrwLockGuard guard(g_state.lock);
            g_state.active.reset();
        }
        {
            const SrwLockGuard guard(g_constrained.lock);
            g_constrained.active.reset();
        }
    }
    return TRUE;
}

ULONG WINAPI RegisterAppStateChangeNotification(PVOID callback, PVOID context, PVOID* out_reg) {
    return register_notification(g_state, callback, context, out_reg,
                                 "RegisterAppStateChangeNotification");
}

VOID WINAPI UnregisterAppStateChangeNotification(PVOID reg) {
    unregister_notification(g_state, reg, "UnregisterAppStateChangeNotification");
}

ULONG WINAPI RegisterAppConstrainedChangeNotification(PVOID callback, PVOID context, PVOID* out_reg) {
    return register_notification(g_constrained, callback, context, out_reg,
                                 "RegisterAppConstrainedChangeNotification");
}

VOID WINAPI UnregisterAppConstrainedChangeNotification(PVOID reg) {
    unregister_notification(g_constrained, reg, "UnregisterAppConstrainedChangeNotification");
}

} // extern "C"
