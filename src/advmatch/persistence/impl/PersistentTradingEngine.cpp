#include "PersistentTradingEngine.h"
#include "../codec/CommandCodec.h"
#include "FilePersistenceStore.h"
#include "advmatch/matching/contracts/ContractEngine.h"
#include "advmatch/api/command/validation/TradingCommandValidator.h"

#include <chrono>
#include <algorithm>
#include <deque>
#include <thread>

namespace advmatch
{
    namespace
    {
        constexpr std::uint32_t ReplayImageMagic = 0x524D4441;
        constexpr std::uint16_t ReplayImageVersion = 1;

        std::vector<std::byte> encodeCommit(const CommandResult& result, const TradeBuffer& trades, const std::vector<ContractEvent>& events)
        {
            BinaryWriter writer;
            const auto encodedResult = encodeResult(result);
            writer.number<std::uint32_t>(static_cast<std::uint32_t>(encodedResult.size()));
            writer.bytes(encodedResult);
            writer.number<std::uint64_t>(trades.size());
            for (const Trade& trade : trades)
            {
                writer.number(trade.makerOrderId);
                writer.number(trade.takerOrderId);
                writer.number(trade.price);
                writer.number(trade.quantity);
            }

            writer.number<std::uint64_t>(events.size());
            for (const ContractEvent& event : events)
            {
                writer.enumeration(event.type);
                writer.number(event.symbolId);
                writer.number(event.positionId);
                writer.number(event.userId);
                writer.number(event.price);
                writer.number(event.quantity);
                writer.number(event.realizedPnL);
                writer.number(event.fundingChange);
                writer.number(event.fee);
                writer.number(event.insurance);
                writer.number(event.timestamp);
                writer.number(event.orderId);
                writer.number(event.counterOrderId);
            }

            return writer.take();
        }

        CommittedCommand decodeCommit(CommandSequence sequence, std::uint64_t requestId, std::span<const std::byte> payload)
        {
            BinaryReader reader(payload);
            CommittedCommand value{sequence, requestId, decodeResult(reader.bytes(reader.number<std::uint32_t>())), {}, {}};
            const auto trades = reader.count(4 * sizeof(std::uint64_t));
            value.trades.reserve(trades);
            for (std::size_t index = 0; index < trades; ++index)
            {
                value.trades.push_back({reader.number<OrderId>(), reader.number<OrderId>(), reader.number<Price>(), reader.number<Quantity>()});
            }

            const auto events = reader.count(93);
            value.contractEvents.reserve(events);
            for (std::size_t index = 0; index < events; ++index)
            {
                value.contractEvents.push_back({reader.enumeration(ContractEventType::Delivery), reader.number<SymbolId>(), reader.number<PositionId>(), reader.number<UserId>(), reader.number<Price>(), reader.number<Quantity>(), reader.number<BalanceChange>(), reader.number<BalanceChange>(), reader.number<Balance>(), reader.number<Balance>(), reader.number<Timestamp>(), reader.number<OrderId>(), reader.number<OrderId>()});
            }

            reader.finish();
            return value;
        }

        using PairVisitor = std::function<void(CommandSequence, std::span<const std::byte>, std::span<const std::byte>)>;

        std::uint64_t readImage(const StoredSnapshot& snapshot, const PairVisitor& visitor)
        {
            BinaryReader reader(snapshot.payload);
            persistenceCheck(reader.number<std::uint32_t>() == ReplayImageMagic, "invalid replay checkpoint magic");
            persistenceCheck(reader.number<std::uint16_t>() == ReplayImageVersion, "unsupported replay checkpoint schema", PersistenceFailure::UnsupportedVersion);
            const auto count = reader.count(16);
            CommandSequence expected = 1;
            for (std::size_t index = 0; index < count; ++index)
            {
                const auto sequence = reader.number<CommandSequence>();
                persistenceCheck(sequence == expected++, "checkpoint sequence gap");
                const auto intent = reader.bytes(reader.number<std::uint32_t>());
                const auto commit = reader.bytes(reader.number<std::uint32_t>());
                visitor(sequence, intent, commit);
            }

            reader.finish();
            persistenceCheck(count == snapshot.sequence, "checkpoint boundary mismatch");
            return count;
        }

