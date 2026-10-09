#include "advmatch/persistence/PersistentTradingEngine.h"
#include "advmatch/persistence/FilePersistenceStore.h"
#include "advmatch/persistence/CommandCodec.h"
#include "advmatch/matching/contracts/ContractEngine.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <chrono>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

#define CHECK(condition) \
    do \
    { \
        if (!(condition)) \
        { \
            std::fprintf(stderr, "Check failed at line %d: %s\n", __LINE__, #condition); \
            std::abort(); \
        } \
    } while (false)

namespace
{
    using namespace advmatch;

    struct TemporaryDirectory
    {
        std::filesystem::path path;

        explicit TemporaryDirectory(const std::string& name)
            : path(std::filesystem::temp_directory_path() / ("advmatch-persistence-" + name + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())))
        {
            std::filesystem::create_directories(this->path);
        }

        ~TemporaryDirectory()
        {
            std::error_code error;
            std::filesystem::remove_all(this->path, error);
        }

        PersistenceConfig config() const
        {
            PersistenceConfig config;
            config.directory = this->path;
            return config;
        }
    };

    template <typename Function>
    void expectError(PersistenceFailure failure, Function function)
    {
        try
        {
            function();
            CHECK(false);
        }
        catch (const PersistenceError& error)
        {
            CHECK(error.reason() == failure);
        }
    }

    void prepare(PersistentTradingEngine& engine, SymbolType type = SymbolType::Spot)
    {
        CHECK(engine.execute(commands::CreateAsset{"Bitcoin", "BTC", 0}).result.entityId == 1);
        CHECK(engine.execute(commands::CreateAsset{"Dollar", "USD", 0}).result.entityId == 2);
        CHECK(engine.execute(commands::CreateUser{}).result.entityId == 1);
        CHECK(engine.execute(commands::CreateUser{}).result.entityId == 2);
        CHECK(engine.execute(commands::CreateUser{}).result.entityId == 3);
        CHECK(engine.execute(commands::CreateSymbol{"BTC-USD", type, 1, 2}).result.entityId == 1);
    }

    commands::PlaceOrder order(UserId userId, OrderSide side, Price price, Quantity quantity, Timestamp timestamp, std::optional<ContractOrderOptions> options = std::nullopt)
    {
        return {userId, 1, side, OrderType::Limit, TimeInForce::GTC, price, quantity, timestamp, options};
    }

    void spotRecovery()
    {
        TemporaryDirectory directory("spot");
        auto config = directory.config();
        config.retainedRequests = 3;
        OrderId first = 0;
        OrderId second = 0;
        CommandSequence final = 0;
        {
            PersistentTradingEngine engine(config);
            prepare(engine);
            CHECK(engine.execute(commands::AddSpotBook{1}).result);
            CHECK(engine.execute(commands::UpdateBalance{1, 1, 100}).result);
            CHECK(engine.execute(commands::UpdateBalance{3, 1, 100}).result);
            CHECK(engine.execute(commands::UpdateBalance{5, 2, 10'000}).result);
            first = engine.execute(order(1, OrderSide::Ask, 100, 10, 1), 100).result.entityId;
            second = engine.execute(order(2, OrderSide::Ask, 100, 10, 2), 101).result.entityId;
            CHECK(engine.execute(commands::ModifyQuantity{1, first, 1, 3}, 102).result);
            engine.checkpoint();
            CHECK(std::filesystem::file_size(directory.path / "journal.wal") == 0);
            CHECK(engine.execute(order(3, OrderSide::Bid, 100, 5, 4), 103).result);
            CHECK(engine.getTrades().size() == 1);
            CHECK(engine.getTrades()[0].makerOrderId == second);
            final = engine.lastSequence();
            CHECK(engine.durableThrough() == final);
            engine.close();
        }

        {
            PersistentTradingEngine engine(config);
            CHECK(engine.lastSequence() == final);
            CHECK(engine.getTrades().empty());
            CHECK(engine.getMatchingEngine().bestAsk(1)->quantity == 16);
            CHECK(engine.getMatchingEngine().queryOrder(second)->getFillingStatus().filled == 5);
            CHECK(engine.getUserManager().getUser(3)->getSpotAccount().queryActiveBalance(1) == 5);
            const auto duplicate = engine.execute(order(3, OrderSide::Bid, 100, 5, 4), 103);
            CHECK(duplicate.duplicate && duplicate.durable() && engine.lastSequence() == final);
            expectError(PersistenceFailure::RequestConflict, [&] { engine.execute(order(3, OrderSide::Bid, 100, 6, 4), 103); });
            expectError(PersistenceFailure::RequestExpired, [&] { engine.execute(order(1, OrderSide::Ask, 100, 10, 1), 100); });
            CHECK(!engine.isFaulted());
            std::vector<OrderId> makers;
            engine.visitCommitted(0, [&](const CommittedCommand& command)
            {
                for (const auto& trade : command.trades)
                {
                    makers.push_back(trade.makerOrderId);
                }
            });
            CHECK(makers == std::vector<OrderId>{second});
            CHECK(engine.execute(commands::CancelOrder{1, first, 5}, 104).result);
            engine.checkpoint();
            CHECK(engine.execute(commands::CreateAsset{"Third", "OTHER", 0}).result.entityId == 3);
            engine.checkpoint();
            std::size_t files = 0;
            for (const auto& entry : std::filesystem::directory_iterator(directory.path))
            {
                files += entry.path().extension() == ".bin";
            }

            CHECK(files == config.retainedCheckpoints);
            engine.close();
        }

        PersistentTradingEngine recovered(config);
        CHECK(recovered.getMatchingEngine().bestAsk(1)->quantity == 5);
        CHECK(recovered.getMatchingEngine().queryOrder(first)->getFillingStatus().status == OrderStatus::Cancelled);
        recovered.close();
    }

    struct Events : PersistentEventSink
    {
        std::size_t count = 0;
        void onCommitted(const CommandReceipt&, std::span<const Trade>, std::span<const ContractEvent> events) noexcept override
        {
            this->count += events.size();
        }
    };

    void contractRecovery(SymbolType type, MarginMode mode)
    {
        TemporaryDirectory directory("contract");
        auto config = directory.config();
        PositionId longId = 0;
        PositionId shortId = 0;
        ContractSpec spec;
        spec.makerFeeRate = 0;
        spec.takerFeeRate = 0;
        spec.liquidationFeeRate = 0;
        spec.fundingInterval = 100;
        spec.firstFundingTimestamp = type == SymbolType::Perpetual ? 100 : 0;
        spec.expiryTimestamp = type == SymbolType::Future ? 1000 : 0;
        Balance before = 0;
        {
            PersistentTradingEngine engine(config);
            Events events;
            engine.setEventSink(&events);
            prepare(engine, type);
            CHECK(engine.execute(commands::AddContractBook{1, spec, 100, 0}).result);
            for (AccountId id : {2, 4, 6})
            {
                CHECK(engine.execute(commands::UpdateBalance{id, 2, 10'000}).result);
            }

            ContractOrderOptions options;
            options.marginMode = mode;
            shortId = engine.execute(order(2, OrderSide::Ask, 100, 10, 1, options)).result.positionId;
            const auto bid = engine.execute(order(1, OrderSide::Bid, 100, 10, 2, options));
            CHECK(bid.result);
            longId = bid.result.positionId;
            CHECK(events.count == 2);
            CHECK(engine.execute(commands::SetLeverage{longId, 1, 1, 5, 3}).result);
            if (mode == MarginMode::Isolated)
            {
                CHECK(engine.execute(commands::AddMargin{longId, 1, 1, 100}).result);
            }

            CHECK(engine.execute(commands::FundInsurance{1, 6, 500}).result);
            CHECK(engine.execute(commands::UpdateMarkPrice{1, 105, 4}).result);
            engine.checkpoint();
            if (type == SymbolType::Perpetual)
            {
                CHECK(engine.execute(commands::SettleFunding{1, 10'000, 100}).result);
            }

            before = engine.getUserManager().getUser(1)->getContractAccount().queryActiveBalance(2).value_or(0);
            engine.close();
        }

        {
            PersistentTradingEngine engine(config);
            Events events;
            engine.setEventSink(&events);
            CHECK(events.count == 0);
            const auto& contracts = engine.getMatchingEngine().getContractEngine();
            const auto* position = contracts.queryPosition(longId);
            CHECK(position != nullptr && position->positionSize == 10 && position->leverage == 5 && position->marginMode == mode);
            CHECK(contracts.queryPosition(shortId)->positionSize == 10);
            CHECK(contracts.findMarket(1)->markPrice == 105 && contracts.findMarket(1)->insuranceBalance == 500);
            CHECK(engine.getUserManager().getUser(1)->getContractAccount().queryActiveBalance(2) == before);
            std::size_t recoveredEvents = 0;
            engine.visitCommitted(0, [&](const CommittedCommand& command) { recoveredEvents += command.contractEvents.size(); });
            CHECK(recoveredEvents >= 2);
            if (type == SymbolType::Future)
            {
                CHECK(engine.execute(commands::Expire{1, 1000}).result);
                CHECK(engine.execute(commands::Deliver{1, 105, 1000}).result);
            }
            else
            {
                CHECK(engine.execute(commands::SetAutomaticLiquidationBudget{0}).result);
                const auto update = engine.execute(commands::UpdateMarkPrice{1, 10, 101});
                CHECK(update.result.state == static_cast<std::uint8_t>(mode == MarginMode::Isolated ? ContractState::PendingLiquidation : ContractState::Success));
            }

            engine.checkpoint();
            engine.close();
        }

        PersistentTradingEngine engine(config);
        const auto* market = engine.getMatchingEngine().getContractEngine().findMarket(1);
        if (type == SymbolType::Future)
        {
            CHECK(market->status == ContractStatus::Settled);
            CHECK(engine.getMatchingEngine().getContractEngine().queryPosition(longId)->closed);
        }
        else
        {
            CHECK(market->markPrice == 10);
            CHECK(market->hasPendingLiquidation(longId) == (mode == MarginMode::Isolated));
            CHECK(engine.execute(commands::ProcessLiquidations{1, 102, true, 1}).result.state == static_cast<std::uint8_t>(ContractState::PendingLiquidation) || mode == MarginMode::Cross);
        }

        engine.close();
    }

    void flushPolicies()
    {
        TemporaryDirectory directory("flush");
        auto config = directory.config();
        config.flushPolicy = FlushPolicy::Batch;
        config.flushEveryCommands = 2;
        config.flushIntervalMilliseconds = 0;
        {
            PersistentTradingEngine engine(config);
            CHECK(!engine.execute(commands::CreateUser{}).durable());
            CHECK(engine.execute(commands::CreateUser{}).durable());
            CHECK(!engine.execute(commands::CreateUser{}).durable());
            engine.flush();
            CHECK(engine.durableThrough() == 3);
            engine.close();
        }

        config.flushPolicy = FlushPolicy::Manual;
        {
            PersistentTradingEngine engine(config);
            CHECK(engine.durableThrough() == 3);
            CHECK(!engine.execute(commands::CreateUser{}).durable());
            engine.close();
        }

        config.enabled = false;
        PersistentTradingEngine memory(config);
        CHECK(!memory.execute(commands::CreateUser{}).durable());
        CHECK(memory.lastSequence() == 1 && memory.durableThrough() == 0);
        memory.close();
    }

    void tailAndCorruption()
    {
        TemporaryDirectory directory("tail");
        auto config = directory.config();
        {
            PersistentTradingEngine engine(config);
            CHECK(engine.execute(commands::CreateUser{}).result.entityId == 1);
            expectError(PersistenceFailure::Locked, [&] { PersistentTradingEngine other(config); });
            engine.close();
        }

        const auto journal = directory.path / "journal.wal";
        const auto size = std::filesystem::file_size(journal);
        {
            std::ofstream file(journal, std::ios::binary | std::ios::app);
            file << "partial";
        }

        config.tailRecovery = TailRecovery::Reject;
        expectError(PersistenceFailure::CorruptData, [&] { PersistentTradingEngine engine(config); });
        config.tailRecovery = TailRecovery::TruncateIncomplete;
        {
            PersistentTradingEngine engine(config);
            CHECK(engine.lastSequence() == 1 && std::filesystem::file_size(journal) == size);
            engine.close();
        }

        // A fully written intent without its commit must execute once on restart.
        {
            FilePersistenceStore store(config);
            store.append({JournalRecordType::Intent, 2, encodeCommand(commands::CreateUser{}, 10)});
            store.sync();
        }

        {
            PersistentTradingEngine engine(config);
            CHECK(engine.getUserManager().getUserCount() == 2);
            CHECK(engine.execute(commands::CreateUser{}, 10).duplicate);
            CHECK(engine.lastSequence() == 2);
            engine.close();
        }

        {
            std::fstream file(journal, std::ios::binary | std::ios::in | std::ios::out);
            file.seekp(24);
            file.put('\x7f');
        }

        expectError(PersistenceFailure::CorruptData, [&] { PersistentTradingEngine engine(config); });
    }

    struct MemoryState
    {
        std::vector<JournalRecord> records;
        std::optional<StoredSnapshot> snapshot;
        JournalRecordType failAppend = static_cast<JournalRecordType>(0);
        bool failSync = false;
        bool failReset = false;
    };

    class MemoryStore : public PersistenceStore
    {
        std::shared_ptr<MemoryState> state;

    public:
        explicit MemoryStore(std::shared_ptr<MemoryState> state)
            : state(std::move(state))
        {
        }

        std::optional<StoredSnapshot> loadSnapshot() override
        {
            return this->state->snapshot;
        }

        void replay(const std::function<void(const JournalRecord&)>& visitor) override
        {
            for (const auto& record : this->state->records)
            {
                visitor(record);
            }
        }

        void append(const JournalRecord& record) override
        {
            if (record.type == this->state->failAppend)
            {
                throw PersistenceError(PersistenceFailure::Io, "injected append failure");
            }

            this->state->records.push_back(record);
        }

        void sync() override
        {
            if (this->state->failSync)
            {
                throw PersistenceError(PersistenceFailure::Io, "injected sync failure");
            }
        }

        void publishSnapshot(CommandSequence sequence, std::span<const std::byte> payload) override
        {
            this->state->snapshot = StoredSnapshot{sequence, {payload.begin(), payload.end()}};
        }

        void resetJournalThrough(CommandSequence) override
        {
            if (this->state->failReset)
            {
                throw PersistenceError(PersistenceFailure::Io, "injected truncation failure");
            }

            this->state->records.clear();
        }
    };

    void failureRecovery()
    {
        PersistenceConfig config;
        auto memory = std::make_shared<MemoryState>();
        {
            PersistentTradingEngine engine(config, std::make_unique<MemoryStore>(memory));
            memory->failAppend = JournalRecordType::Intent;
            expectError(PersistenceFailure::Io, [&] { engine.execute(commands::CreateUser{}); });
            CHECK(engine.isFaulted() && engine.getUserManager().getUserCount() == 0);
            expectError(PersistenceFailure::Faulted, [&] { engine.execute(commands::CreateUser{}); });
        }

        memory->failAppend = static_cast<JournalRecordType>(0);
        {
            PersistentTradingEngine engine(config, std::make_unique<MemoryStore>(memory));
            memory->failAppend = JournalRecordType::Commit;
            expectError(PersistenceFailure::Io, [&] { engine.execute(commands::CreateUser{}, 1); });
            CHECK(engine.isFaulted() && engine.getUserManager().getUserCount() == 1);
        }

        memory->failAppend = static_cast<JournalRecordType>(0);
        {
            PersistentTradingEngine engine(config, std::make_unique<MemoryStore>(memory));
            CHECK(engine.getUserManager().getUserCount() == 1);
            CHECK(engine.execute(commands::CreateUser{}, 1).duplicate);
            memory->failSync = true;
            expectError(PersistenceFailure::Io, [&] { engine.execute(commands::CreateUser{}, 2); });
            CHECK(engine.isFaulted());
        }

        memory->failSync = false;
        {
            PersistentTradingEngine engine(config, std::make_unique<MemoryStore>(memory));
            CHECK(engine.getUserManager().getUserCount() == 2);
            CHECK(engine.execute(commands::CreateUser{}, 2).duplicate);
            memory->failReset = true;
            expectError(PersistenceFailure::Io, [&] { engine.checkpoint(); });
            CHECK(engine.isFaulted() && memory->snapshot.has_value());
        }

        memory->failReset = false;
        {
            PersistentTradingEngine engine(config, std::make_unique<MemoryStore>(memory));
            CHECK(engine.getUserManager().getUserCount() == 2);
            CHECK(engine.execute(commands::CreateUser{}, 3).result.entityId == 3);
            engine.close();
        }

        memory->records.back().payload[4] ^= std::byte{1};
        expectError(PersistenceFailure::ReplayMismatch, [&] { PersistentTradingEngine engine(config, std::make_unique<MemoryStore>(memory)); });
    }

    void corruptSnapshot()
    {
        TemporaryDirectory directory("snapshot-corruption");
        auto config = directory.config();
        std::filesystem::path latest;
        {
            PersistentTradingEngine engine(config);
            CHECK(engine.execute(commands::CreateUser{}).result);
            engine.checkpoint();
            CHECK(engine.execute(commands::CreateUser{}).result);
            engine.checkpoint();
            latest = directory.path / "snapshot-2.bin";
            engine.close();
        }

        {
            std::fstream file(latest, std::ios::binary | std::ios::in | std::ios::out);
            file.seekp(28);
            file.put('\x01');
        }

        // Never fall back to snapshot-1 after discarding snapshot-2's WAL.
        expectError(PersistenceFailure::CorruptData, [&] { PersistentTradingEngine engine(config); });
    }

    void invalidJournal()
    {
        TemporaryDirectory directory("invalid-sequence");
        auto config = directory.config();
        {
            FilePersistenceStore store(config);
            store.append({JournalRecordType::Intent, 2, encodeCommand(commands::CreateUser{}, 0)});
            store.sync();
        }

        expectError(PersistenceFailure::CorruptData, [&] { PersistentTradingEngine engine(config); });
        TemporaryDirectory versionDirectory("invalid-version");
        config = versionDirectory.config();
        {
            FilePersistenceStore store(config);
            auto payload = encodeCommand(commands::CreateUser{}, 0);
            payload[0] = std::byte{2};
            store.append({JournalRecordType::Intent, 1, std::move(payload)});
            store.sync();
        }

        expectError(PersistenceFailure::UnsupportedVersion, [&] { PersistentTradingEngine engine(config); });
        config.retainedCheckpoints = 0;
        expectError(PersistenceFailure::InvalidConfiguration, [&] { PersistentTradingEngine engine(config); });
    }

    void partialCommitAndArchive()
    {
        TemporaryDirectory directory("partial-commit");
        auto config = directory.config();
        {
            PersistentTradingEngine engine(config);
            prepare(engine);
            CHECK(engine.execute(commands::AddSpotBook{1, OrderBookImplementation::IndexedTree}).result);
            CHECK(engine.execute(commands::UpdateBalance{1, 1, 5}).result);
            CHECK(engine.execute(order(1, OrderSide::Ask, 100, 5, 1), 100).result);
            CHECK(engine.execute(commands::CancelOrder{1, 1, 2}, 101).result);
            engine.close();
        }

        const auto journal = directory.path / "journal.wal";
        std::filesystem::resize_file(journal, std::filesystem::file_size(journal) - 8);
        {
            PersistentTradingEngine engine(config);
            CHECK(engine.execute(commands::CancelOrder{1, 1, 2}, 101).duplicate);
            CHECK(engine.getMatchingEngine().queryOrder(1)->getFillingStatus().status == OrderStatus::Cancelled);
            CHECK(engine.getUserManager().getUser(1)->getSpotAccount().queryActiveBalance(1) == 5);
            CHECK(engine.execute(commands::ArchiveHistory{128, {1}, {}}).result.archivedOrders == 1);
            CHECK(!engine.getMatchingEngine().queryOrder(1));
            engine.checkpoint();
            engine.close();
        }

        PersistentTradingEngine engine(config);
        CHECK(!engine.getMatchingEngine().queryOrder(1));
        CHECK(engine.execute(order(1, OrderSide::Ask, 100, 1, 3)).result.entityId == 2);
        engine.close();
    }

    void abruptExit(const std::filesystem::path& directory)
    {
        PersistenceConfig config;
        config.directory = directory;
        PersistentTradingEngine engine(config);
        CHECK(engine.execute(commands::CreateUser{}, 77).durable());
        // Deliberately bypass destructors and stdio cleanup.
        std::_Exit(0);
    }

    void crashRecovery()
    {
        TemporaryDirectory directory("process-crash");
#ifdef _WIN32
        std::wstring executable(32768, L'\0');
        const auto length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
        CHECK(length != 0 && length < executable.size());
        executable.resize(length);
        std::wstring command = L"\"" + executable + L"\" --crash \"" + directory.path.wstring() + L"\"";
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        CHECK(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process));
        CHECK(WaitForSingleObject(process.hProcess, 30000) == WAIT_OBJECT_0);
        DWORD code = 1;
        CHECK(GetExitCodeProcess(process.hProcess, &code) && code == 0);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
#else
        const auto child = fork();
        CHECK(child >= 0);
        if (child == 0)
        {
            abruptExit(directory.path);
        }

        int status = 0;
        CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
#endif
        PersistentTradingEngine engine(directory.config());
        CHECK(engine.getUserManager().getUserCount() == 1);
        CHECK(engine.execute(commands::CreateUser{}, 77).duplicate);
        CHECK(engine.execute(commands::CreateUser{}, 78).result.entityId == 2);
        engine.close();
    }

    void ownerAndIdleFlush()
    {
        TemporaryDirectory directory("owner");
        auto config = directory.config();
        config.flushPolicy = FlushPolicy::Batch;
        config.flushEveryCommands = 1000;
        config.flushIntervalMilliseconds = 1;
        PersistentTradingEngine engine(config);
        CHECK(engine.execute(commands::CreateUser{}).result);
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
        engine.tick();
        CHECK(engine.durableThrough() == engine.lastSequence());
        std::thread incorrectOwner([&]
        {
            expectError(PersistenceFailure::InvalidConfiguration, [&] { engine.execute(commands::CreateUser{}); });
        });
        incorrectOwner.join();
        CHECK(engine.getUserManager().getUserCount() == 1 && !engine.isFaulted());
        engine.close();
    }

    void checkpointBoundaries()
    {
        TemporaryDirectory directory("checkpoint-boundary");
        auto config = directory.config();
        config.pruneJournalAfterCheckpoint = false;
        config.checkpointEveryCommands = 2;
        {
            PersistentTradingEngine engine(config);
            CHECK(engine.execute(commands::CreateUser{}).result.entityId == 1);
            CHECK(engine.execute(commands::CreateUser{}).result.entityId == 2);
            CHECK(std::filesystem::exists(directory.path / "snapshot-2.bin"));
            CHECK(std::filesystem::file_size(directory.path / "journal.wal") != 0);
            engine.close();
        }

        // A checkpoint can cover a stale incomplete commit. Recovery must
        // remove that orphan before it starts a new command sequence.
        const auto journal = directory.path / "journal.wal";
        std::filesystem::resize_file(journal, std::filesystem::file_size(journal) - 4);
        {
            PersistentTradingEngine engine(config);
            CHECK(engine.getUserManager().getUserCount() == 2);
            CHECK(engine.execute(commands::CreateUser{}).result.entityId == 3);
            engine.close();
        }

        {
            PersistentTradingEngine engine(config);
            CHECK(engine.getUserManager().getUserCount() == 3);
            expectError(PersistenceFailure::RequestConflict, [&]
            {
                engine.visitCommitted(0, [](const CommittedCommand&)
                {
                    throw PersistenceError(PersistenceFailure::RequestConflict, "external visitor failure");
                });
            });
            CHECK(!engine.isFaulted());
            CHECK(engine.execute(commands::CreateUser{}).result.entityId == 4);
            engine.close();
        }

        {
            PersistentTradingEngine engine(config);
            CHECK(engine.getUserManager().getUserCount() == 4);
            CHECK(engine.execute(commands::CreateUser{}).result.entityId == 5);
            engine.close();
        }

        std::filesystem::remove(directory.path / "snapshot-4.bin");
        expectError(PersistenceFailure::CorruptData, [&] { PersistentTradingEngine engine(config); });
    }

    void oversizedCommit()
    {
        TemporaryDirectory directory("commit-limit");
        auto config = directory.config();
        config.maxRecordBytes = 128;
        {
            PersistentTradingEngine engine(config);
            prepare(engine);
            CHECK(engine.execute(commands::AddSpotBook{1}).result);
            CHECK(engine.execute(commands::UpdateBalance{1, 1, 5}).result);
            CHECK(engine.execute(commands::UpdateBalance{3, 1, 5}).result);
            CHECK(engine.execute(commands::UpdateBalance{5, 2, 2000}).result);
            CHECK(engine.execute(order(1, OrderSide::Ask, 100, 5, 1)).result);
            CHECK(engine.execute(order(2, OrderSide::Ask, 100, 5, 2)).result);
            expectError(PersistenceFailure::InvalidConfiguration, [&] { engine.execute(order(3, OrderSide::Bid, 100, 10, 3), 10); });
            CHECK(engine.isFaulted());
        }

        config.maxRecordBytes = 1024;
        PersistentTradingEngine engine(config);
        CHECK(engine.execute(order(3, OrderSide::Bid, 100, 10, 3), 10).duplicate);
        CHECK(!engine.getMatchingEngine().bestAsk(1));
        CHECK(engine.getUserManager().getUser(3)->getSpotAccount().queryActiveBalance(1) == 10);
        engine.close();
    }

    void conservationDiagnostics()
    {
        PersistenceConfig config;
        config.enabled = false;
        PersistentTradingEngine engine(config);
        prepare(engine);
        CHECK(engine.execute(commands::UpdateBalance{1, 2, 1000}).result);
        auto& matching = engine.getMatchingEngine();
        auto& audit = matching.getAuditEngine();
        CHECK(audit.report(engine.getAssetManager()).consistent);
        // Deliberate out-of-band corruption: public commands never bypass the
        // external-flow accounting or create unowned reservations.
        auto& account = const_cast<SpotAccount&>(engine.getUserManager().getUser(1)->getSpotAccount());
        CHECK(account.updateBalance(2, 1));
        auto report = audit.report(engine.getAssetManager(), 0);
        CHECK(!report.conserved && !report.consistent && report.issueCount == 1 && report.issues.empty());
        CHECK(account.updateBalance(2, -1));
        CHECK(account.applyBalanceChange(2, -5, 5));
        report = audit.report(engine.getAssetManager());
        CHECK(report.conserved && !report.consistent && report.issues.front().type == audit::AuditIssueType::Reservation);
        CHECK(account.applyBalanceChange(2, 5, -5));
        CHECK(audit.report(engine.getAssetManager()).consistent);
    }

    void blockedLiquidationRecovery()
    {
        TemporaryDirectory directory("blocked-liquidation");
        const auto config = directory.config();
        PositionId victim = 0;
        CommandSequence sequence = 0;
        {
            PersistentTradingEngine engine(config);
            prepare(engine, SymbolType::Perpetual);
            ContractSpec spec;
            spec.firstFundingTimestamp = 1000;
            spec.makerFeeRate = 0;
            spec.takerFeeRate = 0;
            spec.liquidationFeeRate = 0;
            CHECK(engine.execute(commands::AddContractBook{1, spec, 100, 0}).result);
            for (AccountId id : {2, 4, 6})
            {
                CHECK(engine.execute(commands::UpdateBalance{id, 2, 10'000}).result);
            }

            ContractOrderOptions options;
            CHECK(engine.execute(order(2, OrderSide::Ask, 100, 10, 1, options)).result);
            victim = engine.execute(order(1, OrderSide::Bid, 100, 10, 2, options)).result.positionId;
            CHECK(engine.execute(commands::UpdateMarkPrice{1, 80, 3}).result.state == static_cast<std::uint8_t>(ContractState::PendingLiquidation));
            CHECK(engine.execute(order(3, OrderSide::Bid, 80, 10, 4, options)).result);
            for (Timestamp timestamp = 5; timestamp < 10; ++timestamp)
            {
                CHECK(engine.execute(commands::ProcessLiquidations{1, timestamp, true, 100}).result.state == static_cast<std::uint8_t>(ContractState::PendingLiquidation));
            }

            CHECK(engine.getMatchingEngine().getContractEngine().getSettleEngine().liquidationRetryCacheHits() > 0);
            engine.checkpoint();
            sequence = engine.lastSequence();
            engine.close();
        }

        {
            PersistentTradingEngine engine(config);
            CHECK(engine.lastSequence() == sequence);
            const auto& contracts = engine.getMatchingEngine().getContractEngine();
            CHECK(contracts.queryPosition(victim)->positionSize == 10);
            CHECK(contracts.getSettleEngine().liquidationRetryCacheHits() > 0);
            CHECK(engine.execute(commands::FundInsurance{1, 6, 100}).result);
            CHECK(engine.execute(commands::ProcessLiquidations{1, 10, true, 100}).result);
            CHECK(contracts.queryPosition(victim)->closed);
            CHECK(contracts.findMarket(1)->insuranceBalance == 0);
            engine.close();
        }

        PersistentTradingEngine recovered(config);
        CHECK(recovered.getMatchingEngine().getContractEngine().queryPosition(victim)->closed);
        recovered.close();
    }
}

int main(int argc, char** argv)
{
    if (argc == 3 && std::string(argv[1]) == "--crash")
    {
        abruptExit(std::filesystem::path(argv[2]));
    }

    spotRecovery();
    contractRecovery(SymbolType::Perpetual, MarginMode::Isolated);
    contractRecovery(SymbolType::Perpetual, MarginMode::Cross);
    contractRecovery(SymbolType::Future, MarginMode::Isolated);
    flushPolicies();
    tailAndCorruption();
    failureRecovery();
    corruptSnapshot();
    invalidJournal();
    partialCommitAndArchive();
    crashRecovery();
    ownerAndIdleFlush();
    checkpointBoundaries();
    oversizedCommit();
    blockedLiquidationRecovery();
    conservationDiagnostics();
    std::puts("Persistence tests passed");
}
