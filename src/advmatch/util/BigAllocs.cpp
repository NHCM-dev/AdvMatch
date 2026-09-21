#ifndef ADVMATCH_BIGALLOCS_IMPL
#define ADVMATCH_BIGALLOCS_IMPL

#include "BigAllocs.h"

#include <bit>
#include <limits>
#include <new>
#include <utility>

namespace advmatch
{
    template <typename T, std::size_t initialBlocks, std::size_t blockSize>
    BigAllocs<T, initialBlocks, blockSize>::Block::Block()
        : slots(new Slot[blockSize])
    {
    }

    template <typename T, std::size_t initialBlocks, std::size_t blockSize>
    bool BigAllocs<T, initialBlocks, blockSize>::Block::contains(std::size_t slotIndex) const noexcept
    {
        const std::size_t wordIndex = slotIndex / occupiedWordBits;
        const std::size_t bitIndex = slotIndex % occupiedWordBits;
        return static_cast<bool>(this->occupied[wordIndex] & (std::uint64_t{1} << bitIndex));
    }

    template <typename T, std::size_t initialBlocks, std::size_t blockSize>
    void BigAllocs<T, initialBlocks, blockSize>::Block::markOccupied(std::size_t slotIndex) noexcept
    {
        const std::size_t wordIndex = slotIndex / occupiedWordBits;
        const std::size_t bitIndex = slotIndex % occupiedWordBits;
        this->occupied[wordIndex] |= std::uint64_t{1} << bitIndex;
        ++this->objectCount;
    }

    template <typename T, std::size_t initialBlocks, std::size_t blockSize>
    void BigAllocs<T, initialBlocks, blockSize>::Block::markFree(std::size_t slotIndex) noexcept
    {
        const std::size_t wordIndex = slotIndex / occupiedWordBits;
        const std::size_t bitIndex = slotIndex % occupiedWordBits;
        this->occupied[wordIndex] &= ~(std::uint64_t{1} << bitIndex);
        --this->objectCount;
    }

    template <typename T, std::size_t initialBlocks, std::size_t blockSize>
    void BigAllocs<T, initialBlocks, blockSize>::Block::reset() noexcept
    {
        this->occupied.fill(0);
        this->nextUnused = 0;
        this->objectCount = 0;
    }

    template <typename T, std::size_t initialBlocks, std::size_t blockSize>
    BigAllocs<T, initialBlocks, blockSize>::BigAllocs()
    {
        this->blocks.reserve(initialBlocks);
        for (std::size_t index = 0; index < initialBlocks; ++index)
        {
            this->appendBlock();
        }
    }

    template <typename T, std::size_t initialBlocks, std::size_t blockSize>
    BigAllocs<T, initialBlocks, blockSize>::~BigAllocs()
    {
        this->clear();
    }

    template <typename T, std::size_t initialBlocks, std::size_t blockSize>
    BigAllocs<T, initialBlocks, blockSize>::Block& BigAllocs<T, initialBlocks, blockSize>::appendBlock()
    {
        this->blocks.emplace_back(std::make_unique<Block>());
        return *this->blocks.back();
    }

    template <typename T, std::size_t initialBlocks, std::size_t blockSize>
    T* BigAllocs<T, initialBlocks, blockSize>::objectFromSlot(Slot* slot) noexcept
    {
        return std::launder(reinterpret_cast<T*>(slot->payload.object));
    }

    template <typename T, std::size_t initialBlocks, std::size_t blockSize>
    BigAllocs<T, initialBlocks, blockSize>::Slot* BigAllocs<T, initialBlocks, blockSize>::slotFromObject(T* object) noexcept
    {
        std::byte* address = reinterpret_cast<std::byte*>(object);
        return reinterpret_cast<Slot*>(address - offsetof(Slot, payload));
    }

    template <typename T, std::size_t initialBlocks, std::size_t blockSize>
    void BigAllocs<T, initialBlocks, blockSize>::pushFree(Slot* slot) noexcept
    {
        slot->payload.nextFree = this->firstFree;
        this->firstFree = slot;
    }

