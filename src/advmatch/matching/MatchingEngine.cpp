#include "MatchingEngine.h"
#include "contracts/ContractEngine.h"
#include "orderbooks/impl/IndexedTreeOrderBook.h"
#include "orderbooks/impl/RadixOrderBook.h"

#include <limits>

namespace advmatch
{
    namespace
    {
        std::unique_ptr<OrderBook> createOrderBook(SymbolId symbolId, MatchPolicy* policy, OrderBookImplementation implementation)
        {
            if (implementation == OrderBookImplementation::IndexedTree)
            {
                return std::make_unique<IndexedTreeOrderBook>(symbolId, policy);
            }

            return std::make_unique<RadixOrderBook>(symbolId, policy);
        }
    }

    OrderNode::~OrderNode()
    {
        if (this->contractParticipant)
        {
            --this->contractParticipant->orderReferences;
        }
    }

    MatchingEngine::MatchingEngine(UserManager& userManager, SymbolManager& symbolManager)
        : contracts(std::make_unique<ContractEngine>(*this)), userManager(userManager), symbolManager(symbolManager),
          settleEngine(this->accountLedger)
    {
    }

    MatchingEngine::~MatchingEngine() = default;

    HistoryArchiveResult MatchingEngine::archiveHistory(HistorySink& sink, std::size_t scanBudget)
    {
        HistoryArchiveResult result;
        const std::size_t orderBudget = std::min(this->orders.size(), scanBudget / 2 + (scanBudget % 2 != 0 && !this->historyPositionTurn));
        this->historyPositionTurn = !this->historyPositionTurn;
        while (result.scannedOrders < orderBudget)
        {
            this->historyCursor %= this->orders.size();
            OrderNode*& node = this->orders[this->historyCursor++];
            ++result.scannedOrders;
            if (!node || !isOrderStatusTerminal(node->basicOrder.getStatus()) || node->reservedBalance != 0 || node->priceLevel ||
                node->contractPrev || node->contractNext || (node->contractParticipant && node->contractParticipant->firstOrder == node))
            {
                continue;
            }

            if (sink.archiveOrder(node->basicOrder))
            {
                this->orderPool.release(node);
                node = nullptr;
                ++result.archivedOrders;
            }
        }

        const auto positions = this->contracts->archiveHistory(sink, scanBudget - result.scannedOrders);
        result.scannedPositions = positions.scannedPositions;
        result.archivedPositions = positions.archivedPositions;
        return result;
    }

    ContractEngine& MatchingEngine::getContractEngine() noexcept
    {
        return *this->contracts;
    }

    const ContractEngine& MatchingEngine::getContractEngine() const noexcept
    {
        return *this->contracts;
    }

    ContractResult MatchingEngine::addContractBook(SymbolId symbolId, const ContractSpec& spec, Price markPrice, Timestamp timestamp,
        OrderBookImplementation implementation)
    {
        const Symbol* symbol = this->symbolManager.getSymbol(symbolId);
        if (!symbol || this->findBook(symbolId))
        {
            return {ContractState::InvalidSymbol};
        }

        const auto result = this->contracts->addMarket(*symbol, spec, markPrice, timestamp);
        if (!result)
        {
            return result;
        }

        this->orderBooks.resize(std::max(this->orderBooks.size(), static_cast<std::size_t>(symbolId)));
        this->bookServices.resize(this->orderBooks.size());
        this->orderBooks[symbolId - 1] = createOrderBook(symbolId, &this->contracts->getSettleEngine(), implementation);
        this->orderBooks[symbolId - 1]->observationListener = this->observationListener;
        ++this->orderBookCount;
        this->bookServices[symbolId - 1] = {&this->contracts->getRiskEngine(), &this->contracts->getLedger(), &this->contracts->getSettleEngine()};
        return {};
    }

    PlaceOrderResult MatchingEngine::placeContractOrder(UserId userId, SymbolId symbolId, OrderSide side, OrderType type,
        TimeInForce timeInForce, Price price, Quantity quantity, Timestamp timestamp,
        const ContractOrderOptions& options, TradeBuffer& trades)
    {
        return this->submitOrder(userId, symbolId, side, type, timeInForce, price, quantity, timestamp, trades, &options, false);
    }

    bool MatchingEngine::reservePriceLevels(SymbolId symbolId, std::size_t bidLevels, std::size_t askLevels)
    {
        OrderBook* book = this->findBook(symbolId);
        if (!book)
        {
            return false;
        }

        book->reservePriceLevels(bidLevels, askLevels);
        return true;
    }

