#ifndef ADVMATCH_TREE_PRICE_LEVELS_H
#define ADVMATCH_TREE_PRICE_LEVELS_H

#include "../../Matching.h"
#include "advmatch/util/Containers.h"

#include <functional>
#include <map>
#include <type_traits>

namespace advmatch
{
    template <bool Descending>
    class TreePriceLevels
    {
    public:
        // The tree supplies price ordering and stable PriceLevel addresses.
        // DenseMap is only a fast exact-price index into those tree nodes.
        std::map<Price, PriceLevel, std::conditional_t<Descending, std::greater<Price>, std::less<Price>>> ordered;
        DenseMap<Price, PriceLevel*> byPrice;

        PriceLevel* find(Price price) noexcept
        {
            auto it = this->byPrice.find(price);
            return it == this->byPrice.end() ? nullptr : it->second;
        }

        [[nodiscard]]
        const PriceLevel* find(Price price) const noexcept
        {
            auto it = this->byPrice.find(price);
            return it == this->byPrice.end() ? nullptr : it->second;
        }

        PriceLevel& getOrCreate(Price price)
        {
            if (PriceLevel* level = this->find(price))
            {
                return *level;
            }

            auto [it, inserted] = this->ordered.try_emplace(price, price);
            if (inserted)
            {
                try
                {
                    this->byPrice.emplace(price, &it->second);
                }
                catch (...)
                {
                    this->ordered.erase(it);
                    throw;
                }
            }

            return it->second;
        }

        void erase(Price price) noexcept
        {
            this->byPrice.erase(price);
            this->ordered.erase(price);
        }

        PriceLevel* best() noexcept
        {
            return this->ordered.empty() ? nullptr : &this->ordered.begin()->second;
        }

        [[nodiscard]]
        const PriceLevel* best() const noexcept
        {
            return this->ordered.empty() ? nullptr : &this->ordered.begin()->second;
        }

        void reserve(std::size_t count)
        {
            this->byPrice.reserve(count);
        }

        template <typename Visitor>
        void forEachWhile(Visitor&& visitor) const
        {
            for (const auto& [price, level] : this->ordered)
            {
                if (!visitor(level))
                {
                    break;
                }
            }
        }
    };
}

#endif // ADVMATCH_TREE_PRICE_LEVELS_H
