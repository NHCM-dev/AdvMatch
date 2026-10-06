#ifndef ADVMATCH_INDEXED_TREE_ORDER_BOOK_H
#define ADVMATCH_INDEXED_TREE_ORDER_BOOK_H

#include "../../Matching.h"
#include "../OrderBook.h"
#include "../../../util/Containers.h"

#include <functional>
#include <map>

namespace advmatch
{
    class IndexedTreeOrderBook final : public OrderBook
    {
        template <typename Compare>
        struct Levels
        {
            // The tree supplies price ordering and stable PriceLevel addresses.
            // DenseMap is only a fast exact-price index into those tree nodes.
            std::map<Price, PriceLevel, Compare> ordered;
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
        };

        SymbolId bookSymbolId;
        Levels<std::greater<>> bids;
        Levels<std::less<>> asks;

        static std::optional<Quote> quote(const PriceLevel* level) noexcept;
        static bool crosses(const BasicOrder& taker, Price makerPrice) noexcept;
        [[nodiscard]]
        bool canFillAll(const BasicOrder& order) const noexcept;
        void match(OrderNode& taker, Timestamp timestamp, TradeBuffer& trades);
        void append(OrderNode& node);
        void unlink(OrderNode& node, Quantity remaining) noexcept;

    public:
        explicit IndexedTreeOrderBook(SymbolId symbolId) : bookSymbolId(symbolId)
        {
        }

        [[nodiscard]]
        SymbolId symbolId() const noexcept override
        {
            return this->bookSymbolId;
        }

        PlaceOrderResult place(OrderNode& node, Timestamp timestamp, TradeBuffer& trades) override;
        ModifyOrderResult modifyPrice(OrderNode& node, Price price, Timestamp timestamp,
                                      TradeBuffer& trades) override;
        ModifyOrderResult modifyQuantity(OrderNode& node, QuantityChange change, Timestamp timestamp) override;
        CancelOrderResult cancel(OrderNode& node, Timestamp timestamp) override;

        [[nodiscard]]
        std::optional<Quote> bestBid() const noexcept override
        {
            return quote(this->bids.best());
        }

        [[nodiscard]]
        std::optional<Quote> bestAsk() const noexcept override
        {
            return quote(this->asks.best());
        }

        [[nodiscard]]
        std::optional<Quote> bidAt(Price price) const noexcept override
        {
            return quote(this->bids.find(price));
        }

        [[nodiscard]]
        std::optional<Quote> askAt(Price price) const noexcept override
        {
            return quote(this->asks.find(price));
        }

        [[nodiscard]]
        std::optional<Balance> estimateMarketBuyCost(Quantity quantity) const noexcept override;
    };
} // advmatch

#endif // ADVMATCH_INDEXED_TREE_ORDER_BOOK_H
