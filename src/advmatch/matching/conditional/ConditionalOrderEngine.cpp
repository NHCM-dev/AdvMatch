#include "ConditionalOrderEngine.h"
#include "advmatch/matching/MatchingEngine.h"
#include "advmatch/matching/contracts/ContractEngine.h"

#include <limits>

namespace advmatch
{
    void ConditionalOrderEngine::addSymbol(SymbolId symbolId)
    {
        this->markets.resize(std::max(this->markets.size(), static_cast<std::size_t>(symbolId)));
        this->prices.resize(this->markets.size());
    }

    ConditionalOrderEngine::Node* ConditionalOrderEngine::find(ConditionalOrderId id) const noexcept
    {
        return id != 0 && id <= this->orders.size() ? this->orders[id - 1] : nullptr;
    }

    bool ConditionalOrderEngine::rising(const ConditionalOrderRequest& request) noexcept
    {
        return (request.kind == ConditionalOrderKind::StopLoss) == (request.side == OrderSide::Bid);
    }

    ConditionalOrderState ConditionalOrderEngine::validate(const ConditionalOrderRequest& request) const
    {
        const auto* user = this->matching.userManager.getUser(request.userId);
        if (!user || !user->isActive())
        {
            return ConditionalOrderState::InvalidUser;
        }

        const auto* symbol = this->matching.symbolManager.getSymbol(request.symbolId);
        if (!symbol || !this->matching.findBook(request.symbolId))
        {
            return ConditionalOrderState::InvalidSymbol;
        }

        const bool contract = symbol->symbolType != SymbolType::Spot;
        if (contract != request.contractOptions.has_value() || (!contract && request.source != TriggerPriceSource::LastTrade))
        {
            return ConditionalOrderState::InvalidOrder;
        }

        if (request.kind > ConditionalOrderKind::TakeProfit || request.source > TriggerPriceSource::Index || request.triggerPrice == 0 || request.triggerPrice > static_cast<Price>(std::numeric_limits<Balance>::max()))
        {
            return ConditionalOrderState::InvalidTrigger;
        }

        const auto state = this->matching.orderFactory.basicCheck(request.side, request.type, request.timeInForce, request.price, request.quantity, request.timestamp);
        if (state != PlaceOrderState::Success)
        {
            return state == PlaceOrderState::InvalidTimestamp ? ConditionalOrderState::InvalidTimestamp : ConditionalOrderState::InvalidOrder;
        }

        if (request.triggerPrice % symbol->tradingRules.priceTick != 0 || this->matching.getTradingRuleEngine().check(*symbol, request.side, request.type, request.price, request.quantity, request.contractOptions ? request.contractOptions->priceLimit : 0) != PlaceOrderState::Success)
        {
            return ConditionalOrderState::InvalidOrder;
        }

        if (request.timestamp < this->prices[request.symbolId - 1][static_cast<std::size_t>(request.source)].timestamp)
        {
            return ConditionalOrderState::InvalidTimestamp;
        }

        const auto& conditionalMarket = this->markets[request.symbolId - 1];
        if (conditionalMarket && request.timestamp < conditionalMarket->commandTimestamp)
        {
            return ConditionalOrderState::InvalidTimestamp;
        }

        if (contract)
        {
            const auto* market = this->matching.getContractEngine().findMarket(request.symbolId);
            if (!market || market->status != ContractStatus::Trading || request.timestamp < market->commandTimestamp || (symbol->symbolType == SymbolType::Future && request.timestamp >= market->spec.expiryTimestamp))
            {
                return ConditionalOrderState::InvalidSymbol;
            }

            const auto& options = *request.contractOptions;
            if (options.marginMode > MarginMode::Cross || options.leverage > market->spec.maxLeverage || (request.type == OrderType::Market && options.priceLimit == 0))
            {
                return ConditionalOrderState::InvalidOrder;
            }

            if (options.positionId != 0)
            {
                const auto position = this->matching.getContractEngine().queryPosition(options.positionId);
                if (!position || position->userId != request.userId || position->symbolId != request.symbolId || position->closed || (options.reduceOnly && ((position->positionSide == PositionSide::Long) == (request.side == OrderSide::Bid))))
                {
                    return ConditionalOrderState::InvalidOrder;
                }
            }
            else if (options.reduceOnly)
            {
                return ConditionalOrderState::InvalidOrder;
            }
        }

        return ConditionalOrderState::Success;
    }

    void ConditionalOrderEngine::attach(Node& node, Index& index, Index::Entry& entry) noexcept
    {
        node.index = &index;
        node.bucket = &entry;
        node.previous = entry.value.tail;
        node.next = nullptr;
        if (node.previous)
        {
            node.previous->next = &node;
        }
        else
        {
            entry.value.head = &node;
        }

        entry.value.tail = &node;
        ++this->markets[node.view.request.symbolId - 1]->waiting;
    }

