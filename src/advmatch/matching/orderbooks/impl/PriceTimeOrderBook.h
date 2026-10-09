#ifndef ADVMATCH_PRICE_TIME_ORDER_BOOK_H
#define ADVMATCH_PRICE_TIME_ORDER_BOOK_H

#include "../../Matching.h"
#include "../OrderBook.h"
#include "../../../util/Containers.h"

#include <functional>

namespace advmatch
{
    template <typename BidLevels, typename AskLevels>
    class PriceTimeOrderBook final : public OrderBook
    {
        SymbolId bookSymbolId;
        MatchPolicy* matchPolicy = nullptr;
        bool synchronousSettlement = false;
        BidLevels bids;
        AskLevels asks;

        static std::optional<Quote> quote(const PriceLevel* level) noexcept;
        static bool crosses(const BasicOrder& taker, Price makerPrice) noexcept;
        [[nodiscard]]
        bool canFillAll(const BasicOrder& order) const noexcept;
        void match(OrderNode& taker, Timestamp timestamp, TradeBuffer& trades);
        void append(OrderNode& node);
        void unlink(OrderNode& node, Quantity remaining) noexcept;

        const PriceLevel* observedBestLevel(OrderSide side) const noexcept override
        {
            return side == OrderSide::Bid ? this->bids.best() : this->asks.best();
        }

        const PriceLevel* observedNextLevel(OrderSide side, Price price) const noexcept override
        {
            return side == OrderSide::Bid ? this->bids.next(price) : this->asks.next(price);
        }

        std::size_t observedLevelCount(OrderSide side) const noexcept override
        {
            return side == OrderSide::Bid ? this->bids.size() : this->asks.size();
        }

    public:
        void setMatchPolicy(MatchPolicy* policy) noexcept override
        {
            this->matchPolicy = policy;
            this->synchronousSettlement = policy && policy->settlesTrades();
        }

        explicit PriceTimeOrderBook(SymbolId symbolId, MatchPolicy* matchPolicy = nullptr)
            : bookSymbolId(symbolId), matchPolicy(matchPolicy), synchronousSettlement(matchPolicy && matchPolicy->settlesTrades())
        {
        }

        void reservePriceLevels(std::size_t bidLevels, std::size_t askLevels) override
        {
            this->bids.reserve(bidLevels);
            this->asks.reserve(askLevels);
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
        void forEachLevel(OrderSide side, const std::function<void(const Quote&)>& visitor) const override
        {
            const auto visit = [&](const auto& levels)
            {
                levels.forEachWhile([&](const PriceLevel& level)
                {
                    visitor(Quote{level.price, level.totalQuantity, level.orderCount});
                    return true;
                });
            };

            if (side == OrderSide::Bid)
            {
                visit(this->bids);
            }
            else if (side == OrderSide::Ask)
            {
                visit(this->asks);
            }
        }

        void forEachOrderWhile(OrderSide side, const std::function<bool(const BasicOrder&)>& visitor) const override
        {
            const auto visit = [&](const auto& levels)
            {
                levels.forEachWhile([&](const PriceLevel& level)
                {
                    for (const OrderNode* node = level.head; node != nullptr; node = node->next)
                    {
                        if (!visitor(node->basicOrder))
                        {
                            return false;
                        }
                    }

                    return true;
                });
            };

            if (side == OrderSide::Bid)
            {
                visit(this->bids);
            }
            else if (side == OrderSide::Ask)
            {
                visit(this->asks);
            }
        }

        void forEachOrder(OrderSide side, const std::function<void(const BasicOrder&)>& visitor) const override
        {
            this->forEachOrderWhile(side, [&](const BasicOrder& order)
            {
                visitor(order);
                return true;
            });
        }

    };
} // advmatch

#include "PriceTimeOrderBook.tpp"

#endif // ADVMATCH_PRICE_TIME_ORDER_BOOK_H