        void writePair(BinaryWriter& writer, CommandSequence sequence, std::span<const std::byte> intent, std::span<const std::byte> commit)
        {
            persistenceCheck(intent.size() <= std::numeric_limits<std::uint32_t>::max() && commit.size() <= std::numeric_limits<std::uint32_t>::max(), "checkpoint record too large");
            writer.number(sequence);
            writer.number<std::uint32_t>(static_cast<std::uint32_t>(intent.size()));
            writer.bytes(intent);
            writer.number<std::uint32_t>(static_cast<std::uint32_t>(commit.size()));
            writer.bytes(commit);
        }
    }

    struct PersistentTradingEngine::Impl : ContractEventSink
    {
        struct Request
        {
            CommandSequence sequence;
            std::vector<std::byte> intent;
            CommandResult result;
        };

        PersistenceConfig config;
        std::unique_ptr<PersistenceStore> store;
        TradingState state;
        TradeBuffer trades;
        std::vector<ContractEvent> events;
        DenseMap<std::uint64_t, Request> requests;
        std::deque<std::uint64_t> requestOrder;
        std::uint64_t highestRequestId = 0;
        CommandSequence sequence = 0;
        CommandSequence durable = 0;
        CommandSequence checkpointSequence = 0;
        std::chrono::steady_clock::time_point lastFlush = std::chrono::steady_clock::now();
        PersistentEventSink* sink = nullptr;
        std::optional<std::thread::id> owner;
        bool faulted = false;
        bool closed = false;
        bool busy = false;
        bool eventAllocationFailed = false;

        explicit Impl(PersistenceConfig config, std::unique_ptr<PersistenceStore> store)
            : config(std::move(config)), store(std::move(store))
        {
            persistenceCheck(this->config.retainedCheckpoints > 0 && this->config.maxRecordBytes >= 128 && this->config.maxSnapshotBytes >= 14 && (this->config.flushPolicy != FlushPolicy::Batch || this->config.flushEveryCommands > 0), "invalid persistence configuration", PersistenceFailure::InvalidConfiguration);
            persistenceCheck(this->config.flushPolicy >= FlushPolicy::EveryCommand && this->config.flushPolicy <= FlushPolicy::Manual && this->config.tailRecovery >= TailRecovery::Reject && this->config.tailRecovery <= TailRecovery::TruncateIncomplete, "invalid persistence policy", PersistenceFailure::InvalidConfiguration);
            persistenceCheck(sizeof(std::size_t) >= sizeof(std::uint64_t), "persistent trading requires a 64-bit target", PersistenceFailure::InvalidConfiguration);
            persistenceCheck(this->config.enabled || this->store == nullptr, "disabled persistence cannot use a store", PersistenceFailure::InvalidConfiguration);
            if (this->config.enabled && this->store == nullptr)
            {
                this->store = std::make_unique<FilePersistenceStore>(this->config);
            }

            this->state.matching.getContractEngine().setEventSink(this);
            if (this->store)
            {
                this->recover();
            }
        }

        void onEvent(const ContractEvent& event) noexcept override
        {
            try
            {
                this->events.push_back(event);
            }
            catch (...)
            {
                this->eventAllocationFailed = true;
            }
        }

        void checkAccess()
        {
            persistenceCheck(!this->faulted && !this->closed, "persistence session is closed or faulted", PersistenceFailure::Faulted);
            const auto current = std::this_thread::get_id();
            if (!this->owner)
            {
                this->owner = current;
            }

            persistenceCheck(*this->owner == current && !this->busy, "persistence requires one owner thread and no reentrancy", PersistenceFailure::InvalidConfiguration);
        }

        CommandResult run(const TradingCommand& command)
        {
            this->events.clear();
            this->eventAllocationFailed = false;
            auto result = executeTradingCommand(this->state, command, this->trades);
            persistenceCheck(!this->eventAllocationFailed, "contract event buffer allocation failed", PersistenceFailure::Faulted);
            return result;
        }

        void remember(std::uint64_t requestId, CommandSequence sequence, std::span<const std::byte> intent, const CommandResult& result)
        {
            if (requestId == 0)
            {
                return;
            }

            persistenceCheck(requestId > this->highestRequestId, "non-monotonic request ID in journal");
            this->highestRequestId = requestId;
            if (this->config.retainedRequests == 0)
            {
                return;
            }

            this->requests.emplace(requestId, Request{sequence, {intent.begin(), intent.end()}, result});
            this->requestOrder.push_back(requestId);
            while (this->requestOrder.size() > this->config.retainedRequests)
            {
                this->requests.erase(this->requestOrder.front());
                this->requestOrder.pop_front();
            }
        }