    void ConditionalOrderEngine::detach(Node& node) noexcept
    {
        auto& bucket = node.bucket->value;
        if (node.previous)
        {
            node.previous->next = node.next;
        }
        else
        {
            bucket.head = node.next;
        }

        if (node.next)
        {
            node.next->previous = node.previous;
        }
        else
        {
            bucket.tail = node.previous;
        }

        if (!bucket.head)
        {
            node.index->erase(*node.bucket);
        }

        node.bucket = nullptr;
        node.index = nullptr;
        node.previous = nullptr;
        node.next = nullptr;
        --this->markets[node.view.request.symbolId - 1]->waiting;
    }

    ConditionalOrderResult ConditionalOrderEngine::place(const ConditionalOrderRequest& request)
    {
        const auto state = this->validate(request);
        if (state != ConditionalOrderState::Success)
        {
            return {state};
        }

        Node* sibling = request.ocoOrderId == 0 ? nullptr : this->find(request.ocoOrderId);
        if (request.ocoOrderId != 0 && (!sibling || sibling->view.status != ConditionalOrderStatus::Waiting || sibling->view.request.userId != request.userId || sibling->view.request.symbolId != request.symbolId || sibling->view.request.ocoOrderId != 0))
        {
            return {ConditionalOrderState::InvalidOco};
        }

        if (this->orders.size() == std::numeric_limits<ConditionalOrderId>::max())
        {
            return {ConditionalOrderState::Overflow};
        }

        auto& market = this->markets[request.symbolId - 1];
        if (!market)
        {
            market = std::make_unique<Market>();
        }

        auto& index = rising(request) ? market->rising[static_cast<std::size_t>(request.source)] : market->falling[static_cast<std::size_t>(request.source)];
        auto& entry = index.getOrCreate(request.triggerPrice);
        Node* node = this->pool.emplace();
        try
        {
            this->orders.push_back(node);
        }
        catch (...)
        {
            this->pool.release(node);
            if (!entry.value.head)
            {
                index.erase(entry);
            }

            throw;
        }

        node->view.conditionalOrderId = this->orders.size();
        node->view.request = request;
        node->view.updatedTimestamp = request.timestamp;
        this->attach(*node, index, entry);
        if (sibling)
        {
            sibling->view.request.ocoOrderId = node->view.conditionalOrderId;
            sibling->view.updatedTimestamp = request.timestamp;
        }

        market->commandTimestamp = request.timestamp;

        const auto point = this->prices[request.symbolId - 1][static_cast<std::size_t>(request.source)];
        if (point.price != 0)
        {
            this->events.push_back({request.symbolId, request.source, {point.price, request.timestamp}});
        }

        this->process();
        return {ConditionalOrderState::Success, node->view.conditionalOrderId};
    }

    ConditionalOrderResult ConditionalOrderEngine::modify(UserId userId, ConditionalOrderId id, Price triggerPrice, Price price, Quantity quantity, Timestamp timestamp)
    {
        Node* node = this->find(id);
        if (!node)
        {
            return {ConditionalOrderState::NotFound};
        }

        if (node->view.request.userId != userId)
        {
            return {ConditionalOrderState::WrongUser};
        }

        if (node->view.status != ConditionalOrderStatus::Waiting)
        {
            return {ConditionalOrderState::NotWaiting};
        }

        if (timestamp < node->view.updatedTimestamp)
        {
            return {ConditionalOrderState::InvalidTimestamp};
        }

        auto request = node->view.request;
        request.triggerPrice = triggerPrice;
        request.price = price;
        request.quantity = quantity;
        request.timestamp = timestamp;
        const auto state = this->validate(request);
        if (state != ConditionalOrderState::Success)
        {
            return {state};
        }

        if (triggerPrice != node->view.request.triggerPrice)
        {
            Index* index = node->index;
            auto& entry = index->getOrCreate(triggerPrice);
            this->detach(*node);
            this->attach(*node, *index, entry);
        }

        node->view.request = request;
        node->view.updatedTimestamp = timestamp;
        this->markets[request.symbolId - 1]->commandTimestamp = timestamp;
        const auto point = this->prices[request.symbolId - 1][static_cast<std::size_t>(request.source)];
        if (point.price != 0)
        {
            this->events.push_back({request.symbolId, request.source, {point.price, timestamp}});
        }

        this->process();
        return {ConditionalOrderState::Success, id};
    }

    void ConditionalOrderEngine::cancel(Node& node, Timestamp timestamp) noexcept
    {
        this->detach(node);
        node.view.status = ConditionalOrderStatus::Cancelled;
        node.view.updatedTimestamp = timestamp;
        Node* sibling = this->find(node.view.request.ocoOrderId);
        if (sibling && sibling->view.status == ConditionalOrderStatus::Waiting)
        {
            sibling->view.request.ocoOrderId = 0;
            sibling->view.updatedTimestamp = timestamp;
        }
    }

