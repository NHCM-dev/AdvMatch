#include "advmatch/consuming/SingleThreadConsumer.h"
#include "advmatch/matching/MatchingEngine.h"
#include "advmatch/matching/contracts/ContractEngine.h"

#include <algorithm>
#include <barrier>
#include <bit>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace
{
    using namespace advmatch;
    using Clock = std::chrono::steady_clock;

    constexpr Price executionPrice = 100'000;
    constexpr AssetId settlementAsset = 2;
    constexpr Timestamp tradingTimestamp = 1;
    constexpr Timestamp fundingTimestamp = 1'000;
    constexpr std::int32_t fundingRate = 1'000;

    struct Options
    {
        std::uint64_t orders = 100'000;
        std::uint64_t warmup = 1'000;
        std::uint32_t users = 1'024;
        std::uint32_t symbols = 1;
        std::uint32_t producers = 4;
        std::uint32_t batchSize = 256;
        std::uint32_t publishBatch = 0;
        std::uint64_t latencySample = 1'024;
        int ringSize = 32;
        MarginMode marginMode = MarginMode::Isolated;
        LeverageMultiplier leverage = 10;
        MMRRate makerFeeRate = 100;
        MMRRate takerFeeRate = 500;
    };

    enum class CommandType
    {
        Place,
        Mark,
        Funding
    };

    struct Command
    {
        CommandType type = CommandType::Place;
        UserId userId = 0;
        SymbolId symbolId = 0;
        OrderSide side = OrderSide::Ask;
        TimeInForce timeInForce = TimeInForce::GTC;
        // A zero time point disables timing on unsampled orders.
        Clock::time_point submittedAt{};
    };

    struct Response
    {
        bool accepted = false;
        bool filled = false;
        std::size_t trades = 0;
        std::size_t affectedPositions = 0;
        int state = 0;
        std::int64_t latencyNanoseconds = 0;
    };

    class EventCounter final : public ContractEventSink
    {
    public:
        std::uint64_t fills = 0;
        std::uint64_t fundings = 0;
        std::uint64_t liquidations = 0;

        void onEvent(const ContractEvent& event) noexcept override
        {
            if (event.type == ContractEventType::Fill)
            {
                ++this->fills;
            }
            else if (event.type == ContractEventType::Funding)
            {
                ++this->fundings;
            }
            else if (event.type == ContractEventType::Liquidation)
            {
                ++this->liquidations;
            }
        }
    };

    class BenchmarkConsumer final : public consuming::SingleThreadConsumer<Command, Response>
    {
        // The sink outlives the matching engine, including teardown.
        EventCounter events;
        MatchingEngine matching;
        TradeBuffer trades;
        ContractOrderOptions orderOptions;

    protected:
        Response consume(Command&& command) override
        {
            Response response;
            if (command.type == CommandType::Place)
            {
                const auto result = this->matching.placeContractOrder(command.userId, command.symbolId, command.side,
                    OrderType::Limit, command.timeInForce, executionPrice, 1, tradingTimestamp, this->orderOptions, this->trades);
                response.accepted = static_cast<bool>(result);
                response.filled = result.state.status == OrderStatus::Filled;
                response.trades = this->trades.size();
                response.state = result.rejectReason ? static_cast<int>(*result.rejectReason) : -1;
            }
            else
            {
                auto& contracts = this->matching.getContractEngine();
                const auto result = command.type == CommandType::Mark
                    ? contracts.updateMarkPrice(command.symbolId, executionPrice, tradingTimestamp + 1)
                    : contracts.settleFunding(command.symbolId, fundingRate, fundingTimestamp);
                response.accepted = static_cast<bool>(result);
                response.affectedPositions = result.affectedPositions;
                response.state = static_cast<int>(result.state);
            }

            if (command.submittedAt != Clock::time_point{})
            {
                response.latencyNanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - command.submittedAt).count();
            }

            return response;
        }

    public:
        BenchmarkConsumer(UserManager& users, SymbolManager& symbols, const Options& options)
            : matching(users, symbols)
        {
            this->orderOptions.marginMode = options.marginMode;
            this->orderOptions.leverage = options.leverage;
            this->matching.reserve(static_cast<std::size_t>(options.orders * 2), options.symbols, options.orders);
            this->matching.getContractEngine().reserve(options.symbols, static_cast<std::size_t>(options.orders * 2));
            this->matching.getContractEngine().setEventSink(&this->events);

            ContractSpec spec;
            spec.firstFundingTimestamp = fundingTimestamp;
            spec.defaultLeverage = options.leverage;
            spec.makerFeeRate = options.makerFeeRate;
            spec.takerFeeRate = options.takerFeeRate;
            for (SymbolId symbolId = 1; symbolId <= options.symbols; ++symbolId)
            {
                if (!this->matching.addContractBook(symbolId, spec, executionPrice, 0))
                {
                    throw std::runtime_error("perpetual book registration failed");
                }
            }
        }

        ~BenchmarkConsumer() override
        {
            this->stopBeforeDerivedDestruction();
        }

        // Inspect only before start() or after stop(). Running state is owned
        // exclusively by consume(), including marks and funding settlements.
        const MatchingEngine& getMatching() const noexcept
        {
            return this->matching;
        }

        const EventCounter& getEvents() const noexcept
        {
            return this->events;
        }
    };

    struct ProducerContext
    {
        std::uint64_t offset = 0;
        std::uint64_t count = 0;
        std::uint64_t accepted = 0;
        std::uint64_t filled = 0;
        std::uint64_t trades = 0;
        int firstReject = -1;
        std::exception_ptr error;
        std::unique_ptr<consuming::ResponseSlot<Response>[]> slots;
        std::vector<Command> commands;
        std::vector<consuming::RequestTicket<Response>> tickets;
        std::vector<std::int64_t> latencies;

        void prepare(const Options& options, std::uint64_t offset, std::uint64_t count)
        {
            this->offset = offset;
            this->count = count;
            this->slots = std::make_unique<consuming::ResponseSlot<Response>[]>(options.batchSize);
            this->commands.reserve(options.batchSize);
            this->tickets.reserve(options.batchSize);
            if (options.latencySample != 0)
            {
                this->latencies.reserve(static_cast<std::size_t>(count / options.latencySample + 2));
            }
        }
    };

    std::uint64_t parseInteger(std::string_view value)
    {
        std::uint64_t result = 0;
        const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
        if (error != std::errc{} || end != value.data() + value.size())
        {
            throw std::invalid_argument("expected a non-negative integer: " + std::string(value));
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
                std::cout << "Usage: advmatch_perpetual_benchmark [--orders N] [--warmup N] [--users N] [--symbols N]\n"
                    << "  [--producers N] [--ring-size N] [--batch-size N] [--publish-batch N]\n"
                    << "  [--margin-mode isolated|cross] [--leverage N] [--maker-fee-rate N] [--taker-fee-rate N]\n"
                    << "  [--latency-sample N] (0 disables sampling; fees use a scale of 1,000,000)\n";
                std::exit(0);
            }

            if (++index == argumentCount)
            {
                throw std::invalid_argument("missing value for " + std::string(name));
            }

            const std::string_view value(arguments[index]);
            if (name == "--margin-mode")
            {
                if (value != "isolated" && value != "cross")
                {
                    throw std::invalid_argument("margin mode must be isolated or cross");
                }

                options.marginMode = value == "cross" ? MarginMode::Cross : MarginMode::Isolated;
                continue;
            }

            const auto number = parseInteger(value);
            if (name == "--orders")
            {
                options.orders = number;
            }
            else if (name == "--warmup")
            {
                options.warmup = number;
            }
            else if (name == "--latency-sample")
            {
                options.latencySample = number;
            }
            else if (name == "--users" && number <= std::numeric_limits<std::uint32_t>::max())
            {
                options.users = static_cast<std::uint32_t>(number);
            }
            else if (name == "--symbols" && number <= std::numeric_limits<AssetId>::max() - 2)
            {
                options.symbols = static_cast<std::uint32_t>(number);
            }
            else if (name == "--producers" && number <= std::numeric_limits<std::uint32_t>::max())
            {
                options.producers = static_cast<std::uint32_t>(number);
            }
            else if (name == "--batch-size" && number <= std::numeric_limits<std::uint32_t>::max())
            {
                options.batchSize = static_cast<std::uint32_t>(number);
            }
            else if (name == "--publish-batch" && number <= std::numeric_limits<std::uint32_t>::max())
            {
                options.publishBatch = static_cast<std::uint32_t>(number);
            }
            else if (name == "--ring-size" && number <= static_cast<std::uint64_t>(std::numeric_limits<int>::max()))
            {
                options.ringSize = static_cast<int>(number);
            }
            else if (name == "--leverage" && number <= 100)
            {
                options.leverage = static_cast<LeverageMultiplier>(number);
            }
            else if (name == "--maker-fee-rate" && number <= RATE_SCALE)
            {
                options.makerFeeRate = static_cast<MMRRate>(number);
            }
            else if (name == "--taker-fee-rate" && number <= RATE_SCALE)
            {
                options.takerFeeRate = static_cast<MMRRate>(number);
            }
            else
            {
                throw std::invalid_argument("unknown option or value out of range: " + std::string(name));
            }
        }

        const auto maximumOrders = std::min(static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max() / 2),
            static_cast<std::uint64_t>(std::numeric_limits<Balance>::max() / executionPrice / 4));
        if (options.orders == 0 || options.orders > maximumOrders || options.warmup > maximumOrders ||
            options.users < 2 || options.users % 2 != 0 || options.symbols == 0 || options.symbols > options.orders ||
            options.producers == 0 || options.producers > options.orders || options.batchSize == 0 || options.leverage == 0)
        {
            throw std::invalid_argument("orders/users/symbols/producers/batch/leverage out of range; users must be even and >= 2");
        }

        if (!std::has_single_bit(static_cast<unsigned int>(options.ringSize)) ||
            options.publishBatch > static_cast<std::uint32_t>(options.ringSize))
        {
            throw std::invalid_argument("ring size must be a power of two; publish batch must fit the ring");
        }

        if (options.publishBatch == 0)
        {
            options.publishBatch = std::max(1U, std::min({64U, options.batchSize,
                static_cast<std::uint32_t>(options.ringSize) / options.producers}));
        }

        return options;
    }

    bool shouldSample(std::uint64_t index, std::uint64_t interval) noexcept
    {
        if (interval == 0)
        {
            return false;
        }

        // One deterministic, scattered sample per block prevents sampling
        // only the first command of every batch or the same user/product.
        std::uint64_t mixed = index / interval + 0x9e3779b97f4a7c15ULL;
        mixed = (mixed ^ (mixed >> 30)) * 0xbf58476d1ce4e5b9ULL;
        mixed = (mixed ^ (mixed >> 27)) * 0x94d049bb133111ebULL;
        mixed ^= mixed >> 31;
        return index % interval == mixed % interval;
    }

    Command makeOrder(const Options& options, std::uint64_t index, bool maker, bool sample)
    {
        // Disjoint user groups prevent self trades even when producers execute
        // out of order. Global indices give preload/takers identical workloads.
        const auto pair = index % (options.users / 2);
        Command command;
        command.userId = pair * 2 + (maker ? 1 : 2);
        command.symbolId = static_cast<SymbolId>(index % options.symbols + 1);
        command.side = maker ? OrderSide::Ask : OrderSide::Bid;
        command.timeInForce = maker ? TimeInForce::GTC : TimeInForce::IOC;
        if (sample && shouldSample(index, options.latencySample))
        {
            command.submittedAt = Clock::now();
        }

        return command;
    }

    void postOrders(BenchmarkConsumer& consumer, const Options& options, ProducerContext& context, bool maker)
    {
        for (std::uint64_t index = 0; index < context.count;)
        {
            context.commands.clear();
            context.tickets.clear();
            for (std::uint32_t batchIndex = 0; batchIndex < options.batchSize && index < context.count; ++batchIndex, ++index)
            {
                context.commands.push_back(makeOrder(options, context.offset + index, maker, !maker));
            }

            consumer.postBatch(context.commands, {context.slots.get(), context.commands.size()}, context.tickets, options.publishBatch);
            for (auto& ticket : context.tickets)
            {
                const auto response = ticket.get();
                context.accepted += response.accepted;
                context.filled += response.filled;
                context.trades += response.trades;
                if (!response.accepted && context.firstReject == -1)
                {
                    context.firstReject = response.state;
                }

                if (response.latencyNanoseconds != 0)
                {
                    context.latencies.push_back(response.latencyNanoseconds);
                }
            }
        }
    }

    double secondsBetween(Clock::time_point first, Clock::time_point last)
    {
        return std::chrono::duration<double>(last - first).count();
    }

    double percentileMicroseconds(const std::vector<std::int64_t>& sorted, std::size_t percentile)
    {
        if (sorted.empty())
        {
            return 0;
        }

        const std::size_t rank = (sorted.size() / 100) * percentile + ((sorted.size() % 100) * percentile + 99) / 100;
        return static_cast<double>(sorted[rank - 1]) / 1'000;
    }

    void verify(const Options& options, const UserManager& users, const BenchmarkConsumer& consumer, Balance deposit)
    {
        const auto& matching = consumer.getMatching();
        WideBalance cash = 0;
        WideBalance longQuantity = 0;
        WideBalance shortQuantity = 0;
        WideBalance netFunding = 0;
        WideBalance positionFees = 0;
        std::uint64_t positions = 0;
        bool positionsValid = true;
        users.forEachUser([&](UserId, const User& user)
        {
            const auto& account = user.getContractAccount();
            WideBalance accountCash = 0;
            WideBalance accountFunding = 0;
            WideBalance accountFees = 0;
            WideBalance accountMargin = 0;
            WideBalance accountMaintenance = 0;
            WideBalance accountCloseFees = 0;
            account.forEachBalance([&](AssetId, const AssetBalance& balance)
            {
                accountCash += static_cast<WideBalance>(balance.active) + balance.reserved;
                positionsValid = positionsValid && balance.active >= 0 && balance.reserved >= 0;
            });
            account.forEachPosition([&](PositionId, const Position& position)
            {
                ++positions;
                positionsValid = positionsValid && !position.closed && !position.liquidating && position.positionSize == 1 &&
                    position.entryNotional == static_cast<Balance>(executionPrice) && position.marginMode == options.marginMode &&
                    position.leverage == options.leverage;
                if (position.positionSide == PositionSide::Long)
                {
                    longQuantity += position.positionSize;
                }
                else
                {
                    shortQuantity += position.positionSize;
                }

                netFunding += position.fundingPnL;
                positionFees += position.paidFees;
                accountFunding += position.fundingPnL;
                accountFees += position.paidFees;
                accountMargin += position.initialMargin;
                accountMaintenance += position.maintenanceMargin;
                accountCloseFees += position.estimatedCloseFee;
                const Balance expectedFunding = static_cast<Balance>(executionPrice * fundingRate / RATE_SCALE);
                const BalanceChange signedFunding = position.positionSide == PositionSide::Long ? -expectedFunding : expectedFunding;
                const Balance baseMargin = static_cast<Balance>(executionPrice / options.leverage + (executionPrice % options.leverage != 0));
                const Balance expectedMargin = baseMargin + (options.marginMode == MarginMode::Isolated ? signedFunding : 0);
                positionsValid = positionsValid && position.fundingPnL == signedFunding && position.initialMargin == expectedMargin &&
                    position.updatedUnrealizedPnl == 0 && position.realizedPnL == 0;
            });

            cash += accountCash;
            const auto expectedReserved = options.marginMode == MarginMode::Isolated ? accountMargin : WideBalance(0);
            positionsValid = positionsValid && accountCash == static_cast<WideBalance>(deposit) - accountFees + accountFunding &&
                account.queryReservedBalance(settlementAsset).value_or(0) == expectedReserved;
            if (options.marginMode == MarginMode::Cross)
            {
                const auto* cross = account.findCrossMargin(settlementAsset);
                positionsValid = positionsValid && (cross != nullptr || accountMargin == 0);
                if (cross)
                {
                    positionsValid = positionsValid && cross->initialMargin == accountMargin && cross->maintenanceMargin == accountMaintenance &&
                        cross->liquidationFeeReserve == accountCloseFees && cross->unrealizedPnL == 0 && cross->orderReservation == 0 && !cross->liquidating;
                }
            }
        });

        WideBalance marketFees = 0;
        std::size_t activePositions = 0;
        for (SymbolId symbolId = 1; symbolId <= options.symbols; ++symbolId)
        {
            const auto* market = matching.getContractEngine().findMarket(symbolId);
            if (!market || matching.bestAsk(symbolId) || matching.bestBid(symbolId) || !market->pendingLiquidations.empty() ||
                !market->orderParticipants.empty() || market->lastFundingTimestamp != fundingTimestamp)
            {
                throw std::runtime_error("book/funding/liquidation state validation failed");
            }

            cash += static_cast<WideBalance>(market->feeBalance) + market->insuranceBalance + market->clearingBalance + market->fundingRoundingBalance;
            marketFees += market->feeBalance;
            activePositions += market->activePositions.size();
        }

        const WideBalance feePerFill = (executionPrice * options.makerFeeRate + RATE_SCALE - 1) / RATE_SCALE +
            (executionPrice * options.takerFeeRate + RATE_SCALE - 1) / RATE_SCALE;
        const auto& events = consumer.getEvents();
        if (cash != static_cast<WideBalance>(deposit) * options.users || longQuantity != options.orders || shortQuantity != options.orders ||
            positions != options.orders * 2 || activePositions != positions || !positionsValid || netFunding != 0 ||
            marketFees != feePerFill * options.orders || positionFees != marketFees || events.fills != positions ||
            events.fundings != positions || events.liquidations != 0)
        {
            throw std::runtime_error("positions, fees, events or cash conservation validation failed");
        }
    }

    void runBenchmark(const Options& options, bool report)
    {
        const auto setupStart = Clock::now();
        UserManager users;
        SymbolManager symbols;
        users.reserve(options.users);
        symbols.reserve(options.symbols);
        // Generous real collateral avoids measuring balance rejects. Both fee
        // rates may be 100%, and the reserve uses the upper price band. Three
        // notional units cover collateral, fees and that safety allowance.
        const auto ordersPerPair = options.orders / (options.users / 2) + (options.orders % (options.users / 2) != 0);
        const Balance deposit = static_cast<Balance>((ordersPerPair * 3 + 2) * executionPrice);
        for (std::uint32_t index = 0; index < options.users; ++index)
        {
            const auto userId = users.createUser();
            if (!users.getUser(userId)->getContractAccount().updateBalance(settlementAsset, deposit))
            {
                throw std::runtime_error("contract account deposit failed");
            }
        }

        for (std::uint32_t index = 0; index < options.symbols; ++index)
        {
            const AssetId baseAsset = index + 3;
            if (!symbols.createSymbol("PERP-" + std::to_string(index + 1), SymbolType::Perpetual, baseAsset, settlementAsset))
            {
                throw std::runtime_error("perpetual symbol creation failed");
            }
        }

        // Slots outlive the consumer even on exceptions with pending tickets.
        ProducerContext preload;
        preload.prepare(options, 0, options.orders);
        std::vector<ProducerContext> contexts(options.producers);
        std::uint64_t offset = 0;
        for (std::uint32_t index = 0; index < options.producers; ++index)
        {
            const auto count = options.orders / options.producers + (index < options.orders % options.producers ? 1 : 0);
            contexts[index].prepare(options, offset, count);
            offset += count;
        }

        consuming::ResponseSlot<Response> lifecycleSlot;
        BenchmarkConsumer consumer(users, symbols, options);
        consumer.start(options.ringSize);
        const auto preloadStart = Clock::now();
        postOrders(consumer, options, preload, true);
        const auto preloadEnd = Clock::now();
        if (preload.accepted != options.orders || preload.filled != 0 || preload.trades != 0)
        {
            throw std::runtime_error("preload failed; PlaceOrderState=" + std::to_string(preload.firstReject));
        }

        std::barrier startGate(static_cast<std::ptrdiff_t>(options.producers) + 1);
        std::vector<std::jthread> producers;
        producers.reserve(options.producers);
        try
        {
            for (std::uint32_t index = 0; index < options.producers; ++index)
            {
                producers.emplace_back([&, index]
                {
                    startGate.arrive_and_wait();
                    try
                    {
                        postOrders(consumer, options, contexts[index], false);
                    }
                    catch (...)
                    {
                        contexts[index].error = std::current_exception();
                    }
                });
            }
        }
        catch (...)
        {
            // Release successfully started threads if thread creation fails.
            for (std::size_t index = producers.size(); index < options.producers; ++index)
            {
                startGate.arrive_and_drop();
            }

            startGate.arrive_and_wait();
            for (auto& producer : producers)
            {
                producer.join();
            }

            consumer.stop();
            throw;
        }

        const auto matchingStart = Clock::now();
        startGate.arrive_and_wait();
        for (auto& producer : producers)
        {
            producer.join();
        }

        const auto matchingEnd = Clock::now();
        // The main producer posts lifecycle commands through the same consumer.
        const auto markStart = Clock::now();
        for (SymbolId symbolId = 1; symbolId <= options.symbols; ++symbolId)
        {
            const auto response = consumer.post({CommandType::Mark, 0, symbolId}, lifecycleSlot).get();
            if (!response.accepted)
            {
                throw std::runtime_error("mark update failed; ContractState=" + std::to_string(response.state));
            }
        }

        const auto markEnd = Clock::now();
        std::uint64_t fundedPositions = 0;
        for (SymbolId symbolId = 1; symbolId <= options.symbols; ++symbolId)
        {
            const auto response = consumer.post({CommandType::Funding, 0, symbolId}, lifecycleSlot).get();
            if (!response.accepted)
            {
                throw std::runtime_error("funding settlement failed; ContractState=" + std::to_string(response.state));
            }

            fundedPositions += response.affectedPositions;
        }

        const auto fundingEnd = Clock::now();
        consumer.stop();

        std::uint64_t accepted = 0;
        std::uint64_t filled = 0;
        std::uint64_t trades = 0;
        std::vector<std::int64_t> latencies;
        for (const auto& context : contexts)
        {
            if (context.error)
            {
                std::rethrow_exception(context.error);
            }

            accepted += context.accepted;
            filled += context.filled;
            trades += context.trades;
            latencies.insert(latencies.end(), context.latencies.begin(), context.latencies.end());
        }

        if (accepted != options.orders || filled != options.orders || trades != options.orders || fundedPositions != options.orders * 2)
        {
            throw std::runtime_error("unexpected accepted/fill/trade/funding counts");
        }

        verify(options, users, consumer, deposit);
        if (report)
        {
            std::sort(latencies.begin(), latencies.end());
            const double elapsed = secondsBetween(matchingStart, matchingEnd);
            const double fundingSeconds = secondsBetween(markEnd, fundingEnd);
            std::cout << std::fixed << std::setprecision(3)
                << "scenario=perpetual_preloaded_asks_then_limit_bids\n"
                << "logical_threads=" << std::thread::hardware_concurrency() << " users=" << options.users << " symbols=" << options.symbols << '\n'
                << "orders=" << options.orders << " preload_orders=" << options.orders << " positions=" << options.orders * 2 << '\n'
                << "producers=" << options.producers << " ring_size=" << options.ringSize << " batch_size=" << options.batchSize << " publish_batch=" << options.publishBatch << '\n'
                << "margin_mode=" << (options.marginMode == MarginMode::Cross ? "cross" : "isolated") << " leverage=" << options.leverage
                << " maker_fee_rate=" << options.makerFeeRate << " taker_fee_rate=" << options.takerFeeRate << '\n'
                << "setup_seconds=" << secondsBetween(setupStart, preloadStart) << " preload_seconds=" << secondsBetween(preloadStart, preloadEnd) << '\n'
                << "accepted=" << accepted << " rejected=" << options.orders - accepted << " filled=" << filled << " trades=" << trades << '\n'
                << "elapsed_seconds=" << elapsed << " command_tps=" << static_cast<double>(accepted) / elapsed
                << " trade_tps=" << static_cast<double>(trades) / elapsed << '\n'
                << "latency_sample_interval=" << options.latencySample << " latency_samples=" << latencies.size()
                << " match_latency_p50_us=" << percentileMicroseconds(latencies, 50)
                << " match_latency_p95_us=" << percentileMicroseconds(latencies, 95)
                << " match_latency_p99_us=" << percentileMicroseconds(latencies, 99)
                << " match_latency_max_us=" << (latencies.empty() ? 0 : static_cast<double>(latencies.back()) / 1'000) << '\n'
                << "mark_update_seconds=" << secondsBetween(markStart, markEnd) << '\n'
                << "funding_rate=" << fundingRate << " funded_positions=" << fundedPositions << " funding_seconds=" << fundingSeconds
                << " funding_positions_per_second=" << static_cast<double>(fundedPositions) / fundingSeconds << '\n'
                << "fill_events=" << consumer.getEvents().fills << " funding_events=" << consumer.getEvents().fundings << '\n'
                << "validation=passed\n";
        }
    }
}

int main(int argumentCount, char** arguments)
{
    try
    {
        const auto options = parseOptions(argumentCount, arguments);
        std::cout << "perpetual_benchmark starting orders=" << options.orders << " warmup_orders=" << options.warmup << std::endl;
        if (options.warmup != 0)
        {
            auto warmupOptions = options;
            warmupOptions.orders = options.warmup;
            warmupOptions.users = static_cast<std::uint32_t>(std::min<std::uint64_t>(options.users, options.warmup * 2));
            warmupOptions.symbols = static_cast<std::uint32_t>(std::min<std::uint64_t>(options.symbols, options.warmup));
            warmupOptions.producers = static_cast<std::uint32_t>(std::min<std::uint64_t>(options.producers, options.warmup));
            warmupOptions.latencySample = 0;
            runBenchmark(warmupOptions, false);
        }

        runBenchmark(options, true);
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "Perpetual benchmark failed: " << error.what() << '\n';
        return 1;
    }
}
