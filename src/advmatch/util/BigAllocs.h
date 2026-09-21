#ifndef ADVMATCH_BIGALLOCS_H
#define ADVMATCH_BIGALLOCS_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "Containers.h"

namespace advmatch
{
    // Single-thread object pool for stable-address objects. emplace() and
    // release() are O(1) on the hot path; growth is amortized O(1).
    //
    // A pointer passed to release() must have been returned by this pool.
    // The pool detects a repeated release, but arbitrary foreign pointers are
    // outside the contract so the valid release path does not need a lookup.
    template <typename T, std::size_t initialBlocks = 1, std::size_t blockSize = 4096>
    class BigAllocs
    {
        static_assert(initialBlocks > 0, "initialBlocks must be greater than 0");
        static_assert(blockSize > 0, "blockSize must be greater than 0");

        static constexpr std::size_t occupiedWordBits = 64;
        static constexpr std::size_t occupiedWordCount = (blockSize + occupiedWordBits - 1) / occupiedWordBits;

        struct Block;
        struct Slot;

        union SlotPayload
        {
            alignas(T) std::byte object[sizeof(T)];
            Slot* nextFree;
        };

        struct Slot
        {
            Block* owner;
            SlotPayload payload;
        };

        struct Block
        {
            std::unique_ptr<Slot[]> slots;
            std::array<std::uint64_t, occupiedWordCount> occupied{};
            std::size_t nextUnused = 0;
            std::size_t objectCount = 0;

            Block();

            [[nodiscard]]
            bool contains(std::size_t slotIndex) const noexcept;

            void markOccupied(std::size_t slotIndex) noexcept;
            void markFree(std::size_t slotIndex) noexcept;
            void reset() noexcept;
        };

        SmallVector<std::unique_ptr<Block>, initialBlocks> blocks;
        Slot* firstFree = nullptr;
        std::size_t activeBlockIndex = 0;
        std::size_t objectCount = 0;

        Block& appendBlock();

        [[nodiscard]]
        static T* objectFromSlot(Slot* slot) noexcept;

        [[nodiscard]]
        static Slot* slotFromObject(T* object) noexcept;

        void pushFree(Slot* slot) noexcept;

    public:
        BigAllocs();
        ~BigAllocs();

        BigAllocs(const BigAllocs&) = delete;
        BigAllocs& operator=(const BigAllocs&) = delete;
        BigAllocs(BigAllocs&&) = delete;
        BigAllocs& operator=(BigAllocs&&) = delete;

        template <typename... Args>
        T* emplace(Args&&... args);

        bool release(T* object);

        [[nodiscard]]
        std::size_t size() const noexcept;

        [[nodiscard]]
        bool empty() const noexcept;

        [[nodiscard]]
        std::size_t capacity() const noexcept;

        void reserve(std::size_t count);
        void clear() noexcept;
    };
}

#include "BigAllocs.cpp"

#endif // ADVMATCH_BIGALLOCS_H
