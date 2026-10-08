#ifndef ADVMATCH_RADIX_PRICE_LEVELS_H
#define ADVMATCH_RADIX_PRICE_LEVELS_H

#include "../../Matching.h"
#include "advmatch/util/RadixIndex.h"

namespace advmatch
{
    template <bool Descending>
    class RadixPriceLevels
    {
        RadixIndex<PriceLevel> prices;

    public:
        void reserve(std::size_t count)
        {
            this->prices.reserve(count);
        }

        PriceLevel* find(Price price) noexcept
        {
            auto* entry = this->prices.find(price);
            return entry ? &entry->value : nullptr;
        }

        const PriceLevel* find(Price price) const noexcept
        {
            const auto* entry = this->prices.find(price);
            return entry ? &entry->value : nullptr;
        }

        PriceLevel& getOrCreate(Price price)
        {
            return this->prices.getOrCreate(price, price).value;
        }

        void erase(Price price) noexcept
        {
            this->prices.erase(price);
        }

        PriceLevel* best() noexcept
        {
            auto* entry = Descending ? this->prices.last() : this->prices.first();
            return entry ? &entry->value : nullptr;
        }

        const PriceLevel* best() const noexcept
        {
            const auto* entry = Descending ? this->prices.last() : this->prices.first();
            return entry ? &entry->value : nullptr;
        }

        template <typename Visitor>
        void forEachWhile(Visitor&& visitor) const
        {
            const auto* entry = Descending ? this->prices.last() : this->prices.first();
            while (entry)
            {
                if (!visitor(entry->value))
                {
                    break;
                }

                entry = Descending ? entry->previous : entry->next;
            }
        }
    };
}

#endif // ADVMATCH_RADIX_PRICE_LEVELS_H