    ConditionalOrderResult ConditionalOrderEngine::cancel(UserId userId, ConditionalOrderId id, Timestamp timestamp)
    {
        Node* node = this->find(id);
        if (!node)
        {
            return {ConditionalOrderState::NotFound};
        }

        if (node->view.request.userId != userId)
        {
            return {ConditionalOrderState::WrongUser};
        }

        if (node->view.status != ConditionalOrderStatus::Waiting)
        {
            return {ConditionalOrderState::NotWaiting};
        }

        const auto source = static_cast<std::size_t>(node->view.request.source);
        if (timestamp < node->view.updatedTimestamp || timestamp < this->prices[node->view.request.symbolId - 1][source].timestamp || timestamp < this->markets[node->view.request.symbolId - 1]->commandTimestamp)
        {
            return {ConditionalOrderState::InvalidTimestamp};
        }

        this->cancel(*node, timestamp);
        this->markets[node->view.request.symbolId - 1]->commandTimestamp = timestamp;
        return {ConditionalOrderState::Success, id};
    }

    void ConditionalOrderEngine::cancelSymbol(SymbolId symbolId, Timestamp timestamp)
    {
        if (symbolId == 0 || symbolId > this->markets.size() || !this->markets[symbolId - 1])
        {
            return;
        }

        auto& market = *this->markets[symbolId - 1];
        for (std::size_t source = 0; source < 3; ++source)
        {
            for (Index* index : {&market.rising[source], &market.falling[source]})
            {
                while (index->first())
                {
                    this->cancel(*index->first()->value.head, timestamp);
                }
            }
        }
    }

    void ConditionalOrderEngine::price(SymbolId symbolId, TriggerPriceSource source, Price value, Timestamp timestamp)
    {
        if (value == 0 || source > TriggerPriceSource::Index || symbolId == 0 || symbolId > this->prices.size())
        {
            return;
        }

        auto& point = this->prices[symbolId - 1][static_cast<std::size_t>(source)];
        const auto& market = this->markets[symbolId - 1];
        if (timestamp < point.timestamp || (market && timestamp < market->commandTimestamp))
        {
            return;
        }

        point = {value, timestamp};
        if (market && market->waiting != 0)
        {
            this->events.push_back({symbolId, source, point});
        }
    }

    void ConditionalOrderEngine::trades(SymbolId symbolId, const TradeBuffer& trades, Timestamp timestamp)
    {
        for (const auto& trade : trades)
        {
            this->price(symbolId, TriggerPriceSource::LastTrade, trade.price, timestamp);
        }
    }

    void ConditionalOrderEngine::activate(Node& node, const PriceEvent& event)
    {
        this->detach(node);
        const auto& request = node.view.request;
        node.view.triggeredPrice = event.point.price;
        node.view.updatedTimestamp = event.point.timestamp;
        TradeBuffer trades;
        const auto result = this->matching.submitOrder(request.userId, request.symbolId, request.side, request.type, request.timeInForce, request.price, request.quantity, event.point.timestamp, trades, request.contractOptions ? &*request.contractOptions : nullptr, false);
        node.view.orderId = result.orderId;
        node.view.positionId = result.positionId;
        node.view.rejectReason = result.rejectReason;
        node.view.status = result ? ConditionalOrderStatus::Activated : ConditionalOrderStatus::Rejected;
        this->executedTrades.appendSettled(trades);
        Node* sibling = this->find(request.ocoOrderId);
        if (result && sibling && sibling->view.status == ConditionalOrderStatus::Waiting)
        {
            this->cancel(*sibling, event.point.timestamp);
        }
        else if (sibling && sibling->view.status == ConditionalOrderStatus::Waiting)
        {
            sibling->view.request.ocoOrderId = 0;
            sibling->view.updatedTimestamp = event.point.timestamp;
        }
    }

    void ConditionalOrderEngine::process()
    {
        if (this->processing || this->events.empty())
        {
            return;
        }

        // Child fills enqueue events, so cascades drain iteratively rather than
        // recursing through MatchingEngine and exhausting the stack.
        this->processing = true;
        try
        {
            while (!this->events.empty())
            {
                const auto event = this->events.front();
                this->events.pop_front();
                auto& market = *this->markets[event.symbolId - 1];
                const auto source = static_cast<std::size_t>(event.source);
                for (bool rising : {true, false})
                {
                    auto& index = rising ? market.rising[source] : market.falling[source];
                    while (auto* entry = rising ? index.first() : index.last())
                    {
                        if (rising ? entry->key > event.point.price : entry->key < event.point.price)
                        {
                            break;
                        }

                        this->activate(*entry->value.head, event);
                    }
                }
            }
        }
        catch (...)
        {
            this->processing = false;
            throw;
        }

        this->processing = false;
    }

    void ConditionalOrderEngine::collectTrades(TradeBuffer& output)
    {
        output.appendSettled(this->executedTrades);
        this->executedTrades.clear();
    }

    std::optional<ConditionalOrderView> ConditionalOrderEngine::query(ConditionalOrderId id) const
    {
        const Node* node = this->find(id);
        return node ? std::optional(node->view) : std::nullopt;
    }
}