    template <typename T, std::size_t initialBlocks, std::size_t blockSize>
    template <typename... Args>
    T* BigAllocs<T, initialBlocks, blockSize>::emplace(Args&&... args)
    {
        Slot* slot;
        Block* block;
        std::size_t slotIndex;

        if (this->firstFree)
        {
            slot = this->firstFree;
            this->firstFree = slot->payload.nextFree;
            block = slot->owner;
            slotIndex = static_cast<std::size_t>(slot - block->slots.get());
        }
        else
        {
            Block* activeBlock = this->blocks[this->activeBlockIndex].get();
            if (activeBlock->nextUnused == blockSize)
            {
                ++this->activeBlockIndex;
                if (this->activeBlockIndex == this->blocks.size())
                {
                    this->appendBlock();
                }

                activeBlock = this->blocks[this->activeBlockIndex].get();
            }

            block = activeBlock;
            slotIndex = block->nextUnused++;
            slot = block->slots.get() + slotIndex;
            slot->owner = block;
        }

        T* object = objectFromSlot(slot);
        try
        {
            std::construct_at(object, std::forward<Args>(args)...);
        }
        catch (...)
        {
            this->pushFree(slot);
            throw;
        }

        block->markOccupied(slotIndex);
        ++this->objectCount;
        return object;
    }

    template <typename T, std::size_t initialBlocks, std::size_t blockSize>
    bool BigAllocs<T, initialBlocks, blockSize>::release(T* object)
    {
        if (!object)
        {
            return false;
        }

        Slot* slot = slotFromObject(object);
        Block* block = slot->owner;
        const std::size_t slotIndex = static_cast<std::size_t>(slot - block->slots.get());
        if (slotIndex >= block->nextUnused || !block->contains(slotIndex))
        {
            return false;
        }

        std::destroy_at(object);
        block->markFree(slotIndex);
        this->pushFree(slot);
        --this->objectCount;
        return true;
    }

    template <typename T, std::size_t initialBlocks, std::size_t blockSize>
    std::size_t BigAllocs<T, initialBlocks, blockSize>::size() const noexcept
    {
        return this->objectCount;
    }

    template <typename T, std::size_t initialBlocks, std::size_t blockSize>
    bool BigAllocs<T, initialBlocks, blockSize>::empty() const noexcept
    {
        return this->objectCount == 0;
    }

    template <typename T, std::size_t initialBlocks, std::size_t blockSize>
    std::size_t BigAllocs<T, initialBlocks, blockSize>::capacity() const noexcept
    {
        if (this->blocks.size() > std::numeric_limits<std::size_t>::max() / blockSize)
        {
            return std::numeric_limits<std::size_t>::max();
        }

        return this->blocks.size() * blockSize;
    }

    template <typename T, std::size_t initialBlocks, std::size_t blockSize>
    void BigAllocs<T, initialBlocks, blockSize>::reserve(std::size_t count)
    {
        if (count <= this->capacity())
        {
            return;
        }

        const std::size_t requiredBlocks = ((count - 1) / blockSize) + 1;
        this->blocks.reserve(requiredBlocks);
        while (this->blocks.size() < requiredBlocks)
        {
            this->appendBlock();
        }
    }

    template <typename T, std::size_t initialBlocks, std::size_t blockSize>
    void BigAllocs<T, initialBlocks, blockSize>::clear() noexcept
    {
        for (const std::unique_ptr<Block>& blockPointer : this->blocks)
        {
            Block& block = *blockPointer;
            if (block.objectCount != 0)
            {
                for (std::size_t wordIndex = 0; wordIndex < occupiedWordCount; ++wordIndex)
                {
                    std::uint64_t occupied = block.occupied[wordIndex];
                    while (occupied != 0)
                    {
                        const std::size_t bitIndex = static_cast<std::size_t>(std::countr_zero(occupied));
                        const std::size_t slotIndex = (wordIndex * occupiedWordBits) + bitIndex;
                        std::destroy_at(objectFromSlot(block.slots.get() + slotIndex));
                        occupied &= occupied - 1;
                    }
                }
            }

            block.reset();
        }

        this->firstFree = nullptr;
        this->activeBlockIndex = 0;
        this->objectCount = 0;
    }
}

#endif // ADVMATCH_BIGALLOCS_IMPL