        void applyPair(CommandSequence sequence, std::span<const std::byte> intent, std::span<const std::byte> commit)
        {
            persistenceCheck(this->sequence != std::numeric_limits<CommandSequence>::max() && sequence == this->sequence + 1, "journal sequence gap");
            const auto envelope = decodeCommand(intent);
            const auto result = this->run(envelope.command);
            const auto actual = encodeCommit(result, this->trades, this->events);
            persistenceCheck(std::ranges::equal(actual, commit), "replayed result/trades/events differ; engine schema migration required", PersistenceFailure::ReplayMismatch);
            this->remember(envelope.requestId, sequence, intent, result);
            this->sequence = sequence;
        }

        // Covered journal records may remain after a crash between publishing
        // a checkpoint and truncating its WAL. Validate pairs, then skip them.
        std::optional<JournalRecord> scanJournal(CommandSequence covered, const PairVisitor& visitor)
        {
            std::optional<JournalRecord> pending;
            CommandSequence previous = 0;
            this->store->replay([&](const JournalRecord& record)
            {
                if (record.type == JournalRecordType::Intent)
                {
                    persistenceCheck(!pending, "journal has two uncommitted intents");
                    persistenceCheck(record.sequence != 0, "journal has a zero sequence");
                    if (previous == 0)
                    {
                        // A crash after a newer checkpoint publication may leave
                        // WAL starting immediately after an older checkpoint.
                        persistenceCheck(record.sequence <= covered || (covered != std::numeric_limits<CommandSequence>::max() && record.sequence == covered + 1), "journal starts at unexpected sequence");
                    }
                    else
                    {
                        const bool next = previous != std::numeric_limits<CommandSequence>::max() && record.sequence == previous + 1;
                        const bool coveredGap = previous < covered && record.sequence > previous && (record.sequence <= covered || (covered != std::numeric_limits<CommandSequence>::max() && record.sequence == covered + 1));
                        persistenceCheck(next || coveredGap, "journal sequence gap");
                    }

                    pending = record;
                }
                else
                {
                    persistenceCheck(record.type == JournalRecordType::Commit, "unknown journal record type");
                    persistenceCheck(pending && record.sequence == pending->sequence, "journal commit has no matching intent");
                    if (record.sequence > covered)
                    {
                        visitor(record.sequence, pending->payload, record.payload);
                    }

                    previous = record.sequence;
                    pending.reset();
                }
            });
            return pending;
        }

        void recover()
        {
            if (auto snapshot = this->store->loadSnapshot())
            {
                readImage(*snapshot, [&](CommandSequence sequence, auto intent, auto commit)
                {
                    persistenceCheck(intent.size() <= this->config.maxRecordBytes && commit.size() <= this->config.maxRecordBytes, "checkpoint record exceeds configured limit");
                    this->applyPair(sequence, intent, commit);
                });
                this->checkpointSequence = snapshot->sequence;
            }

            const auto pending = this->scanJournal(this->checkpointSequence, [&](CommandSequence sequence, auto intent, auto commit)
            {
                this->applyPair(sequence, intent, commit);
            });
            if (pending && pending->sequence <= this->checkpointSequence)
            {
                // The durable checkpoint already contains this command. Its
                // stale WAL tail must be removed before appending new intents.
                this->store->resetJournalThrough(this->checkpointSequence);
            }
            else if (pending)
            {
                persistenceCheck(this->sequence != std::numeric_limits<CommandSequence>::max() && pending->sequence == this->sequence + 1, "unfinished intent sequence gap");
                const auto envelope = decodeCommand(pending->payload);
                const auto result = this->run(envelope.command);
                auto commit = encodeCommit(result, this->trades, this->events);
                persistenceCheck(commit.size() <= this->config.maxRecordBytes, "recovered commit exceeds configured limit", PersistenceFailure::InvalidConfiguration);
                this->store->append({JournalRecordType::Commit, pending->sequence, std::move(commit)});
                this->remember(envelope.requestId, pending->sequence, pending->payload, result);
                this->sequence = pending->sequence;
            }

            this->store->sync();
            this->durable = this->sequence;
            this->events.clear();
            this->trades.clear();
        }