    bool MatchingEngine::addSpotBook(SymbolId symbolId, OrderBookImplementation implementation)
    {
        const Symbol* symbol = this->symbolManager.getSymbol(symbolId);
        if (!symbol || symbol->symbolType != SymbolType::Spot || symbol->baseAssetId == symbol->quoteAssetId)
        {
            return false;
        }

        if (this->orderBooks.size() < symbolId)
        {
            this->orderBooks.resize(symbolId);
        }

        std::unique_ptr<OrderBook>& book = this->orderBooks[symbolId - 1];
        if (book)
        {
            return false;
        }

        book = createOrderBook(symbolId, nullptr, implementation);
        book->observationListener = this->observationListener;
        ++this->orderBookCount;
        this->bookServices.resize(this->orderBooks.size());
        this->bookServices[symbolId - 1] = {&this->riskEngine, &this->accountLedger, &this->settleEngine};
        return true;
    }

    OrderNode* MatchingEngine::findNode(OrderId orderId) noexcept
    {
        if (orderId == 0 || orderId > this->orders.size())
        {
            return nullptr;
        }

        return this->orders[orderId - 1];
    }

    const OrderNode* MatchingEngine::findNode(OrderId orderId) const noexcept
    {
        if (orderId == 0 || orderId > this->orders.size())
        {
            return nullptr;
        }

        return this->orders[orderId - 1];
    }

    OrderBook* MatchingEngine::findBook(SymbolId symbolId) noexcept
    {
        if (symbolId == 0 || symbolId > this->orderBooks.size())
        {
            return nullptr;
        }

        return this->orderBooks[symbolId - 1].get();
    }

    const OrderBook* MatchingEngine::findBook(SymbolId symbolId) const noexcept
    {
        if (symbolId == 0 || symbolId > this->orderBooks.size())
        {
            return nullptr;
        }

        return this->orderBooks[symbolId - 1].get();
    }

    PlaceOrderResult MatchingEngine::placeOrder(UserId userId, SymbolId symbolId, OrderSide side, OrderType type,
                                                TimeInForce timeInForce, Price price,
                                                Quantity quantity, Timestamp timestamp, TradeBuffer& trades)
    {
        return this->submitOrder(userId, symbolId, side, type, timeInForce, price, quantity, timestamp, trades, nullptr, false);
    }

    PlaceOrderResult MatchingEngine::submitOrder(UserId userId, SymbolId symbolId, OrderSide side, OrderType type,
        TimeInForce timeInForce, Price price, Quantity quantity, Timestamp timestamp, TradeBuffer& trades,
        const ContractOrderOptions* options, bool liquidation, const OrderSubmissionContext* context)
    {
        trades.clear();

        const auto reject = [&](PlaceOrderState reason)
        {
            return PlaceOrderResult(0, {OrderStatus::Rejected, 0, quantity}, reason);
        };

        User* user = context ? context->user : this->userManager.getUser(userId);
        if (!user || !user->isActive())
        {
            return reject(PlaceOrderState::UserNotFound);
        }

        const Symbol* symbol = context ? context->symbol : this->symbolManager.getSymbol(symbolId);
        if (!symbol || !symbol->isActive())
        {
            return reject(PlaceOrderState::InvalidSymbol);
        }

        OrderBook* book = context ? context->book : findBook(symbolId);
        if (!book)
        {
            return reject(PlaceOrderState::InvalidSymbol);
        }

        const bool contract = symbol->symbolType != SymbolType::Spot;
        if (contract != (options != nullptr))
        {
            return reject(PlaceOrderState::InvalidSymbol);
        }

        BookServices& services = this->bookServices[symbolId - 1];
        if (this->nextOrderId == 0)
        {
            return reject(PlaceOrderState::OrderIdOverflow);
        }

        const OrderId id = this->nextOrderId++;

        if (this->orders.size() < id)
        {
            this->orders.resize(id, nullptr);
        }

        CreateOrderResult orderResult = this->orderFactory.createOrder(id, userId, symbolId, side, type, timeInForce, price, quantity, timestamp);
        if (!orderResult)
        {
            return reject(orderResult.state);
        }

        MKTOverflowBidResult mktBidResult = contract ? MKTOverflowBidResult{PlaceOrderState::Success, 0}
            : this->orderFactory.checkMKTOverflowBid(*orderResult.content, book);
        if (mktBidResult.first != PlaceOrderState::Success)
        {
            return reject(mktBidResult.first);
        }

        OrderNode* node = this->orderPool.emplace(
            *orderResult.content,
            *user, contract ? static_cast<Account&>(user->getContractAccount()) : static_cast<Account&>(user->getSpotAccount()),
            *symbol, symbol->baseAssetId, symbol->quoteAssetId);

        if (contract)
        {
            node->liquidationOrder = liquidation;
            const ContractState state = this->contracts->prepareOrder(*node, *options, timestamp, context ? context->participant : nullptr);
            if (state != ContractState::Success)
            {
                this->orderPool.release(node);
                const PlaceOrderState reason = ContractRiskEngine::placeState(state);
                return reject(reason);
            }
        }

        const PlaceOrderRiskDecision riskDecision = services.risk->checkPlaceOrder(*node, mktBidResult.second);
        if (riskDecision.rejectReason)
        {
            if (contract)
            {
                this->contracts->discardPreparedOrder(*node);
            }

            this->orderPool.release(node);
            return reject(*riskDecision.rejectReason);
        }

        this->orders[id - 1] = node;
        if (contract && liquidation && node->contractParticipant->liquidationRetry)
        {
            const OrderSide opposingSide = side == OrderSide::Bid ? OrderSide::Ask : OrderSide::Bid;
            const PriceLevel* level = book->observedBestLevel(opposingSide);
            if (level && this->contracts->getSettleEngine().matchesLiquidationRetry(*level->head, *node, level->price))
            {
                // Factory, timestamp and risk checks already passed. This is
                // the same failed internal market IOC, with zero reservation:
                // retain its cancelled history/participant reference but avoid
                // attach/detach and pointless zero-value ledger operations.
                static_cast<void>(node->basicOrder.cancel(timestamp));
                PlaceOrderResult cancelled(id, node->basicOrder.getFillingStatus());
                cancelled.positionId = node->contractParticipant->position->positionId;
                return cancelled;
            }
        }

        if (!services.ledger->reserve(*node, riskDecision.requiredReservationBalance))
        {
            this->orders[id - 1] = nullptr;
            if (contract)
            {
                this->contracts->discardPreparedOrder(*node);
            }

            this->orderPool.release(node);
            return reject(PlaceOrderState::InsufficientBalance);
        }

        if (contract)
        {
            ContractSettleEngine::attach(*node);
        }

        auto result = book->place(*node, timestamp, trades);
        if (contract)
        {
            result.positionId = node->contractParticipant->position->positionId;
        }

        if (!result)
        {
            services.settlement->releaseReservation(*node);
            this->orders[id - 1] = nullptr;
            if (contract)
            {
                this->contracts->discardPreparedOrder(*node);
            }

            this->orderPool.release(node);
            return result;
        }

        services.settlement->settleTrades(trades);
        services.settlement->reconcileReservation(*node);
        if (contract && !liquidation)
        {
            if (node->priceLevel || !trades.empty())
            {
                ++node->contractMarket->liquidityRevision;
            }

            static_cast<void>(this->contracts->processLiquidations(symbolId, timestamp, false));
            PlaceOrderResult refreshed(result.orderId, node->basicOrder.getFillingStatus());
            refreshed.positionId = result.positionId;
            return refreshed;
        }

        return result;
    }

