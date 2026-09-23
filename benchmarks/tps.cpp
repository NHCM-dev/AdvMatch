#include "advmatch/consuming/SingleThreadConsumer.h"
#include "advmatch/matching/MatchingEngine.h"

#include <barrier>
#include <bit>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

namespace
{
    using Clock = std::chrono::steady_clock;

    struct Options
    {
        std::uint64_t orders = 1000000;
        std::uint32_t producers = 4;
        std::uint32_t batchSize = 256;
        std::uint32_t publishBatchSize = 0;
        int ringSize = 32;
    };

    struct PlaceCommand
    {
        advmatch::UserId userId;
        advmatch::SymbolId symbolId;
        advmatch::OrderSide side;
        advmatch::OrderType type;
        advmatch::TimeInForce timeInForce;
        advmatch::Price price;
        advmatch::Quantity quantity;
        advmatch::Timestamp timestamp;
    };

    struct PlaceResponse
    {
        bool accepted;
        bool filled;
        std::size_t tradeCount;
    };

    class BenchmarkConsumer final : public advmatch::consuming::SingleThreadConsumer<PlaceCommand, PlaceResponse>
    {
        advmatch::MatchingEngine engine;
        advmatch::TradeBuffer trades;

    protected:
        PlaceResponse consume(PlaceCommand&& command) override
        {
            auto result = this->engine.placeOrder(command.userId, command.symbolId, command.side, command.type,
                command.timeInForce, command.price, command.quantity, command.timestamp, this->trades);
            return {static_cast<bool>(result), result.state.status == advmatch::OrderStatus::Filled, this->trades.size()};
        }

    public:
        BenchmarkConsumer(advmatch::UserManager& users, advmatch::SymbolManager& symbols)
            : engine(users, symbols)
        {}

        ~BenchmarkConsumer() override
        {
            this->stopBeforeDerivedDestruction();
        }

        bool addSpotBook(advmatch::SymbolId symbolId)
        {
            return this->engine.addSpotBook(symbolId);
        }

        void reserve(std::size_t orderCount, std::size_t bookCount, std::size_t activeOrderCount)
        {
            this->engine.reserve(orderCount, bookCount, activeOrderCount);
        }

        std::optional<advmatch::Quote> bestAsk(advmatch::SymbolId symbolId) const noexcept
        {
            return this->engine.bestAsk(symbolId);
        }
    };

    struct ProducerStats
    {
        std::uint64_t accepted = 0;
        std::uint64_t filled = 0;
        std::uint64_t trades = 0;
        std::exception_ptr error;
    };

    std::uint64_t parsePositive(std::string_view value)
    {
        std::uint64_t result = 0;
        const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
        if (error != std::errc{} || end != value.data() + value.size() || result == 0)
        {
            throw std::invalid_argument("all option values must be positive integers");
        }

        return result;
    }

    Options parseOptions(int argumentCount, char** arguments)
    {
        Options options;
        for (int index = 1; index < argumentCount; ++index)
        {
            const std::string_view name(arguments[index]);
            if (name == "--help")
            {
                std::cout << "Usage: advmatch_tps_benchmark [--orders N] [--producers N] [--batch-size N]";
                std::cout << " [--publish-batch N] [--ring-size N]\n";
                std::exit(0);
            }

            if (++index == argumentCount)
            {
                throw std::invalid_argument("missing value for benchmark option");
            }

            const std::uint64_t value = parsePositive(arguments[index]);
            if (name == "--orders")
            {
                options.orders = value;
            }
            else if (name == "--producers" && value <= std::numeric_limits<std::uint32_t>::max())
            {
                options.producers = static_cast<std::uint32_t>(value);
            }
            else if (name == "--batch-size" && value <= std::numeric_limits<std::uint32_t>::max())
            {
                options.batchSize = static_cast<std::uint32_t>(value);
            }
            else if (name == "--publish-batch" && value <= std::numeric_limits<std::uint32_t>::max())
            {
                options.publishBatchSize = static_cast<std::uint32_t>(value);
            }
            else if (name == "--ring-size" && value <= static_cast<std::uint64_t>(std::numeric_limits<int>::max()))
            {
                options.ringSize = static_cast<int>(value);
            }
            else
            {
                throw std::invalid_argument("unknown option or value out of range");
            }
        }

        if (options.orders > static_cast<std::uint64_t>(std::numeric_limits<advmatch::Balance>::max()) / 100)
        {
            throw std::invalid_argument("order count exceeds balance range");
        }

        if (!std::has_single_bit(static_cast<unsigned int>(options.ringSize)))
        {
            throw std::invalid_argument("ring size must be a power of two");
        }

        if (options.publishBatchSize > static_cast<std::uint32_t>(options.ringSize))
        {
            throw std::invalid_argument("publish batch size must not exceed ring size");
        }

        return options;
    }

