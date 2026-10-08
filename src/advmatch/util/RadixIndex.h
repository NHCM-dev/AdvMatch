#ifndef ADVMATCH_RADIX_INDEX_H
#define ADVMATCH_RADIX_INDEX_H

#include "BigAllocs.h"

#include <bit>
#include <cstdint>
#include <memory>
#include <utility>

namespace advmatch
{
    // Compressed binary radix index for fixed-width unsigned keys. Branch bits
    // strictly decrease along a path, so lookup and insertion visit at most 64
    // branches, independently of key count. Allocation/growth is not a bounded
    // latency operation; reserve before entering a latency-sensitive session.
    template <typename Value>
    class RadixIndex
    {
        struct Node
        {
            Node* parent = nullptr;
            int bit = -1;
        };

    public:
        struct Entry : Node
        {
            template <typename... Args>
            explicit Entry(std::uint64_t key, Args&&... args)
                : key(key), value(std::forward<Args>(args)...)
            {
            }

            const std::uint64_t key;
            Value value;
            Entry* previous = nullptr;
            Entry* next = nullptr;
        };

    private:
        struct Branch : Node
        {
            Node* children[2]{};
        };

        struct Pools
        {
            BigAllocs<Entry, 1, 64> entries;
            BigAllocs<Branch, 1, 64> branches;
        };

        std::unique_ptr<Pools> pools;
        Node* root = nullptr;
        Entry* firstEntry = nullptr;
        Entry* lastEntry = nullptr;
        std::size_t entryCount = 0;

        static unsigned direction(std::uint64_t key, int bit) noexcept
        {
            return static_cast<unsigned>((key >> bit) & 1);
        }

        static Entry* extreme(Node* node, unsigned side) noexcept
        {
            while (node && node->bit >= 0)
            {
                node = static_cast<Branch*>(node)->children[side];
            }

            return static_cast<Entry*>(node);
        }

        Entry* nearest(std::uint64_t key) const noexcept
        {
            Node* node = this->root;
            while (node && node->bit >= 0)
            {
                node = static_cast<Branch*>(node)->children[direction(key, node->bit)];
            }

            return static_cast<Entry*>(node);
        }

    public:
        RadixIndex() = default;
        RadixIndex(const RadixIndex&) = delete;
        RadixIndex& operator=(const RadixIndex&) = delete;

        void reserve(std::size_t entryCapacity)
        {
            if (entryCapacity == 0)
            {
                return;
            }

            if (!this->pools)
            {
                this->pools = std::make_unique<Pools>();
            }

            this->pools->entries.reserve(entryCapacity);
            this->pools->branches.reserve(entryCapacity - 1);
        }

        Entry* find(std::uint64_t key) noexcept
        {
            Entry* entry = this->nearest(key);
            return entry && entry->key == key ? entry : nullptr;
        }

        const Entry* find(std::uint64_t key) const noexcept
        {
            const Entry* entry = this->nearest(key);
            return entry && entry->key == key ? entry : nullptr;
        }

        template <typename... Args>
        Entry& getOrCreate(std::uint64_t key, Args&&... args)
        {
            Entry* closest = this->nearest(key);
            if (closest && closest->key == key)
            {
                return *closest;
            }

            if (!this->pools)
            {
                this->pools = std::make_unique<Pools>();
            }

            Entry* entry = this->pools->entries.emplace(key, std::forward<Args>(args)...);
            if (!closest)
            {
                this->root = entry;
                this->firstEntry = entry;
                this->lastEntry = entry;
                ++this->entryCount;
                return *entry;
            }

            Branch* branch;
            try
            {
                branch = this->pools->branches.emplace();
            }
            catch (...)
            {
                this->pools->entries.release(entry);
                throw;
            }

            const int bit = 63 - std::countl_zero(key ^ closest->key);
            Node* subtree = this->root;
            while (subtree->bit > bit)
            {
                subtree = static_cast<Branch*>(subtree)->children[direction(key, subtree->bit)];
            }

            // Every key in subtree lies on one side of the new entry. Find its
            // boundary before splicing, then ordered iteration is one pointer.
            const unsigned side = direction(key, bit);
            if (side == 0)
            {
                entry->next = extreme(subtree, 0);
                entry->previous = entry->next->previous;
            }
            else
            {
                entry->previous = extreme(subtree, 1);
                entry->next = entry->previous->next;
            }

            if (entry->previous)
            {
                entry->previous->next = entry;
            }
            else
            {
                this->firstEntry = entry;
            }

            if (entry->next)
            {
                entry->next->previous = entry;
            }
            else
            {
                this->lastEntry = entry;
            }

            branch->bit = bit;
            branch->parent = subtree->parent;
            branch->children[side] = entry;
            branch->children[side ^ 1] = subtree;
            if (branch->parent)
            {
                auto* parent = static_cast<Branch*>(branch->parent);
                parent->children[parent->children[1] == subtree] = branch;
            }
            else
            {
                this->root = branch;
            }

            subtree->parent = branch;
            entry->parent = branch;
            ++this->entryCount;
            return *entry;
        }

        // Only entries returned by this index may be erased. All other entry
        // addresses remain stable; the erased entry and its value expire.
        void erase(Entry& entry) noexcept
        {
            if (entry.previous)
            {
                entry.previous->next = entry.next;
            }
            else
            {
                this->firstEntry = entry.next;
            }

            if (entry.next)
            {
                entry.next->previous = entry.previous;
            }
            else
            {
                this->lastEntry = entry.previous;
            }

            auto* parent = static_cast<Branch*>(entry.parent);
            if (parent)
            {
                Node* sibling = parent->children[parent->children[0] == &entry];
                sibling->parent = parent->parent;
                if (parent->parent)
                {
                    auto* grandparent = static_cast<Branch*>(parent->parent);
                    grandparent->children[grandparent->children[1] == parent] = sibling;
                }
                else
                {
                    this->root = sibling;
                }

                this->pools->branches.release(parent);
            }
            else
            {
                this->root = nullptr;
            }

            this->pools->entries.release(&entry);
            --this->entryCount;
        }

        bool erase(std::uint64_t key) noexcept
        {
            Entry* entry = this->find(key);
            if (!entry)
            {
                return false;
            }

            this->erase(*entry);
            return true;
        }

        Entry* first() noexcept
        {
            return this->firstEntry;
        }

        const Entry* first() const noexcept
        {
            return this->firstEntry;
        }

        Entry* last() noexcept
        {
            return this->lastEntry;
        }

        const Entry* last() const noexcept
        {
            return this->lastEntry;
        }

        std::size_t size() const noexcept
        {
            return this->entryCount;
        }

        bool empty() const noexcept
        {
            return this->entryCount == 0;
        }
    };
}

#endif // ADVMATCH_RADIX_INDEX_H
