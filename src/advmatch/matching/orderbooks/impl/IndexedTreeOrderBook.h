#ifndef ADVMATCH_INDEXED_TREE_ORDER_BOOK_H
#define ADVMATCH_INDEXED_TREE_ORDER_BOOK_H

#include "PriceTimeOrderBook.h"
#include "TreePriceLevels.h"

namespace advmatch
{
    using IndexedTreeOrderBook = PriceTimeOrderBook<TreePriceLevels<true>, TreePriceLevels<false>>;
}

#endif // ADVMATCH_INDEXED_TREE_ORDER_BOOK_H
