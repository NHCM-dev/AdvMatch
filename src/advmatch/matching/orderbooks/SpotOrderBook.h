#ifndef ADVMATCH_SPOT_ORDER_BOOK_H
#define ADVMATCH_SPOT_ORDER_BOOK_H

#include "../Matching.h"
#include "../abstract/OrderBook.h"

#include <functional>
#include <map>
#include <unordered_map>

namespace advmatch
{
    class SpotOrderBook final : public OrderBook
    {
        template <typename Compare>
        struct Levels
        {
            std::map<Price, PriceLevel, Compare> ordered;
            std::unordered_map<Price, PriceLevel*> byPrice;

            PriceLevel* find(Price price) noexcept
            {
                auto it = this->byPrice.find(price);
                return it == this->byPrice.end() ? nullptr : it->second;
            }

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

            const PriceLevel* best() const noexcept
            {
                return this->ordered.empty() ? nullptr : &this->ordered.begin()->second;
            }
        };

        SymbolId bookSymbolId;
        Levels<std::greater<Price>> bids;
        Levels<std::less<Price>> asks;

        static std::optional<Quote> quote(const PriceLevel* level) noexcept;
        bool crosses(const BasicOrder& taker, Price makerPrice) const noexcept;
        bool canFillAll(const BasicOrder& order) const noexcept;
        void match(OrderNode& taker, Timestamp timestamp, Sequence sequence, std::vector<Trade>& trades);
        void append(OrderNode& node);
        void unlink(OrderNode& node, Quantity remaining) noexcept;

    public:
        explicit SpotOrderBook(SymbolId symbolId) : bookSymbolId(symbolId)
        {
        }

        SymbolId symbolId() const noexcept override
        {
            return this->bookSymbolId;
        }
        PlaceOrderResult place(OrderNode& order, Timestamp timestamp, Sequence sequence, std::vector<Trade>& trades) override;
        ModifyOrderResult modifyPrice(OrderNode& order, Price price, Timestamp timestamp, Sequence sequence, std::vector<Trade>& trades) override;
        ModifyOrderResult modifyQuantity(OrderNode& order, QuantityChange change, Timestamp timestamp, Sequence sequence) override;
        CancelOrderResult cancel(OrderNode& order, Timestamp timestamp, Sequence sequence) override;
        std::optional<Quote> bestBid() const noexcept override
        {
            return quote(this->bids.best());
        }
        std::optional<Quote> bestAsk() const noexcept override
        {
            return quote(this->asks.best());
        }
        std::optional<Quote> bidAt(Price price) const noexcept override
        {
            return quote(this->bids.find(price));
        }
        std::optional<Quote> askAt(Price price) const noexcept override
        {
            return quote(this->asks.find(price));
        }

        [[nodiscard]]
        std::optional<Balance> marketBuyCost(Quantity quantity) const noexcept;
    };
} // advmatch

#endif // ADVMATCH_SPOT_ORDER_BOOK_H
