#include "advmatch/consuming/SingleThreadConsumer.h"
#include "advmatch/matching/MatchingEngine.h"
#include "advmatch/matching/contracts/ContractEngine.h"
#include "advmatch/util/ContractMath.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
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
    using namespace advmatch;
    using Clock = std::chrono::steady_clock;
    constexpr Price entryPrice = 100'000;
    constexpr AssetId collateral = 2;

    double peakResidentMiB() noexcept
    {
#ifdef _WIN32
        PROCESS_MEMORY_COUNTERS counters{};
        if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)))
        {
            return counters.PeakWorkingSetSize / (1024.0 * 1024.0);
        }
#endif
        return 0;
    }

    void require(bool condition, const char* message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    struct Options
    {
        std::uint64_t users = 20'000;
        std::uint64_t positionsPerUser = 4;
        std::uint64_t symbols = 8;
        std::uint64_t waves = 8;
        std::uint64_t crossPercent = 50;
        std::uint64_t shockPercent = 35;
        std::uint64_t impactPercent = 30;
        std::uint64_t depthPercent = 10;
        std::uint64_t levels = 8;
        std::uint64_t slippageBps = 100;
        std::uint64_t insurancePercent = 10;
        std::uint64_t restingPerUser = 1;
        std::uint64_t candidateBudget = std::numeric_limits<std::size_t>::max();
        int ringSize = 32;
        bool up = false;
        bool recover = true;
        OrderBookImplementation book = OrderBookImplementation::Radix;
    };

    Options parse(int argc, char** argv)
    {
        Options options;
        for (int index = 1; index < argc; ++index)
        {
            const std::string_view name(argv[index]);
            if (name == "--help")
            {
                std::cout << "advmatch_liquidation_benchmark\n"
                    << "  --users N --positions-per-user N --symbols N --waves N\n"
                    << "  --cross-percent N --shock-percent N --impact-percent N\n"
                    << "  --depth-percent N --levels N --slippage-bps N --insurance-percent N\n"
                    << "  --resting-per-user N --ring-size N --direction down|up\n"
                    << "  --recover 0|1 --book radix|tree --candidate-budget N\n";
                std::exit(0);
            }

            require(index + 1 < argc, "option requires a value");
            const std::string_view value(argv[++index]);
            if (name == "--direction")
            {
                require(value == "down" || value == "up", "direction must be down or up");
                options.up = value == "up";
                continue;
            }

            if (name == "--book")
            {
                require(value == "radix" || value == "tree", "book must be radix or tree");
                options.book = value == "radix" ? OrderBookImplementation::Radix : OrderBookImplementation::IndexedTree;
                continue;
            }

            std::uint64_t number = 0;
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), number);
            require(parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size(), "invalid numeric option");
            if (name == "--users")
            {
                options.users = number;
            }
            else if (name == "--positions-per-user")
            {
                options.positionsPerUser = number;
            }
            else if (name == "--symbols")
            {
                options.symbols = number;
            }
            else if (name == "--waves")
            {
                options.waves = number;
            }
            else if (name == "--cross-percent")
            {
                options.crossPercent = number;
            }
            else if (name == "--shock-percent")
            {
                options.shockPercent = number;
            }
            else if (name == "--impact-percent")
            {
                options.impactPercent = number;
            }
            else if (name == "--depth-percent")
            {
                options.depthPercent = number;
            }
            else if (name == "--levels")
            {
                options.levels = number;
            }
            else if (name == "--slippage-bps")
            {
                options.slippageBps = number;
            }
            else if (name == "--insurance-percent")
            {
                options.insurancePercent = number;
            }
            else if (name == "--resting-per-user")
            {
                options.restingPerUser = number;
            }
            else if (name == "--candidate-budget")
            {
                options.candidateBudget = number;
            }
            else if (name == "--ring-size")
            {
                require(number <= 1'048'576, "ring size too large");
                options.ringSize = static_cast<int>(number);
            }
            else if (name == "--recover")
            {
                require(number <= 1, "recover must be 0 or 1");
                options.recover = number == 1;
            }
            else
            {
                throw std::runtime_error("unknown option: " + std::string(name));
            }
        }

        require(options.users > 0 && options.users <= 10'000'000 && options.positionsPerUser > 0 &&
            options.positionsPerUser <= 20'000'000 / options.users, "victim positions must be between 1 and 20 million");
        require(options.symbols > 0 && options.symbols <= 100'000 && options.symbols <= options.users * options.positionsPerUser,
            "symbols must be populated and no more than 100000");
        require(options.waves > 0 && options.waves <= 1000 && options.crossPercent <= 100 && options.shockPercent <= 90 &&
            options.impactPercent <= 90 && options.shockPercent + options.impactPercent <= 95 && options.depthPercent <= 100 &&
            options.levels > 0 && options.levels <= 1000 && options.slippageBps <= 2000 && options.insurancePercent <= 200 &&
            options.restingPerUser <= 100 && options.ringSize > 0 && (options.ringSize & (options.ringSize - 1)) == 0,
            "invalid scenario range or non-power-of-two ring size");
        return options;
    }

    struct Events final : ContractEventSink
    {
        std::uint64_t liquidatedPositions = 0;
        std::uint64_t liquidatedQuantity = 0;
        WideBalance insuranceUsed = 0;

        void onEvent(const ContractEvent& event) noexcept override
        {
            if (event.type == ContractEventType::Liquidation)
            {
                ++this->liquidatedPositions;
                this->liquidatedQuantity += event.quantity;
                this->insuranceUsed += event.insurance;
            }
        }
    };

    enum class CommandType : std::uint8_t
    {
        Mark,
        Refill,
        FundInsurance,
        Process
    };

    struct Command
    {
        CommandType type;
        SymbolId symbolId;
        Price price;
        Quantity quantity;
        Timestamp timestamp;
    };

    struct Response
    {
        ContractState state = ContractState::Success;
        std::uint64_t liquidations = 0;
        std::uint64_t engineNanoseconds = 0;
    };

    class Consumer final : public consuming::SingleThreadConsumer<Command, Response>
    {
        UserManager& users;
        const Options& options;
        MatchingEngine matching;
        Events events;
        UserId maker = 0;
        UserId insurer = 0;
        std::vector<PositionId> makerPositions;
        std::vector<OrderId> liquidityOrders;
        std::vector<Quantity> seededQuantity;
        TradeBuffer trades;

        Response consume(Command&& command) override
        {
            const auto start = Clock::now();
            const auto before = this->events.liquidatedPositions;
            auto& contracts = this->matching.getContractEngine();
            ContractResult result;
            if (command.type == CommandType::Mark)
            {
                result = contracts.updateMarkPrice(command.symbolId, command.price, command.timestamp);
            }
            else if (command.type == CommandType::FundInsurance)
            {
                result = contracts.fundInsurance(command.symbolId, this->users.getUser(this->insurer)->getContractAccount(), command.quantity);
            }
            else if (command.type == CommandType::Process)
            {
                result = contracts.processLiquidations(command.symbolId, command.timestamp);
            }
            else
            {
                const auto slot = command.symbolId - 1;
                for (OrderId id : this->liquidityOrders)
                {
                    static_cast<void>(this->matching.cancelOrder(this->maker, id, command.timestamp));
                }

                this->liquidityOrders.clear();
                const Position* position = contracts.queryPosition(this->makerPositions[slot]);
                Quantity remaining = std::min(command.quantity, position->positionSize);
                ContractOrderOptions closing;
                closing.positionId = position->positionId;
                closing.reduceOnly = true;
                closing.leverage = position->leverage;
                for (std::uint64_t level = 0; level < this->options.levels && remaining != 0; ++level)
                {
                    const Quantity quantity = (remaining + this->options.levels - level - 1) / (this->options.levels - level);
                    const auto slip = this->options.slippageBps * (level + 1) / this->options.levels;
                    const Price price = command.price * (this->options.up ? 10'000 + slip : 10'000 - slip) / 10'000;
                    const auto order = this->matching.placeContractOrder(this->maker, command.symbolId,
                        this->options.up ? OrderSide::Ask : OrderSide::Bid, OrderType::Limit, TimeInForce::GTC,
                        price, quantity, command.timestamp, closing, this->trades);
                    require(static_cast<bool>(order), "liquidation liquidity order rejected");
                    this->liquidityOrders.push_back(order.orderId);
                    remaining -= quantity;
                }
            }

            require(result.state == ContractState::Success || result.state == ContractState::PendingLiquidation,
                "shock command rejected unexpectedly");
            return {result.state, this->events.liquidatedPositions - before,
                static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count())};
        }

    public:
        Consumer(UserManager& users, SymbolManager& symbols, const Options& options)
            : users(users), options(options), matching(users, symbols)
        {
            this->matching.getContractEngine().setEventSink(&this->events);
            this->matching.getContractEngine().setAutomaticLiquidationBudget(options.candidateBudget);
        }

        ~Consumer() override
        {
            this->stopBeforeDerivedDestruction();
        }

        void setup(SymbolManager& symbols, WideBalance& initialCash)
        {
            const Quantity victims = this->options.users * this->options.positionsPerUser;
            this->matching.reserve(static_cast<std::size_t>(victims * 4), this->options.symbols, victims);
            this->matching.getContractEngine().reserve(this->options.symbols, victims + this->options.symbols);
            this->makerPositions.resize(this->options.symbols);
            this->seededQuantity.resize(this->options.symbols);
            this->liquidityOrders.reserve(this->options.levels);
            const auto deposit = [&](UserId id, WideBalance amount)
            {
                const auto checked = ContractMath::balance(amount);
                require(checked && static_cast<bool>(this->users.getUser(id)->getContractAccount().updateBalance(collateral, *checked)), "setup deposit overflow");
                initialCash += *checked;
            };
            this->maker = this->users.createUser();
            this->insurer = this->users.createUser();
            deposit(this->maker, static_cast<WideBalance>(victims) * entryPrice * 3);
            deposit(this->insurer, static_cast<WideBalance>(victims) * entryPrice * 3);
            ContractSpec spec;
            spec.makerFeeRate = 0;
            spec.takerFeeRate = 0;
            spec.liquidationFeeRate = 0;
            spec.priceBandRate = 300'000;
            spec.firstFundingTimestamp = 1'000'000'000;
            ContractOrderOptions makerOptions;
            makerOptions.leverage = 1;
            for (std::uint64_t slot = 0; slot < this->options.symbols; ++slot)
            {
                const auto* symbol = symbols.createSymbol("CASCADE-" + std::to_string(slot + 1), SymbolType::Perpetual, static_cast<AssetId>(slot + 3), collateral);
                require(symbol != nullptr, "symbol creation failed");
                require(static_cast<bool>(this->matching.addContractBook(symbol->symbolId, spec, entryPrice, 0, this->options.book)), "contract registration failed");
                require(this->matching.reservePriceLevels(symbol->symbolId, this->options.levels + 2, this->options.levels + 2), "price reserve failed");
                const Quantity quantity = victims / this->options.symbols + (slot < victims % this->options.symbols);
                this->seededQuantity[slot] = quantity;
                const auto order = this->matching.placeContractOrder(this->maker, symbol->symbolId,
                    this->options.up ? OrderSide::Bid : OrderSide::Ask, OrderType::Limit, TimeInForce::GTC,
                    entryPrice, quantity, 1, makerOptions, this->trades);
                require(static_cast<bool>(order), "initial maker order failed");
                this->makerPositions[slot] = order.positionId;
                const auto insurance = static_cast<PositiveBalanceChange>(quantity * entryPrice * this->options.insurancePercent / 100);
                require(static_cast<bool>(this->matching.getContractEngine().fundInsurance(symbol->symbolId,
                    this->users.getUser(this->insurer)->getContractAccount(), insurance)), "initial insurance funding failed");
            }

            constexpr std::array<LeverageMultiplier, 5> leverages{5, 10, 20, 40, 80};
            for (std::uint64_t userIndex = 0; userIndex < this->options.users; ++userIndex)
            {
                const UserId user = this->users.createUser();
                ContractOrderOptions victim;
                victim.leverage = leverages[userIndex % leverages.size()];
                victim.marginMode = userIndex % 100 < this->options.crossPercent ? MarginMode::Cross : MarginMode::Isolated;
                deposit(user, static_cast<WideBalance>(ContractMath::margin(entryPrice * 13 / 10, victim.leverage)) * this->options.positionsPerUser);
                for (std::uint64_t index = 0; index < this->options.positionsPerUser; ++index)
                {
                    const SymbolId symbol = static_cast<SymbolId>((userIndex * this->options.positionsPerUser + index) % this->options.symbols + 1);
                    const auto order = this->matching.placeContractOrder(user, symbol, this->options.up ? OrderSide::Ask : OrderSide::Bid,
                        OrderType::Limit, TimeInForce::IOC, entryPrice, 1, 1, victim, this->trades);
                    require(order && order.state.status == OrderStatus::Filled, "victim position setup failed");
                    if (index == 0)
                    {
                        ContractOrderOptions close = victim;
                        close.positionId = order.positionId;
                        close.reduceOnly = true;
                        for (std::uint64_t resting = 0; resting < this->options.restingPerUser; ++resting)
                        {
                            const Price price = this->options.up ? entryPrice * 9 / 10 - resting : entryPrice * 11 / 10 + resting;
                            const auto parked = this->matching.placeContractOrder(user, symbol, this->options.up ? OrderSide::Bid : OrderSide::Ask,
                                OrderType::Limit, TimeInForce::GTC, price, 1, 1, close, this->trades);
                            require(static_cast<bool>(parked), "resting victim order setup failed");
                        }
                    }
                }
            }

            // The counterparty is a fully collateralized stress fixture. Add
            // isolated margin so an upward shock cannot liquidate the fixture
            // itself, including the configured execution slippage.
            for (std::size_t index = 0; index < this->makerPositions.size(); ++index)
            {
                require(static_cast<bool>(this->matching.getContractEngine().addMargin(this->makerPositions[index], this->maker,
                    static_cast<SymbolId>(index + 1), static_cast<BalanceChange>(this->seededQuantity[index] * entryPrice))), "maker extra margin failed");
            }
        }

        // Read only when no command is in flight, or after stop(). The ticket
        // completion provides synchronization with the consumer's last write.
        const MatchingEngine& engine() const noexcept
        {
            return this->matching;
        }

        const Events& stats() const noexcept
        {
            return this->events;
        }

        Quantity seeded(std::size_t slot) const noexcept
        {
            return this->seededQuantity[slot];
        }
    };

    std::size_t pending(const MatchingEngine& matching, std::uint64_t symbols)
    {
        std::size_t result = 0;
        for (SymbolId symbol = 1; symbol <= symbols; ++symbol)
        {
            result += matching.getContractEngine().findMarket(symbol)->pendingLiquidations.size();
        }

        return result;
    }

    void validate(const UserManager& users, const MatchingEngine& matching, const Options& options,
        WideBalance initialCash, const Events& events)
    {
        WideBalance cash = 0;
        std::vector<WideBalance> netQuantity(options.symbols);
        std::uint64_t closedVictims = 0;
        users.forEachUser([&](UserId userId, const User& user)
        {
            const auto& account = user.getContractAccount();
            account.forEachBalance([&](AssetId, const AssetBalance& balance)
            {
                require(balance.active >= 0 && balance.reserved >= 0, "negative balance");
                cash += static_cast<WideBalance>(balance.active) + balance.reserved;
            });
            account.forEachPosition([&](PositionId, const Position& position)
            {
                require(position.closed == (position.positionSize == 0) && position.initialMargin >= 0, "invalid position state");
                closedVictims += userId > 2 && position.closed;
                netQuantity[position.symbolId - 1] += position.positionSide == PositionSide::Long
                    ? static_cast<WideBalance>(position.positionSize) : -static_cast<WideBalance>(position.positionSize);
            });
        });
        for (SymbolId symbol = 1; symbol <= options.symbols; ++symbol)
        {
            const auto* market = matching.getContractEngine().findMarket(symbol);
            cash += static_cast<WideBalance>(market->insuranceBalance) + market->feeBalance + market->clearingBalance + market->fundingRoundingBalance;
            require(market->insuranceBalance >= 0 && netQuantity[symbol - 1] == 0, "insurance or position conservation failed");
        }

        require(cash == initialCash, "cash conservation failed");
        require(closedVictims == events.liquidatedPositions && events.liquidatedPositions == events.liquidatedQuantity,
            "liquidation event count does not match closed victim positions");
        require(!options.recover || pending(matching, options.symbols) == 0, "recovery left pending liquidations");
    }

    void run(const Options& options)
    {
        const Quantity victims = options.users * options.positionsPerUser;
        std::cout << "scenario=synthetic_liquidation_cascade direction=" << (options.up ? "up" : "down")
            << " book=" << (options.book == OrderBookImplementation::Radix ? "radix" : "tree")
            << " users=" << options.users << " victim_positions=" << victims << " symbols=" << options.symbols
            << " cross_percent=" << options.crossPercent << " ring_size=" << options.ringSize
            << " candidate_budget=" << options.candidateBudget << std::endl;
        UserManager users;
        SymbolManager symbols;
        users.reserve(options.users + 2);
        symbols.reserve(options.symbols);
        Consumer consumer(users, symbols, options);
        WideBalance initialCash = 0;
        const auto setupStart = Clock::now();
        consumer.setup(symbols, initialCash);
        const double setupSeconds = std::chrono::duration<double>(Clock::now() - setupStart).count();
        consumer.start(options.ringSize);
        consuming::ResponseSlot<Response> slot;
        std::uint64_t markNanoseconds = 0;
        std::uint64_t refillNanoseconds = 0;
        std::vector<double> waveMilliseconds;
        waveMilliseconds.reserve(options.waves);
        const auto dispatch = [&](Command command)
        {
            const Response response = consumer.post(command, slot).get();
            if (command.type == CommandType::Mark)
            {
                markNanoseconds += response.engineNanoseconds;
            }
            else
            {
                refillNanoseconds += response.engineNanoseconds;
            }
        };
        std::uint64_t peakPending = 0;
        Timestamp timestamp = 2;
        const auto stressStart = Clock::now();
        for (std::uint64_t wave = 1; wave <= options.waves; ++wave, ++timestamp)
        {
            const auto before = consumer.stats().liquidatedPositions;
            const auto waveStart = Clock::now();
            const auto external = options.shockPercent * 10'000 * wave / options.waves;
            const auto feedback = options.impactPercent * 10'000 * consumer.stats().liquidatedQuantity / victims;
            const auto movement = external + feedback;
            const Price price = entryPrice * (options.up ? RATE_SCALE + movement : RATE_SCALE - movement) / RATE_SCALE;
            for (SymbolId symbol = 1; symbol <= options.symbols; ++symbol)
            {
                dispatch({CommandType::Mark, symbol, price, 0, timestamp});
            }

            const auto beforeRefill = pending(consumer.engine(), options.symbols);
            peakPending = std::max<std::uint64_t>(peakPending, beforeRefill);
            for (SymbolId symbol = 1; symbol <= options.symbols; ++symbol)
            {
                const Quantity depth = (consumer.seeded(symbol - 1) * options.depthPercent + 99) / 100;
                dispatch({CommandType::Refill, symbol, price, depth, timestamp});
            }

            const auto remaining = pending(consumer.engine(), options.symbols);
            const double elapsed = std::chrono::duration<double>(Clock::now() - waveStart).count();
            waveMilliseconds.push_back(elapsed * 1000);
            std::cout << "wave=" << wave << " mark=" << price << " external_move_ppm=" << external << " feedback_move_ppm=" << feedback
                << " liquidated=" << consumer.stats().liquidatedPositions - before << " pending_before_refill=" << beforeRefill
                << " pending_after_refill=" << remaining << " wave_ms=" << elapsed * 1000 << std::endl;
        }

        const double stressSeconds = std::chrono::duration<double>(Clock::now() - stressStart).count();
        const auto stressLiquidations = consumer.stats().liquidatedPositions;
        const auto pendingBeforeRecovery = pending(consumer.engine(), options.symbols);
        const auto recoveryStart = Clock::now();
        if (options.recover)
        {
            for (SymbolId symbol = 1; symbol <= options.symbols; ++symbol)
            {
                dispatch({CommandType::FundInsurance, symbol, 0, consumer.seeded(symbol - 1) * entryPrice, timestamp});
            }

            for (int pass = 0; pass < 3; ++pass, ++timestamp)
            {
                for (SymbolId symbol = 1; symbol <= options.symbols; ++symbol)
                {
                    const Price price = consumer.engine().getContractEngine().findMarket(symbol)->markPrice;
                    dispatch({CommandType::Refill, symbol, price, consumer.seeded(symbol - 1), timestamp});
                    dispatch({CommandType::Process, symbol, price, 0, timestamp});
                }
            }
        }

        const double recoverySeconds = std::chrono::duration<double>(Clock::now() - recoveryStart).count();
        consumer.stop();
        const auto auditStart = Clock::now();
        validate(users, consumer.engine(), options, initialCash, consumer.stats());
        std::sort(waveMilliseconds.begin(), waveMilliseconds.end());
        const double auditSeconds = std::chrono::duration<double>(Clock::now() - auditStart).count();
        std::cout << std::fixed << std::setprecision(3)
            << "setup_seconds=" << setupSeconds << " stress_seconds=" << stressSeconds << " recovery_seconds=" << recoverySeconds
            << " audit_seconds=" << auditSeconds << '\n'
            << "stress_liquidated_positions=" << stressLiquidations << " total_liquidated_positions=" << consumer.stats().liquidatedPositions
            << " peak_pending=" << peakPending << " pending_before_recovery=" << pendingBeforeRecovery
            << " pending_final=" << pending(consumer.engine(), options.symbols) << '\n'
            << "stress_liquidation_tps=" << stressLiquidations / stressSeconds
            << " total_liquidation_tps=" << consumer.stats().liquidatedPositions / (stressSeconds + recoverySeconds)
            << " mark_engine_ms=" << markNanoseconds / 1'000'000.0 << " maintenance_engine_ms=" << refillNanoseconds / 1'000'000.0 << '\n'
            << "wave_p50_ms=" << waveMilliseconds[waveMilliseconds.size() / 2]
            << " wave_max_ms=" << waveMilliseconds.back()
            << " insurance_used=" << static_cast<std::int64_t>(consumer.stats().insuranceUsed) << '\n'
            << "peak_resident_mib=" << peakResidentMiB() << '\n'
            << "validation=passed" << std::endl;
    }
}

int main(int argc, char** argv)
{
    try
    {
        run(parse(argc, argv));
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "liquidation benchmark failed: " << error.what() << std::endl;
        return 1;
    }
}
