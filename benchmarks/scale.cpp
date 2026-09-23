#include "advmatch/consuming/SingleThreadConsumer.h"
#include "advmatch/matching/MatchingEngine.h"

#include <algorithm>
#include <barrier>
#include <bit>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#endif

namespace
{
    using Clock = std::chrono::steady_clock;

    struct Options
    {
        std::uint64_t users = 3000;
        std::uint64_t accounts = 10000;
        std::uint32_t symbols = 100;
        std::uint64_t pendingOrders = 4000;
        std::uint64_t cycles = 1000;
        std::uint32_t rounds = 2;
        std::uint32_t producers = 4;
        std::uint32_t batchSize = 64;
        int ringSize = 1024;
        bool force = false;
    };

    struct PlaceCommand
    {
        advmatch::UserId userId;
        advmatch::SymbolId symbolId;
        advmatch::OrderSide side;
        advmatch::TimeInForce timeInForce;
        Clock::time_point submittedAt;
    };

    struct PlaceResponse
    {
        bool accepted;
        advmatch::OrderStatus status;
        std::size_t tradeCount;
        std::uint64_t latencyNanos;
    };

    class ScaleConsumer final : public advmatch::consuming::SingleThreadConsumer<PlaceCommand, PlaceResponse>
    {
        advmatch::MatchingEngine engine;
        advmatch::TradeBuffer trades;

    protected:
        PlaceResponse consume(PlaceCommand&& command) override
        {
            auto result = this->engine.placeOrder(command.userId, command.symbolId, command.side, advmatch::OrderType::Limit,
                command.timeInForce, 100, 1, 0, this->trades);
            const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - command.submittedAt).count();
            return {static_cast<bool>(result), result.state.status, this->trades.size(), static_cast<std::uint64_t>(elapsed)};
        }

    public:
        ScaleConsumer(advmatch::UserManager& users, advmatch::SymbolManager& symbols)
            : engine(users, symbols)
        {}

        ~ScaleConsumer() override
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

    struct PendingResponse
    {
        advmatch::consuming::RequestTicket<PlaceResponse> response;
        bool shouldFill;
    };

    struct ProducerStats
    {
        std::uint64_t buys = 0;
        std::uint64_t asks = 0;
        std::vector<std::uint64_t> latencyNanos;
        std::exception_ptr error;
    };

    struct MemoryStats
    {
        std::uint64_t privateBytes = 0;
        std::uint64_t workingSetBytes = 0;
        std::uint64_t peakWorkingSetBytes = 0;
    };

    MemoryStats processMemory()
    {
#ifdef _WIN32
        PROCESS_MEMORY_COUNTERS_EX counters{};
        if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters)))
        {
            return {static_cast<std::uint64_t>(counters.PrivateUsage), static_cast<std::uint64_t>(counters.WorkingSetSize),
                static_cast<std::uint64_t>(counters.PeakWorkingSetSize)};
        }
#else
        std::ifstream status("/proc/self/status");
        std::string key;
        std::uint64_t amount = 0;
        MemoryStats result;
        while (status >> key >> amount)
        {
            if (key == "VmRSS:")
            {
                result.workingSetBytes = amount * 1024;
            }
            else if (key == "VmHWM:")
            {
                result.peakWorkingSetBytes = amount * 1024;
            }

            status.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        }

        return result;