    double secondsBetween(Clock::time_point first, Clock::time_point last)
    {
        return std::chrono::duration<double>(last - first).count();
    }
}

int main(int argumentCount, char** arguments)
{
    try
    {
        using namespace advmatch;

        const Options options = parseOptions(argumentCount, arguments);
        const std::uint32_t automaticPublishBatch = std::max(1U, std::min({64U, options.batchSize,
            static_cast<std::uint32_t>(options.ringSize) / options.producers}));
        const std::uint32_t publishBatchSize = options.publishBatchSize == 0 ? automaticPublishBatch : options.publishBatchSize;
        const unsigned int logicalThreads = std::thread::hardware_concurrency();

        if (logicalThreads != 0 && options.producers + 1 >= logicalThreads)
        {
            std::cerr << "configuration_warning=producer threads plus the consumer saturate available logical CPUs\n";
        }

        if (static_cast<std::uint64_t>(options.ringSize) <= static_cast<std::uint64_t>(options.producers) * 2)
        {
            std::cerr << "configuration_warning=ring size leaves too little burst capacity per producer\n";
        }
        UserManager users;
        SymbolManager symbols;
        users.reserve(2);
        symbols.reserve(1);

        const UserId sellerId = users.createUser();
        const UserId buyerId = users.createUser();
        const Symbol* symbol = symbols.createSymbol("BASE/QUOTE", SymbolType::Spot, 1, 2);
        if (!symbol || !users.getUser(sellerId)->updateBalance(1, static_cast<BalanceChange>(options.orders)) ||
            !users.getUser(buyerId)->updateBalance(2, static_cast<BalanceChange>(options.orders * 100)))
        {
            throw std::runtime_error("benchmark setup failed");
        }

        BenchmarkConsumer consumer(users, symbols);
        consumer.reserve(options.orders * 2, 1, options.orders);
        if (!consumer.addSpotBook(symbol->symbolId))
        {
            throw std::runtime_error("order book registration failed");
        }

        consumer.start(options.ringSize);

        const auto preloadStart = Clock::now();
        auto pendingSlots = std::make_unique<consuming::ResponseSlot<PlaceResponse>[]>(options.batchSize);
        std::vector<consuming::RequestTicket<PlaceResponse>> pending;
        pending.reserve(options.batchSize);
        for (std::uint64_t index = 0; index < options.orders;)
        {
            pending.clear();
            for (std::uint32_t batchIndex = 0; batchIndex < options.batchSize && index < options.orders; ++batchIndex, ++index)
            {
                pending.push_back(consumer.post({sellerId, symbol->symbolId, OrderSide::Ask, OrderType::Limit,
                    TimeInForce::GTC, 100, 1, index + 1}, pendingSlots[batchIndex]));
            }

            for (auto& response : pending)
            {
                const PlaceResponse outcome = response.get();
                if (!outcome.accepted || outcome.filled || outcome.tradeCount != 0)
                {
                    throw std::runtime_error("preload order failed");
                }
            }
        }

        const auto preloadEnd = Clock::now();
        std::vector<ProducerStats> stats(options.producers);
        std::vector<std::thread> producers;
        producers.reserve(options.producers);
        std::barrier startGate(static_cast<std::ptrdiff_t>(options.producers) + 1);

        for (std::uint32_t producer = 0; producer < options.producers; ++producer)
        {
            producers.emplace_back([&, producer]
            {
                startGate.arrive_and_wait();

                try
                {
                    const std::uint64_t count = (options.orders / options.producers) + (producer < options.orders % options.producers ? 1 : 0);
                    auto responseSlots = std::make_unique<consuming::ResponseSlot<PlaceResponse>[]>(options.batchSize);
                    std::vector<PlaceCommand> commands;
                    commands.reserve(options.batchSize);
                    std::vector<consuming::RequestTicket<PlaceResponse>> responses;
                    responses.reserve(options.batchSize);

                    for (std::uint64_t index = 0; index < count;)
                    {
                        responses.clear();
                        commands.clear();
                        for (std::uint32_t batchIndex = 0; batchIndex < options.batchSize && index < count; ++batchIndex, ++index)
                        {
                            commands.push_back({buyerId, symbol->symbolId, OrderSide::Bid, OrderType::Limit,
                                TimeInForce::IOC, 100, 1, index + 1});
                        }

                        consumer.postBatch(commands, {responseSlots.get(), commands.size()}, responses, publishBatchSize);

                        for (auto& response : responses)
                        {
                            const PlaceResponse outcome = response.get();
                            stats[producer].accepted += outcome.accepted;
                            stats[producer].filled += outcome.filled;
                            stats[producer].trades += outcome.tradeCount;
                        }
                    }
                }
                catch (...)
                {
                    stats[producer].error = std::current_exception();
                }
            });
        }

        const auto matchingStart = Clock::now();
        startGate.arrive_and_wait();
        for (auto& producer : producers)
        {
            producer.join();
        }

        const auto matchingEnd = Clock::now();
        consumer.stop();

        std::uint64_t accepted = 0;
        std::uint64_t filled = 0;
        std::uint64_t trades = 0;
        for (const ProducerStats& producer : stats)
        {
            if (producer.error)
            {
                std::rethrow_exception(producer.error);
            }

            accepted += producer.accepted;
            filled += producer.filled;
            trades += producer.trades;
        }

        const User* seller = users.getUser(sellerId);
        const User* buyer = users.getUser(buyerId);
        const bool balancesMatch = seller->queryActiveBalance(1).value_or(0) == 0 && seller->queryReservedBalance(1).value_or(0) == 0 &&
            seller->queryActiveBalance(2).value_or(0) == static_cast<Balance>(options.orders * 100) &&
            buyer->queryActiveBalance(1).value_or(0) == static_cast<Balance>(options.orders) &&
            buyer->queryActiveBalance(2).value_or(0) == 0 && buyer->queryReservedBalance(2).value_or(0) == 0;

        if (accepted != options.orders || filled != options.orders || trades != options.orders || consumer.bestAsk(symbol->symbolId) || !balancesMatch)
        {
            throw std::runtime_error("benchmark results did not match the expected fills");
        }

        const double elapsed = secondsBetween(matchingStart, matchingEnd);
        std::cout << "scenario=preloaded_asks_then_limit_bids\n";
        std::cout << "logical_threads=" << logicalThreads << '\n';
        std::cout << "orders=" << options.orders << " producers=" << options.producers << " batch_size=" << options.batchSize;
        std::cout << " publish_batch=" << publishBatchSize << " ring_size=" << options.ringSize << '\n';
        std::cout << "preload_seconds=" << secondsBetween(preloadStart, preloadEnd) << '\n';
        std::cout << "matched=" << filled << " trades=" << trades << " elapsed_seconds=" << elapsed << '\n';
        std::cout << "tps=" << static_cast<double>(filled) / elapsed << '\n';
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "Benchmark failed: " << error.what() << '\n';
        return 1;
    }
}
