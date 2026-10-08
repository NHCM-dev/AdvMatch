#ifndef ADVMATCH_RADIX_ORDER_BOOK_H
#define ADVMATCH_RADIX_ORDER_BOOK_H

#include "PriceTimeOrderBook.h"
#include "RadixPriceLevels.h"

namespace advmatch
{
    // uint64 price domain: at most 64 branching bits, no configured price
    // range, no array proportional to price, and no ordered-tree allocation.
    using RadixOrderBook = PriceTimeOrderBook<RadixPriceLevels<true>, RadixPriceLevels<false>>;
}

#endif // ADVMATCH_RADIX_ORDER_BOOK_H