#endif
        return {};
    }

    std::uint64_t availablePhysicalMemory()
    {
#ifdef _WIN32
        MEMORYSTATUSEX status{};
        status.dwLength = sizeof(status);
        return GlobalMemoryStatusEx(&status) ? status.ullAvailPhys : 0;
#else
        std::ifstream memory("/proc/meminfo");
        std::string key;
        std::uint64_t amount = 0;
        while (memory >> key >> amount)
        {
            if (key == "MemAvailable:")
            {
                return amount * 1024;
            }

            memory.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        }

        return 0;
#endif
    }

    double megabytes(std::uint64_t bytes)
    {
        return static_cast<double>(bytes) / (1024.0 * 1024.0);
    }

    std::uint64_t parsePositive(std::string_view value)
    {
        std::uint64_t result = 0;
        const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
        if (error != std::errc{} || end != value.data() + value.size() || result == 0)
        {
            throw std::invalid_argument("option values must be positive integers");
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
                std::cout << "Usage: advmatch_scale_benchmark [--profile target] [--users N] [--accounts N] [--symbols N]";
                std::cout << " [--pending-orders N] [--cycles N] [--rounds N] [--producers N]";
                std::cout << " [--batch-size N] [--ring-size N] [--force]\n";
                std::exit(0);
            }

            if (name == "--force")
            {
                options.force = true;
                continue;
            }

            if (++index == argumentCount)
            {
                throw std::invalid_argument("missing value for option");
            }

            const std::string_view value(arguments[index]);
            if (name == "--profile")
            {
                if (value != "target")
                {
                    throw std::invalid_argument("unknown profile");
                }

                options.users = 3000000;
                options.accounts = 10000000;
                options.symbols = 100000;
                options.pendingOrders = 4000000;
                options.cycles = 100000;
                options.rounds = 3;
                options.batchSize = 256;
                continue;
            }

            const std::uint64_t number = parsePositive(value);
            if (name == "--users")
            {
                options.users = number;
            }
            else if (name == "--accounts")
            {
                options.accounts = number;
            }
            else if (name == "--symbols" && number <= std::numeric_limits<std::uint32_t>::max() - 2)
            {
                options.symbols = static_cast<std::uint32_t>(number);
            }
            else if (name == "--pending-orders")
            {
                options.pendingOrders = number;
            }
            else if (name == "--cycles")
            {
                options.cycles = number;
            }
            else if (name == "--rounds" && number <= std::numeric_limits<std::uint32_t>::max())
            {
                options.rounds = static_cast<std::uint32_t>(number);
            }
            else if (name == "--producers" && number <= std::numeric_limits<std::uint32_t>::max())
            {
                options.producers = static_cast<std::uint32_t>(number);
            }
            else if (name == "--batch-size" && number <= std::numeric_limits<std::uint32_t>::max())
            {
                options.batchSize = static_cast<std::uint32_t>(number);
            }
            else if (name == "--ring-size" && number <= static_cast<std::uint64_t>(std::numeric_limits<int>::max()))
            {
                options.ringSize = static_cast<int>(number);
            }
            else
            {
                throw std::invalid_argument("unknown option or value out of range");
            }
        }

        if (options.symbols < 2 || options.users < options.symbols || options.pendingOrders < options.symbols ||
            options.pendingOrders > options.users * options.symbols || options.accounts < options.users + options.pendingOrders + options.symbols)
        {
            throw std::invalid_argument("need symbols >= 2, users >= symbols, pending orders within symbol capacity, "
                "and accounts >= users + pending orders + symbols");
        }

        if (!std::has_single_bit(static_cast<unsigned int>(options.ringSize)) || options.producers > 256 || options.batchSize > 100000)
        {
            throw std::invalid_argument("ring size must be a power of two; producers and batch size must be bounded");
        }

        if (options.rounds > std::numeric_limits<std::uint64_t>::max() / options.cycles)
        {
            throw std::invalid_argument("total cycles overflow");
        }

        const std::uint64_t maxCyclesPerSymbol = ((options.cycles * options.rounds) + options.symbols - 1) / options.symbols;
        if (maxCyclesPerSymbol > static_cast<std::uint64_t>(std::numeric_limits<advmatch::Balance>::max()) / 1000)
        {
            throw std::invalid_argument("balance range exceeded");
        }

        return options;
    }

    std::uint64_t assetBalanceCount(const advmatch::UserManager& users)
    {
        std::uint64_t count = 0;
        for (const auto& [userId, user] : users)
        {
            count += user.getAssetBalanceCount();
        }

        return count;
    }

    std::uint64_t pendingOrderCount(const ScaleConsumer& consumer, std::uint32_t symbolCount)
    {
        std::uint64_t count = 0;
        for (std::uint32_t symbol = 0; symbol < symbolCount; ++symbol)
        {
            auto quote = consumer.bestAsk(symbol + 1);
            if (quote)
            {
                count += quote->orderCount;
            }
        }

        return count;
    }

    double percentile(std::vector<std::uint64_t>& values, std::uint64_t numerator, std::uint64_t denominator)
    {
        const std::size_t index = static_cast<std::size_t>((((values.size() * numerator) + denominator - 1) / denominator) - 1);
        return static_cast<double>(values[index]) / 1000.0;
    }

    void runRound(ScaleConsumer& consumer, const Options& options, std::uint32_t round)
    {
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
                    const std::uint64_t baseCount = options.cycles / options.producers;
                    const std::uint64_t first = (baseCount * producer) + std::min<std::uint64_t>(producer, options.cycles % options.producers);
                    const std::uint64_t count = (options.cycles / options.producers) + (producer < options.cycles % options.producers ? 1 : 0);
                    const std::size_t responseSlotCount = static_cast<std::size_t>(options.batchSize) * 2;
                    auto responseSlots = std::make_unique<advmatch::consuming::ResponseSlot<PlaceResponse>[]>(responseSlotCount);
                    std::vector<PendingResponse> pending;
                    pending.reserve(responseSlotCount);
                    stats[producer].latencyNanos.reserve(static_cast<std::size_t>(count) * 2);

                    for (std::uint64_t index = 0; index < count;)
                    {
                        pending.clear();
                        for (std::uint32_t batch = 0; batch < options.batchSize && index < count; ++batch, ++index)
                        {
                            const std::uint32_t symbol = static_cast<std::uint32_t>((first + index) % options.symbols);
                            const advmatch::UserId buyer = ((symbol + 1) % options.users) + 1;
                            const advmatch::UserId seller = symbol + 1;
                            const std::size_t responseIndex = static_cast<std::size_t>(batch) * 2;

                            auto buy = consumer.post({buyer, symbol + 1, advmatch::OrderSide::Bid, advmatch::TimeInForce::IOC, Clock::now()},
                                responseSlots[responseIndex]);
                            pending.push_back({std::move(buy), true});
                            auto ask = consumer.post({seller, symbol + 1, advmatch::OrderSide::Ask, advmatch::TimeInForce::GTC, Clock::now()},
                                responseSlots[responseIndex + 1]);
                            pending.push_back({std::move(ask), false});
                        }

                        for (auto& item : pending)
                        {
                            const PlaceResponse outcome = item.response.get();
                            if (!outcome.accepted || (item.shouldFill && (outcome.status != advmatch::OrderStatus::Filled || outcome.tradeCount != 1)) ||
                                (!item.shouldFill && (outcome.status != advmatch::OrderStatus::NotFilled || outcome.tradeCount != 0)))
                            {
                                throw std::runtime_error("mixed workload order failed");
                            }

                            if (item.shouldFill)
                            {
                                ++stats[producer].buys;
                            }
                            else
                            {
                                ++stats[producer].asks;
                            }

                            stats[producer].latencyNanos.push_back(outcome.latencyNanos);
                        }
                    }
                }
                catch (...)
                {
                    stats[producer].error = std::current_exception();
                }
            });
        }

        const auto started = Clock::now();
        startGate.arrive_and_wait();
        for (auto& producer : producers)
        {
            producer.join();
        }

        const double elapsed = std::chrono::duration<double>(Clock::now() - started).count();
        std::uint64_t buys = 0;
        std::uint64_t asks = 0;
        std::vector<std::uint64_t> latencies;
        latencies.reserve(static_cast<std::size_t>(options.cycles) * 2);
        for (ProducerStats& producer : stats)
        {
            if (producer.error)
            {
                std::rethrow_exception(producer.error);
            }

            buys += producer.buys;
            asks += producer.asks;
            latencies.insert(latencies.end(), producer.latencyNanos.begin(), producer.latencyNanos.end());
        }

        if (buys != options.cycles || asks != options.cycles || latencies.size() != options.cycles * 2)
        {
            throw std::runtime_error("round did not complete all commands");
        }

        std::sort(latencies.begin(), latencies.end());
        const MemoryStats memory = processMemory();
        std::cout << "round=" << round << " trades=" << buys << " commands=" << buys + asks;
        std::cout << " seconds=" << elapsed << " trade_tps=" << static_cast<double>(buys) / elapsed;
        std::cout << " command_tps=" << static_cast<double>(buys + asks) / elapsed << '\n';
        std::cout << "latency_us_p50=" << percentile(latencies, 50, 100);
        std::cout << " p99=" << percentile(latencies, 99, 100);
        std::cout << " p99_9=" << percentile(latencies, 999, 1000);
        std::cout << " max=" << static_cast<double>(latencies.back()) / 1000.0 << '\n';
        std::cout << "private_mb=" << megabytes(memory.privateBytes) << " working_set_mb=" << megabytes(memory.workingSetBytes);
        std::cout << " peak_working_set_mb=" << megabytes(memory.peakWorkingSetBytes) << '\n' << std::flush;
    }
}