        void sync()
        {
            if (this->store)
            {
                this->store->sync();
                this->durable = this->sequence;
            }

            this->lastFlush = std::chrono::steady_clock::now();
        }

        bool flushDue() const
        {
            if (this->config.flushPolicy != FlushPolicy::Batch || this->sequence == this->durable)
            {
                return false;
            }

            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - this->lastFlush).count();
            return this->sequence - this->durable >= this->config.flushEveryCommands || (this->config.flushIntervalMilliseconds != 0 && static_cast<std::uint64_t>(elapsed) >= this->config.flushIntervalMilliseconds);
        }

        void checkpoint()
        {
            if (!this->store || this->sequence == this->checkpointSequence)
            {
                return;
            }

            this->sync();
            BinaryWriter image;
            image.number(ReplayImageMagic);
            image.number(ReplayImageVersion);
            image.number<std::uint64_t>(this->sequence);
            CommandSequence copied = 0;
            auto copy = [&](CommandSequence sequence, auto intent, auto commit)
            {
                persistenceCheck(sequence == ++copied, "checkpoint source sequence gap");
                persistenceCheck(image.view().size() <= this->config.maxSnapshotBytes && intent.size() + commit.size() + 16 <= this->config.maxSnapshotBytes - image.view().size(), "checkpoint exceeds configured size", PersistenceFailure::InvalidConfiguration);
                writePair(image, sequence, intent, commit);
            };
            if (auto snapshot = this->store->loadSnapshot())
            {
                persistenceCheck(snapshot->sequence == this->checkpointSequence, "checkpoint changed outside this session");
                readImage(*snapshot, copy);
            }

            const auto pending = this->scanJournal(this->checkpointSequence, copy);
            persistenceCheck(!pending && copied == this->sequence, "checkpoint contains uncommitted/gapped commands");
            this->store->publishSnapshot(this->sequence, image.view());
            this->checkpointSequence = this->sequence;
            if (this->config.pruneJournalAfterCheckpoint)
            {
                this->store->resetJournalThrough(this->sequence);
            }
        }
    };

    PersistentTradingEngine::PersistentTradingEngine(PersistenceConfig config, std::unique_ptr<PersistenceStore> store)
        : impl(std::make_unique<Impl>(std::move(config), std::move(store)))
    {
    }

    PersistentTradingEngine::~PersistentTradingEngine() = default;

    CommandReceipt PersistentTradingEngine::execute(const TradingCommand& command, std::uint64_t requestId)
    {
        this->impl->checkAccess();
        std::vector<std::byte> intent;
        // maxRecordBytes is at least 128; every fixed command is <= 111 bytes.
        // Keep exact size checks for strings/archive lists, WAL and idempotency.
        if (this->impl->store || requestId != 0 || !api::command::TradingCommandValidator::validateWithoutEncoding(command))
        {
            intent = encodeCommand(command, requestId);
            persistenceCheck(intent.size() <= this->impl->config.maxRecordBytes, "command exceeds configured size", PersistenceFailure::InvalidConfiguration);
            static_cast<void>(decodeCommand(intent));
        }
        if (requestId != 0)
        {
            const auto found = this->impl->requests.find(requestId);
            if (found != this->impl->requests.end())
            {
                persistenceCheck(found->second.intent == intent, "request ID reused for different command", PersistenceFailure::RequestConflict);
                this->impl->trades.clear();
                return {found->second.sequence, this->impl->durable, true, found->second.result};
            }

            persistenceCheck(requestId > this->impl->highestRequestId, "request ID expired or out of order", PersistenceFailure::RequestExpired);
        }

        persistenceCheck(this->impl->sequence != std::numeric_limits<CommandSequence>::max(), "command sequence exhausted", PersistenceFailure::InvalidConfiguration);
        const auto sequence = this->impl->sequence + 1;
        this->impl->busy = true;
        try
        {
            if (this->impl->store)
            {
                this->impl->store->append({JournalRecordType::Intent, sequence, intent});
            }

            const auto result = this->impl->run(command);
            if (this->impl->store)
            {
                auto commit = encodeCommit(result, this->impl->trades, this->impl->events);
                persistenceCheck(commit.size() <= this->impl->config.maxRecordBytes, "commit exceeds configured size; increase maxRecordBytes before recovery", PersistenceFailure::InvalidConfiguration);
                this->impl->store->append({JournalRecordType::Commit, sequence, std::move(commit)});
            }

            this->impl->sequence = sequence;
            this->impl->remember(requestId, sequence, intent, result);
            if (this->impl->store && (this->impl->config.flushPolicy == FlushPolicy::EveryCommand || this->impl->flushDue()))
            {
                this->impl->sync();
            }

            if (this->impl->config.checkpointEveryCommands != 0 && sequence - this->impl->checkpointSequence >= this->impl->config.checkpointEveryCommands)
            {
                this->impl->checkpoint();
            }

            CommandReceipt receipt{sequence, this->impl->durable, false, result};
            if (this->impl->sink != nullptr)
            {
                this->impl->sink->onCommitted(receipt, {this->impl->trades.data(), this->impl->trades.size()}, this->impl->events);
            }

            this->impl->busy = false;
            return receipt;
        }
        catch (...)
        {
            this->impl->faulted = true;
            this->impl->busy = false;
            throw;
        }
    }

    void PersistentTradingEngine::flush()
    {
        this->impl->checkAccess();
        try
        {
            this->impl->sync();
        }
        catch (...)
        {
            this->impl->faulted = true;
            throw;
        }
    }

    void PersistentTradingEngine::tick()
    {
        this->impl->checkAccess();
        if (this->impl->flushDue())
        {
            this->flush();
        }
    }

    void PersistentTradingEngine::checkpoint()
    {
        this->impl->checkAccess();
        this->impl->busy = true;
        try
        {
            this->impl->checkpoint();
            this->impl->busy = false;
        }
        catch (...)
        {
            this->impl->faulted = true;
            this->impl->busy = false;
            throw;
        }
    }

    void PersistentTradingEngine::close()
    {
        if (this->impl->closed)
        {
            return;
        }

        this->flush();
        this->impl->closed = true;
        this->impl->store.reset();
    }

    void PersistentTradingEngine::setEventSink(PersistentEventSink* sink)
    {
        this->impl->checkAccess();
        this->impl->sink = sink;
    }

    void PersistentTradingEngine::visitCommitted(CommandSequence afterSequence, const std::function<void(const CommittedCommand&)>& visitor)
    {
        this->impl->checkAccess();
        if (!this->impl->store)
        {
            return;
        }

        // Reading an update stream would otherwise flush stdio implicitly.
        // Make the resulting durability boundary explicit before visiting.
        this->flush();
        this->impl->busy = true;
        bool visitorFailed = false;
        try
        {
            auto visit = [&](CommandSequence sequence, auto intent, auto commit)
            {
                if (sequence > afterSequence && sequence <= this->impl->durable)
                {
                    const auto envelope = decodeCommand(intent);
                    const auto value = decodeCommit(sequence, envelope.requestId, commit);
                    try
                    {
                        visitor(value);
                    }
                    catch (...)
                    {
                        visitorFailed = true;
                        throw;
                    }
                }
            };
            if (auto snapshot = this->impl->store->loadSnapshot())
            {
                readImage(*snapshot, visit);
            }

            const auto pending = this->impl->scanJournal(this->impl->checkpointSequence, visit);
            persistenceCheck(!pending, "uncommitted live journal during outbox visit");
            this->impl->busy = false;
        }
        catch (...)
        {
            this->impl->busy = false;
            if (!visitorFailed)
            {
                this->impl->faulted = true;
            }

            // A failed external visitor is retryable; a failed storage read
            // stops new writes to a potentially damaged journal.
            throw;
        }
    }

    const AssetManager& PersistentTradingEngine::getAssetManager() const noexcept
    {
        return this->impl->state.assets;
    }

    const UserManager& PersistentTradingEngine::getUserManager() const noexcept
    {
        return this->impl->state.users;
    }

    const SymbolManager& PersistentTradingEngine::getSymbolManager() const noexcept
    {
        return this->impl->state.symbols;
    }

    const MatchingEngine& PersistentTradingEngine::getMatchingEngine() const noexcept
    {
        return this->impl->state.matching;
    }

    const TradeBuffer& PersistentTradingEngine::getTrades() const noexcept
    {
        return this->impl->trades;
    }

    CommandSequence PersistentTradingEngine::lastSequence() const noexcept
    {
        return this->impl->sequence;
    }

    CommandSequence PersistentTradingEngine::durableThrough() const noexcept
    {
        return this->impl->durable;
    }

    bool PersistentTradingEngine::isFaulted() const noexcept
    {
        return this->impl->faulted;
    }
}
