#ifndef ADVMATCH_MATCHING_H
#define ADVMATCH_MATCHING_H

#include "../trading/Order.h"

#include <cstdint>
#include <utility>

namespace advmatch
{
    struct PriceLevel;

    // MatchingEngine owns this node; a price level borrows its address.
    struct OrderNode
    {
        explicit OrderNode(BasicOrder value) : order(value)
        {}

        BasicOrder order;
        PriceLevel* level = nullptr;
        OrderNode* prev = nullptr;
        OrderNode* next = nullptr;
    };

    struct PriceLevel
    {
        explicit PriceLevel(Price value)
        : price(value)
        {}

        Price price;
        OrderNode* head = nullptr;
        OrderNode* tail = nullptr;
        Quantity totalQuantity = 0;
        std::uint32_t orderCount = 0;
    };
} // advmatch

#endif // ADVMATCH_MATCHING_H