int main(int argumentCount, char** arguments)
{
    try
    {
        using namespace advmatch;

        const Options options = parseOptions(argumentCount, arguments);
        const std::uint64_t estimatedBytes = (options.users * (sizeof(User) + 64)) +
            (options.accounts * sizeof(AssetBalanceEntry)) + (static_cast<std::uint64_t>(options.symbols) * 640) +
            (options.pendingOrders * 360) + ((options.cycles * options.rounds) * 512);
        const std::uint64_t availableBytes = availablePhysicalMemory();
        std::cout << "users=" << options.users << " asset_balance_records=" << options.accounts;
        std::cout << " symbols=" << options.symbols << " pending_orders=" << options.pendingOrders << '\n';
        std::cout << "cycles_per_round=" << options.cycles << " rounds=" << options.rounds;
        std::cout << " producers=" << options.producers << " batch_size=" << options.batchSize << " ring_size=" << options.ringSize << '\n';
        std::cout << "estimated_memory_mb=" << megabytes(estimatedBytes) << " available_physical_mb=" << megabytes(availableBytes) << '\n' << std::flush;

        if (!options.force && availableBytes != 0 && estimatedBytes > availableBytes)
        {
            throw std::runtime_error("insufficient free physical memory for this profile; retry on a larger host or use --force");
        }

        const auto setupStart = Clock::now();
        UserManager users;
        SymbolManager symbols;
        users.reserve(options.users);
        symbols.reserve(options.symbols);

        for (std::uint32_t index = 0; index < options.symbols; ++index)
        {
            if (!symbols.createSymbol("BASE" + std::to_string(index) + "/QUOTE", SymbolType::Spot, index + 2, 1))
            {
                throw std::runtime_error("symbol registration failed");
            }
        }

        std::cout << "setup_phase=symbols_done\n" << std::flush;

        const std::uint64_t totalCycles = options.cycles * options.rounds;
        const BalanceChange quoteCredit = static_cast<BalanceChange>(
            (((totalCycles + options.symbols - 1) / options.symbols) * 100) + 100);
        for (std::uint64_t index = 0; index < options.users; ++index)
        {
            const UserId userId = users.createUser();
            if (userId != index + 1 || !users.getUser(userId)->updateBalance(1, quoteCredit))
            {
                throw std::runtime_error("user setup failed");
            }
        }

        std::cout << "setup_phase=users_done\n" << std::flush;

        for (std::uint64_t index = 0; index < options.pendingOrders; ++index)
        {
            const std::uint64_t owner = index % options.users;
            const std::uint32_t symbol = static_cast<std::uint32_t>(((owner % options.symbols) + (index / options.users)) % options.symbols);
            if (!users.getUser(owner + 1)->updateBalance(symbol + 2, 2))
            {
                throw std::runtime_error("maker balance setup failed");
            }
        }

        for (std::uint32_t symbol = 0; symbol < options.symbols; ++symbol)
        {
            const UserId buyer = ((symbol + 1) % options.users) + 1;
            if (!users.getUser(buyer)->updateBalance(symbol + 2, 1))
            {
                throw std::runtime_error("buyer asset balance setup failed");
            }
        }

        const std::uint64_t fillerCount = options.accounts - options.users - options.pendingOrders - options.symbols;
        for (std::uint64_t index = 0; index < fillerCount; ++index)
        {
            const std::uint64_t assetId = static_cast<std::uint64_t>(options.symbols) + 2 + (index / options.users);
            if (assetId > std::numeric_limits<AssetId>::max() || !users.getUser((index % options.users) + 1)->updateBalance(static_cast<AssetId>(assetId), 1))
            {
                throw std::runtime_error("account proxy setup failed");
            }
        }

        const BalanceChange replenishmentCredit = static_cast<BalanceChange>(
            (totalCycles + options.symbols - 1) / options.symbols);
        for (std::uint32_t symbol = 0; symbol < options.symbols; ++symbol)
        {
            if (!users.getUser(symbol + 1)->updateBalance(symbol + 2, replenishmentCredit))
            {
                throw std::runtime_error("replenishment balance setup failed");
            }
        }

        if (users.getUserCount() != options.users || symbols.getSymbolCount() != options.symbols || assetBalanceCount(users) != options.accounts)
        {
            throw std::runtime_error("setup counts do not match requested scale");
        }

        std::cout << "setup_phase=balances_done\n" << std::flush;

        ScaleConsumer consumer(users, symbols);
        consumer.reserve(options.pendingOrders + (totalCycles * 2), options.symbols, options.pendingOrders);
        for (std::uint32_t symbol = 0; symbol < options.symbols; ++symbol)
        {
            if (!consumer.addSpotBook(symbol + 1))
            {
                throw std::runtime_error("order book setup failed");
            }
        }

        std::cout << "setup_phase=books_done\n" << std::flush;

        consumer.start(options.ringSize);
        auto pendingSlots = std::make_unique<consuming::ResponseSlot<PlaceResponse>[]>(options.batchSize);
        std::vector<consuming::RequestTicket<PlaceResponse>> pending;
        pending.reserve(options.batchSize);
        std::uint64_t nextProgress = 1000000;
        for (std::uint64_t index = 0; index < options.pendingOrders;)
        {
            pending.clear();
            for (std::uint32_t batch = 0; batch < options.batchSize && index < options.pendingOrders; ++batch, ++index)
            {
                const std::uint64_t owner = index % options.users;
                const std::uint32_t symbol = static_cast<std::uint32_t>(((owner % options.symbols) + (index / options.users)) % options.symbols);
                pending.push_back(consumer.post({owner + 1, symbol + 1, OrderSide::Ask, TimeInForce::GTC, Clock::now()},
                    pendingSlots[batch]));
            }

            for (auto& response : pending)
            {
                const PlaceResponse outcome = response.get();
                if (!outcome.accepted || outcome.status != OrderStatus::NotFilled || outcome.tradeCount != 0)
                {
                    throw std::runtime_error("pending order preload failed");
                }
            }

            if (index >= nextProgress)
            {
                std::cout << "preloaded=" << index << '\n' << std::flush;
                nextProgress += 1000000;
            }
        }

        if (pendingOrderCount(consumer, options.symbols) != options.pendingOrders)
        {
            throw std::runtime_error("pending order count differs from target");
        }

        std::cout << "setup_seconds=" << std::chrono::duration<double>(Clock::now() - setupStart).count() << '\n';
        const MemoryStats before = processMemory();
        std::cout << "before_private_mb=" << megabytes(before.privateBytes);
        std::cout << " before_working_set_mb=" << megabytes(before.workingSetBytes) << '\n' << std::flush;

        for (std::uint32_t round = 1; round <= options.rounds; ++round)
        {
            runRound(consumer, options, round);
        }

        consumer.stop();
        const std::uint64_t finalPending = pendingOrderCount(consumer, options.symbols);
        const std::uint64_t finalAccounts = assetBalanceCount(users);
        if (finalPending != options.pendingOrders || finalAccounts != options.accounts)
        {
            std::cerr << "final_pending=" << finalPending << " final_asset_balance_records=" << finalAccounts << '\n';
            throw std::runtime_error("scale state changed unexpectedly after workload");
        }

        const MemoryStats after = processMemory();
        std::cout << "verified_users=" << users.getUserCount() << " verified_accounts=" << finalAccounts;
        std::cout << " verified_symbols=" << symbols.getSymbolCount() << " verified_pending=" << finalPending << '\n';
        std::cout << "retained_order_records=" << options.pendingOrders + (totalCycles * 2) << '\n';
        const double privateGrowth = megabytes(after.privateBytes) - megabytes(before.privateBytes);
        std::cout << "private_growth_mb=" << privateGrowth;
        std::cout << " private_growth_mb_per_million_trades=" << privateGrowth * 1000000.0 / static_cast<double>(totalCycles) << '\n';
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "Scale benchmark failed: " << error.what() << '\n';
        return 1;
    }
}
