#ifndef ADVMATCH_QUEUED_MATCHING_ENGINE_H
#define ADVMATCH_QUEUED_MATCHING_ENGINE_H

#include "MatchingEngine.h"

#include <disruptor/BlockingWaitStrategy.h>
#include <disruptor/RingBuffer.h>

#include <condition_variable>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <thread>
#include <utility>
#include <variant>

namespace advmatch
{
    struct PlaceOutcome
    {
        PlaceOrderResult result;
        std::vector<Trade> trades;
    };

    struct ModifyPriceOutcome
    {
        ModifyOrderResult result;
        std::vector<Trade> trades;
    };

    // Synchronous request interface over an MPSC Disruptor ring. Producer
    // threads may call request methods concurrently; one worker owns state.
    class QueuedMatchingEngine
    {
        using Response = std::variant<std::monostate, PlaceOutcome, ModifyPriceOutcome, ModifyOrderResult, CancelOrderResult, std::optional<BasicOrder>,
                                      std::optional<Quote>>;

        struct Completion
        {
            std::mutex mutex;
            std::condition_variable condition;
            bool done = false;
            Response response;
            std::exception_ptr error;

            template <typename T>
            void finish(T value)
            {
                {
                    std::lock_guard lock(this->mutex);
                    this->response.template emplace<T>(std::move(value));
                    this->done = true;
                }
                this->condition.notify_one();
            }

            void fail(std::exception_ptr exception) noexcept;

            template <typename T>
            T wait()
            {
                std::unique_lock lock(this->mutex);
                this->condition.wait(lock, [&] { return this->done; });
                if (this->error)
                {
                    std::rethrow_exception(this->error);
                }
                return std::move(std::get<T>(this->response));
            }
        };

        enum class Kind : std::uint8_t
        {
            Place,
            ModifyPrice,
            ModifyQuantity,
            Cancel,
            QueryOrder,
            BestBid,
            BestAsk,
            BidAt,
            AskAt,
            Stop
        };

        struct Command
        {
            Kind kind = Kind::Stop;
            UserId userId = 0;
            SymbolId symbolId = 0;
            OrderId orderId = 0;
            OrderSide side = OrderSide::Bid;
            OrderType type = OrderType::Limit;
            TimeInForce timeInForce = TimeInForce::GTC;
            Price price = 0;
            Quantity quantity = 0;
            QuantityChange quantityChange = 0;
            Timestamp timestamp = 0;
            Completion* completion = nullptr;
        };

        struct CommandFactory : disruptor::EventFactory<Command>
        {
            Command newInstance() override
            {
                return {};
            }
        };

        using Ring = disruptor::MultiProducerRingBuffer<Command, disruptor::BlockingWaitStrategy>;
        using Barrier = decltype(std::declval<Ring&>().newBarrier());

        MatchingEngine engine;
        disruptor::BlockingWaitStrategy waitStrategy;
        std::shared_ptr<Ring> ring;
        Barrier barrier;
        disruptor::Sequence consumed;
        std::thread worker;
        mutable std::shared_mutex submitMutex;
        bool started = false;
        bool stopping = false;

        void publish(Command command);
        void run() noexcept;
        void handle(Command& command) noexcept;

        template <typename T>
        T request(Command command)
        {
            Completion completion;
            command.completion = &completion;
            publish(command);
            return completion.wait<T>();
        }

    public:
        explicit QueuedMatchingEngine(UserManager& users, SymbolManager& symbols) : engine(users, symbols)
        {
        }
        ~QueuedMatchingEngine();
        QueuedMatchingEngine(const QueuedMatchingEngine&) = delete;
        QueuedMatchingEngine& operator=(const QueuedMatchingEngine&) = delete;

        bool addSpotBook(SymbolId symbolId);
        void start(int ringSize = 1024);
        void stop();

        PlaceOutcome placeOrder(UserId userId, SymbolId symbolId, OrderSide side, OrderType type,
            TimeInForce timeInForce, Price price, Quantity quantity,Timestamp timestamp);

        ModifyPriceOutcome modifyPrice(UserId userId, OrderId orderId, Price price, Timestamp timestamp);
        ModifyOrderResult modifyQuantity(UserId userId, OrderId orderId, QuantityChange change, Timestamp timestamp);

        CancelOrderResult cancelOrder(UserId userId, OrderId orderId, Timestamp timestamp);

        std::optional<BasicOrder> queryOrder(OrderId orderId);

        std::optional<Quote> bestBid(SymbolId symbolId);
        std::optional<Quote> bestAsk(SymbolId symbolId);

        std::optional<Quote> bidAt(SymbolId symbolId, Price price);
        std::optional<Quote> askAt(SymbolId symbolId, Price price);
    };
} // advmatch

#endif // ADVMATCH_QUEUED_MATCHING_ENGINE_H
