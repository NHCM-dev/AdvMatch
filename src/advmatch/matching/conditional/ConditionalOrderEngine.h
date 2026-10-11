#ifndef ADVMATCH_CONDITIONAL_ORDER_ENGINE_H
#define ADVMATCH_CONDITIONAL_ORDER_ENGINE_H

#include "advmatch/trading/conditional/ConditionalOrder.h"
#include "advmatch/util/RadixIndex.h"
#include "advmatch/matching/TradeBuffer.h"

#include <array>
#include <deque>
#include <span>

namespace advmatch
{
    class MatchingEngine;

    // Single owner, stable pooled nodes and direct ID slots. Trigger levels use
    // fixed-width radix indexes (at most 64 branches), never a full order scan.
    class ConditionalOrderEngine
    {
        friend class ::advmatch::StateSnapshotCodec;
        struct Node;
        struct Bucket
        {
            Node* head = nullptr;
            Node* tail = nullptr;
        };

        using Index = RadixIndex<Bucket>;

        struct Node
        {
            ConditionalOrderView view;
            Index::Entry* bucket = nullptr;
            Index* index = nullptr;
            Node* previous = nullptr;
            Node* next = nullptr;
        };

        struct Point
        {
            Price price = 0;
            Timestamp timestamp = 0;
        };

        struct Market
        {
            std::array<Index, 3> rising;
            std::array<Index, 3> falling;
            std::size_t waiting = 0;
            Timestamp commandTimestamp = 0;
        };

        struct PriceEvent
        {
            SymbolId symbolId;
            TriggerPriceSource source;
            Point point;
        };

        MatchingEngine& matching;
        BigAllocs<Node> pool;
        std::vector<Node*> orders;
        DenseMap<UserId, std::vector<ConditionalOrderId>> userHistory;
        DenseMap<UserId, DenseSet<ConditionalOrderId>> userWaiting;
        std::vector<std::unique_ptr<Market>> markets;
        std::vector<std::array<Point, 3>> prices;
        std::deque<PriceEvent> events;
        TradeBuffer executedTrades;
        bool processing = false;

        Node* find(ConditionalOrderId id) const noexcept;
        ConditionalOrderState validate(const ConditionalOrderRequest& request) const;
        static bool rising(const ConditionalOrderRequest& request) noexcept;
        void attach(Node& node, Index& index, Index::Entry& entry) noexcept;
        void detach(Node& node) noexcept;
        void activate(Node& node, const PriceEvent& event);
        void cancel(Node& node, Timestamp timestamp) noexcept;

    public:
        explicit ConditionalOrderEngine(MatchingEngine& matching)
            : matching(matching)
        {
        }

        void addSymbol(SymbolId symbolId);
        ConditionalOrderResult place(const ConditionalOrderRequest& request);
        ConditionalOrderResult modify(UserId userId, ConditionalOrderId id, Price triggerPrice, Price price, Quantity quantity, Timestamp timestamp);
        ConditionalOrderResult cancel(UserId userId, ConditionalOrderId id, Timestamp timestamp);
        void cancelSymbol(SymbolId symbolId, Timestamp timestamp);
        void resume(SymbolId symbolId, Timestamp timestamp);
        void price(SymbolId symbolId, TriggerPriceSource source, Price price, Timestamp timestamp);
        void trades(SymbolId symbolId, const TradeBuffer& trades, Timestamp timestamp);
        void process();
        void collectTrades(TradeBuffer& output);
        std::optional<ConditionalOrderView> query(ConditionalOrderId id) const;

        // Owner-thread borrowed IDs; copy immutable views before leaving the boundary.
        std::span<const ConditionalOrderId> userOrderIds(UserId userId, bool includeTerminal) const noexcept
        {
            if (includeTerminal)
            {
                const auto found = this->userHistory.find(userId);
                return found == this->userHistory.end() ? std::span<const ConditionalOrderId>{} : found->second;
            }
            const auto found = this->userWaiting.find(userId);
            return found == this->userWaiting.end() ? std::span<const ConditionalOrderId>{} : std::span<const ConditionalOrderId>(found->second.values());
        }

        Timestamp commandTimestamp(SymbolId symbolId) const noexcept
        {
            if (symbolId == 0 || symbolId > this->markets.size()) return 0;
            Timestamp timestamp = this->markets[symbolId - 1] ? this->markets[symbolId - 1]->commandTimestamp : 0;
            for (const auto& point : this->prices[symbolId - 1]) timestamp = std::max(timestamp, point.timestamp);
            return timestamp;
        }

        std::size_t size() const noexcept
        {
            return this->orders.size();
        }
    };
}

#endif // ADVMATCH_CONDITIONAL_ORDER_ENGINE_H
