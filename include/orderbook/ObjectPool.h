#pragma once

#include <cstddef>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

#if defined(__SANITIZE_ADDRESS__)
#include <sanitizer/asan_interface.h>
#define ORDERBOOK_POISON(addr, size) ASAN_POISON_MEMORY_REGION(addr, size)
#define ORDERBOOK_UNPOISON(addr, size) ASAN_UNPOISON_MEMORY_REGION(addr, size)
#else
#define ORDERBOOK_POISON(addr, size) ((void)0)
#define ORDERBOOK_UNPOISON(addr, size) ((void)0)
#endif

namespace orderbook {

// Fixed-size object pool: memory is taken from the system in big chunks and
// recycled through a free list, so Create/Destroy are a few instructions with
// no malloc/free. Objects have stable addresses for their whole lifetime.
//
// The pool never returns memory to the system, which would hide use-after-free
// bugs from AddressSanitizer. Under ASan, freed slots are therefore poisoned
// by hand (all but the first pointer, which holds the free-list link).
template <typename T>
class ObjectPool {
    static_assert(std::is_trivially_destructible_v<T>,
                  "the pool frees its chunks without running destructors");

public:
    explicit ObjectPool(std::size_t chunkSize = 4096) : chunkSize_{chunkSize} {}

    ObjectPool(const ObjectPool&) = delete;
    ObjectPool& operator=(const ObjectPool&) = delete;

    ~ObjectPool() {
        for ([[maybe_unused]] auto& chunk : chunks_)
            ORDERBOOK_UNPOISON(chunk.get(), chunkSize_ * sizeof(Slot));
    }

    template <typename... Args>
    T* Create(Args&&... args) {
        if (!free_)
            Grow();
        Slot* slot = free_;
        free_ = slot->next;
        ORDERBOOK_UNPOISON(slot, sizeof(Slot));
        return std::construct_at(reinterpret_cast<T*>(slot), std::forward<Args>(args)...);
    }

    void Destroy(T* object) {
        std::destroy_at(object);
        Slot* slot = reinterpret_cast<Slot*>(object);
        slot->next = free_;
        free_ = slot;
        Poison(slot);
    }

private:
    union Slot {
        Slot* next;  // while free: link to the next free slot
        alignas(T) std::byte storage[sizeof(T)];
    };

    static void Poison([[maybe_unused]] Slot* slot) {
        if (sizeof(Slot) > sizeof(Slot*))
            ORDERBOOK_POISON(reinterpret_cast<std::byte*>(slot) + sizeof(Slot*),
                             sizeof(Slot) - sizeof(Slot*));
    }

    void Grow() {
        auto chunk = std::make_unique_for_overwrite<Slot[]>(chunkSize_);
        // Link back to front so slots are handed out in address order.
        for (std::size_t i = chunkSize_; i-- > 0;) {
            chunk[i].next = free_;
            free_ = &chunk[i];
            Poison(&chunk[i]);
        }
        chunks_.push_back(std::move(chunk));
    }

    std::size_t chunkSize_;
    std::vector<std::unique_ptr<Slot[]>> chunks_;
    Slot* free_ = nullptr;
};

}  // namespace orderbook
