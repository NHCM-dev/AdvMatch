#include "QueuedMatchingEngine.h"

#include <stdexcept>
#include <utility>

namespace advmatch
{
    void QueuedMatchingEngine::Completion::fail(std::exception_ptr exception) noexcept
    {
        {
            std::lock_guard lock(this->mutex);
            this->error = exception;
            this->done = true;
        }

        this->condition.notify_one();
    }

    QueuedMatchingEngine::~QueuedMatchingEngine()
    {
        stop();
    }

    bool QueuedMatchingEngine::addSpotBook(SymbolId symbolId)
    {
        std::unique_lock lock(this->submitMutex);
        if (this->started)
        {
            throw std::logic_error("register books before start");
        }

        return this->engine.addSpotBook(symbolId);
    }

    void QueuedMatchingEngine::start(int ringSize)
    {
        std::unique_lock lock(this->submitMutex);
        if (this->started)
        {
            throw std::logic_error("engine already started");
        }

        if (this->stopping)
        {
            throw std::logic_error("engine cannot restart");
        }

        this->ring = Ring::createMultiProducer(std::make_shared<CommandFactory>(), ringSize, this->waitStrategy);
        this->ring->addGatingSequences(this->consumed);
        this->barrier = this->ring->newBarrier();
        this->worker = std::thread(&QueuedMatchingEngine::run, this);
        this->started = true;
    }

    void QueuedMatchingEngine::stop()
    {
        std::unique_lock lock(this->submitMutex);
        if (!this->started || this->stopping)
        {
            return;
        }

        this->stopping = true;
        const auto sequence = this->ring->next();
        this->ring->get(sequence) = Command{};
        this->ring->publish(sequence);

        lock.unlock();
        if (this->worker.joinable())
        {
            this->worker.join();
        }

        lock.lock();
        this->started = false;
    }

    void QueuedMatchingEngine::publish(Command command)
    {
        std::shared_lock lock(this->submitMutex);
        if (!this->started || this->stopping)
        {
            throw std::logic_error("engine is not accepting commands");
        }

        const auto sequence = this->ring->next();
        this->ring->get(sequence) = command;
        this->ring->publish(sequence);
    }

    void QueuedMatchingEngine::run() noexcept
    {
        std::int64_t next = this->consumed.get() + 1;
        for (;;)
        {
            const auto available = this->barrier->waitFor(next);
            for (; next <= available; ++next)
            {
                Command& command = this->ring->get(next);
                if (command.kind == Kind::Stop)
                {
                    this->consumed.set(next);
                    return;
                }

                handle(command);
                this->consumed.set(next);
            }
        }
    }

    void QueuedMatchingEngine::handle(Command& command) noexcept
    {
        Completion& completion = *command.completion;
        try
        {
            switch (command.kind)
            {
            case Kind::Place:
            {
                std::vector<Trade> trades;
                auto result = this->engine.placeOrder(command.userId, command.symbolId, command.side, command.type, command.timeInForce, command.price,
                                                      command.quantity, command.timestamp, trades);
                completion.finish(PlaceOutcome{std::move(result), std::move(trades)});
                break;
            }
            case Kind::ModifyPrice:
            {
                std::vector<Trade> trades;
                auto result = this->engine.modifyPrice(command.userId, command.orderId, command.price, command.timestamp, trades);
                completion.finish(ModifyPriceOutcome{std::move(result), std::move(trades)});
                break;
            }
            case Kind::ModifyQuantity:
                completion.finish(this->engine.modifyQuantity(command.userId, command.orderId, command.quantityChange, command.timestamp));
                break;
            case Kind::Cancel:
                completion.finish(this->engine.cancelOrder(command.userId, command.orderId, command.timestamp));
                break;
            case Kind::QueryOrder:
                completion.finish(this->engine.queryOrder(command.orderId));
                break;
            case Kind::BestBid:
                completion.finish(this->engine.bestBid(command.symbolId));
                break;
            case Kind::BestAsk:
                completion.finish(this->engine.bestAsk(command.symbolId));
                break;
            case Kind::BidAt:
                completion.finish(this->engine.bidAt(command.symbolId, command.price));
                break;
            case Kind::AskAt:
                completion.finish(this->engine.askAt(command.symbolId, command.price));
                break;
            case Kind::Stop:
                break;
            }
        }
        catch (...)
        {
            completion.fail(std::current_exception());
        }
    }

    PlaceOutcome QueuedMatchingEngine::placeOrder(UserId userId, SymbolId symbolId, OrderSide side, OrderType type, TimeInForce timeInForce, Price price,
                                                  Quantity quantity, Timestamp timestamp)
    {
        Command command;
        command.kind = Kind::Place;
        command.userId = userId;
        command.symbolId = symbolId;
        command.side = side;
        command.type = type;
        command.timeInForce = timeInForce;
        command.price = price;
        command.quantity = quantity;
        command.timestamp = timestamp;

        return request<PlaceOutcome>(command);
    }

    ModifyPriceOutcome QueuedMatchingEngine::modifyPrice(UserId userId, OrderId orderId, Price price, Timestamp timestamp)
    {
        Command command;
        command.kind = Kind::ModifyPrice;
        command.userId = userId;
        command.orderId = orderId;
        command.price = price;
        command.timestamp = timestamp;

        return request<ModifyPriceOutcome>(command);
    }

    ModifyOrderResult QueuedMatchingEngine::modifyQuantity(UserId userId, OrderId orderId, QuantityChange change, Timestamp timestamp)
    {
        Command command;
        command.kind = Kind::ModifyQuantity;
        command.userId = userId;
        command.orderId = orderId;
        command.quantityChange = change;
        command.timestamp = timestamp;

        return request<ModifyOrderResult>(command);
    }

    CancelOrderResult QueuedMatchingEngine::cancelOrder(UserId userId, OrderId orderId, Timestamp timestamp)
    {
        Command command;
        command.kind = Kind::Cancel;
        command.userId = userId;
        command.orderId = orderId;
        command.timestamp = timestamp;

        return request<CancelOrderResult>(command);
    }

    std::optional<BasicOrder> QueuedMatchingEngine::queryOrder(OrderId orderId)
    {
        Command command;
        command.kind = Kind::QueryOrder;
        command.orderId = orderId;

        return request<std::optional<BasicOrder>>(command);
    }

    std::optional<Quote> QueuedMatchingEngine::bestBid(SymbolId symbolId)
    {
        Command command;
        command.kind = Kind::BestBid;
        command.symbolId = symbolId;

        return request<std::optional<Quote>>(command);
    }

    std::optional<Quote> QueuedMatchingEngine::bestAsk(SymbolId symbolId)
    {
        Command command;
        command.kind = Kind::BestAsk;
        command.symbolId = symbolId;

        return request<std::optional<Quote>>(command);
    }

    std::optional<Quote> QueuedMatchingEngine::bidAt(SymbolId symbolId, Price price)
    {
        Command command;
        command.kind = Kind::BidAt;
        command.symbolId = symbolId;
        command.price = price;

        return request<std::optional<Quote>>(command);
    }

    std::optional<Quote> QueuedMatchingEngine::askAt(SymbolId symbolId, Price price)
    {
        Command command;
        command.kind = Kind::AskAt;
        command.symbolId = symbolId;
        command.price = price;

        return request<std::optional<Quote>>(command);
    }
} // advmatch