    ModifyOrderResult MatchingEngine::modifyPrice(UserId userId, OrderId orderId, Price price, Timestamp timestamp,
                                                  TradeBuffer& trades)
    {
        trades.clear();

        OrderNode* node = findNode(orderId);
        if (!node)
        {
            return {ModifyOrderState::OrderNotFound};
        }

        if (node->basicOrder.userId != userId)
        {
            return {ModifyOrderState::WrongUser};
        }

        OrderBook* book = findBook(node->basicOrder.symbolId);
        if (!book)
        {
            return {ModifyOrderState::OrderNotFound};
        }

        BookServices& services = this->bookServices[node->basicOrder.symbolId - 1];
        if (node->contractMarket)
        {
            if (this->contracts->beginCommand(node->basicOrder.symbolId, timestamp) != ContractState::Success)
            {
                return {ModifyOrderState::TradingDisabled};
            }
        }

        BasicOrder& order = node->basicOrder;
        if (order.getStatus() == OrderStatus::Filled)
        {
            return {ModifyOrderState::OrderAlreadyFilled};
        }

        if (order.getStatus() == OrderStatus::Cancelled)
        {
            return {ModifyOrderState::OrderCancelled};
        }

        if (order.orderType != OrderType::Limit)
        {
            return {ModifyOrderState::InvalidOrderType};
        }

        if (price == 0)
        {
            return {ModifyOrderState::InvalidPrice};
        }

        const ModifyOrderRiskDecision riskDecision = services.risk->checkModifyPrice(*node, price);
        if (riskDecision.rejectReason)
        {
            return {*riskDecision.rejectReason};
        }

        if (!services.ledger->reserve(*node, riskDecision.requiredReservationBalance))
        {
            return {ModifyOrderState::InsufficientBalance};
        }

        auto result = book->modifyPrice(*node, price, timestamp, trades);
        if (result)
        {
            services.settlement->settleTrades(trades);
        }

        services.settlement->reconcileReservation(*node);
        if (node->contractMarket)
        {
            if (result)
            {
                ++node->contractMarket->liquidityRevision;
            }

            static_cast<void>(this->contracts->processLiquidations(node->basicOrder.symbolId, timestamp, false));
        }

        return result;
    }

