#include "advmatch/consuming/SingleThreadConsumer.h"
#include "advmatch/matching/MatchingEngine.h"

#include <barrier>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <future>
#include <iostream>
#include <limits>
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

    protected:
        PlaceResponse consume(PlaceCommand&& command) override
        {
            std::vector<advmatch::Trade> trades;
            auto result = this->engine.placeOrder(command.userId, command.symbolId, command.side, command.type,
                command.timeInForce, command.price, command.quantity, command.timestamp, trades);
            return {static_cast<bool>(result), result.state.status == advmatch::OrderStatus::Filled, trades.size()};
        }

    public:
        BenchmarkConsumer(advmatch::UserManager& users, advmatch::SymbolManager& symbols)
            : engine(users, symbols)
        {}

        ~BenchmarkConsumer() override
        {
            stop();
        }

        bool addSpotBook(advmatch::SymbolId symbolId)
        {
            return this->engine.addSpotBook(symbolId);
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
                std::cout << "Usage: advmatch_tps_benchmark [--orders N] [--producers N] [--batch-size N] [--ring-size N]\n";
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

        if ((options.ringSize & (options.ringSize - 1)) != 0)
        {
            throw std::invalid_argument("ring size must be a power of two");
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
        UserManager users;
        SymbolManager symbols;
        const UserId sellerId = users.createUser();
        const UserId buyerId = users.createUser();
        const Symbol* symbol = symbols.createSymbol("BASE/QUOTE", SymbolType::Spot, 1, 2);
        if (!symbol || !users.getUser(sellerId)->updateBalance(1, static_cast<Balance>(options.orders)) ||
            !users.getUser(buyerId)->updateBalance(2, static_cast<Balance>(options.orders * 100)))
        {
            throw std::runtime_error("benchmark setup failed");
        }

        BenchmarkConsumer consumer(users, symbols);
        if (!consumer.addSpotBook(symbol->symbolId))
        {
            throw std::runtime_error("spot book registration failed");
        }

        consumer.start(options.ringSize);

        const auto preloadStart = Clock::now();
        std::vector<std::future<PlaceResponse>> pending;
        pending.reserve(options.batchSize);
        for (std::uint64_t index = 0; index < options.orders;)
        {
            pending.clear();
            for (std::uint32_t batchIndex = 0; batchIndex < options.batchSize && index < options.orders; ++batchIndex, ++index)
            {
                pending.push_back(consumer.post({sellerId, symbol->symbolId, OrderSide::Ask, OrderType::Limit,
                    TimeInForce::GTC, 100, 1, index + 1}));
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
                    const std::uint64_t count = options.orders / options.producers + (producer < options.orders % options.producers ? 1 : 0);
                    std::vector<std::future<PlaceResponse>> responses;
                    responses.reserve(options.batchSize);

                    for (std::uint64_t index = 0; index < count;)
                    {
                        responses.clear();
                        for (std::uint32_t batchIndex = 0; batchIndex < options.batchSize && index < count; ++batchIndex, ++index)
                        {
                            responses.push_back(consumer.post({buyerId, symbol->symbolId, OrderSide::Bid, OrderType::Limit,
                                TimeInForce::IOC, 100, 1, index + 1}));
                        }

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
        const bool balancesMatch = seller->queryActiveBalance(1).value_or(0) == 0 && seller->queryLockedBalance(1).value_or(0) == 0 &&
            seller->queryActiveBalance(2).value_or(0) == static_cast<Balance>(options.orders * 100) &&
            buyer->queryActiveBalance(1).value_or(0) == static_cast<Balance>(options.orders) &&
            buyer->queryActiveBalance(2).value_or(0) == 0 && buyer->queryLockedBalance(2).value_or(0) == 0;

        if (accepted != options.orders || filled != options.orders || trades != options.orders || consumer.bestAsk(symbol->symbolId) || !balancesMatch)
        {
            throw std::runtime_error("benchmark results did not match the expected fills");
        }

        const double elapsed = secondsBetween(matchingStart, matchingEnd);
        std::cout << "scenario=preloaded_asks_then_limit_bids\n";
        std::cout << "orders=" << options.orders << " producers=" << options.producers << " batch_size=" << options.batchSize;
        std::cout << " ring_size=" << options.ringSize << '\n';
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