    ModifyOrderResult MatchingEngine::modifyQuantity(UserId userId, OrderId orderId, QuantityChange change,
                                                     Timestamp timestamp)
    {
        OrderNode* node = findNode(orderId);
        if (!node)
        {
            return {ModifyOrderState::OrderNotFound};
        }

        if (node->basicOrder.userId != userId)
        {
            return {ModifyOrderState::WrongUser};
        }

        OrderBook* book = findBook(node->basicOrder.symbolId);
        if (!book)
        {
            return {ModifyOrderState::OrderNotFound};
        }

        BasicOrder& order = node->basicOrder;
        if (order.getStatus() == OrderStatus::Filled)
        {
            return {ModifyOrderState::OrderAlreadyFilled};
        }

        if (order.getStatus() == OrderStatus::Cancelled)
        {
            return {ModifyOrderState::OrderCancelled};
        }

        BookServices& services = this->bookServices[node->basicOrder.symbolId - 1];
        if (node->contractMarket)
        {
            if (this->contracts->beginCommand(node->basicOrder.symbolId, timestamp) != ContractState::Success)
            {
                return {ModifyOrderState::TradingDisabled};
            }
        }

        const Quantity unfilledQuantity = order.getUnfilledQuantity();
        if (change > 0 && static_cast<Quantity>(change) > std::numeric_limits<Quantity>::max() - unfilledQuantity)
        {
            return {ModifyOrderState::Overflow};
        }

        const Quantity decrease = change < 0 ? static_cast<Quantity>(-(change + 1)) + 1 : 0;
        if (decrease > unfilledQuantity)
        {
            return {ModifyOrderState::InsufficientQuantity};
        }

        const ModifyOrderRiskDecision riskDecision = services.risk->checkModifyQuantity(*node, change);
        if (riskDecision.rejectReason)
        {
            return {*riskDecision.rejectReason};
        }

        if (!services.ledger->reserve(*node, riskDecision.requiredReservationBalance))
        {
            return {ModifyOrderState::InsufficientBalance};
        }

        auto result = book->modifyQuantity(*node, change, timestamp);
        services.settlement->reconcileReservation(*node);
        return result;
    }

    CancelOrderResult MatchingEngine::cancelOrder(UserId userId, OrderId orderId, Timestamp timestamp)
    {
        OrderNode* node = findNode(orderId);
        if (!node)
        {
            return {CancelOrderState::OrderNotFound};
        }

        if (node->basicOrder.userId != userId)
        {
            return {CancelOrderState::WrongUser};
        }

        OrderBook* book = findBook(node->basicOrder.symbolId);
        if (!book)
        {
            return {CancelOrderState::OrderNotFound};
        }

        BookServices& services = this->bookServices[node->basicOrder.symbolId - 1];
        auto result = book->cancel(*node, timestamp);
        if (result)
        {
            services.settlement->reconcileReservation(*node);
        }

        return result;
    }

    BalanceResult MatchingEngine::transferToContract(UserId userId, AssetId assetId, PositiveBalanceChange amount)
    {
        User* user = this->userManager.getUser(userId);
        if (!user)
        {
            return {BalanceOpResult::InvalidAccount, "User ledging was not found"};
        }

        return user->transferToContract(assetId, amount);
    }

    BalanceResult MatchingEngine::transferToSpot(UserId userId, AssetId assetId, PositiveBalanceChange amount)
    {
        User* user = this->userManager.getUser(userId);
        if (!user)
        {
            return {BalanceOpResult::InvalidAccount, "User ledging was not found"};
        }

        return user->transferToSpot(assetId, amount);
    }

    std::optional<BasicOrder> MatchingEngine::queryOrder(OrderId orderId) const
    {
        const OrderNode* node = this->findNode(orderId);
        if (!node)
        {
            return std::nullopt;
        }

        return node->basicOrder;
    }

    std::optional<Quote> MatchingEngine::bestBid(SymbolId symbolId) const noexcept
    {
        const OrderBook* book = findBook(symbolId);
        return book ? book->bestBid() : std::nullopt;
    }

    std::optional<Quote> MatchingEngine::bestAsk(SymbolId symbolId) const noexcept
    {
        const OrderBook* book = findBook(symbolId);
        return book ? book->bestAsk() : std::nullopt;
    }

    std::optional<Quote> MatchingEngine::bidAt(SymbolId symbolId, Price price) const noexcept
    {
        const OrderBook* book = findBook(symbolId);
        return book ? book->bidAt(price) : std::nullopt;
    }

    std::optional<Quote> MatchingEngine::askAt(SymbolId symbolId, Price price) const noexcept
    {
        const OrderBook* book = findBook(symbolId);
        return book ? book->askAt(price) : std::nullopt;
    }
} // advmatch
