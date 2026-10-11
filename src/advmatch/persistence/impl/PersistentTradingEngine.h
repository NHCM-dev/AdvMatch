#ifndef ADVMATCH_PERSISTENT_TRADING_ENGINE_H
#define ADVMATCH_PERSISTENT_TRADING_ENGINE_H

#include "advmatch/matching/MatchingEngine.h"

#include "../command/CommandExecutor.h"
#include "../config/PersistenceConfig.h"
#include "../abstract/PersistenceStore.h"
#include "../abstract/PersistentEventSink.h"

#include <memory>

namespace advmatch
{
    // Construct fresh or recover automatically. All operations (including reads,
    // flush, checkpoints and event visits) belong on one consumer thread. After
    // I/O/execution failure the session is faulted; discard it and recover.
    class PersistentTradingEngine
    {
        struct Impl;
        std::unique_ptr<Impl> impl;

    public:
        explicit PersistentTradingEngine(PersistenceConfig config = {}, std::unique_ptr<PersistenceStore> store = nullptr);
        ~PersistentTradingEngine();
        PersistentTradingEngine(const PersistentTradingEngine&) = delete;
        PersistentTradingEngine& operator=(const PersistentTradingEngine&) = delete;

        // requestId=0 disables retry deduplication. IDs without the high bit must
        // increase globally. High-bit IDs use a 15-bit gateway namespace and a
        // 48-bit counter; out-of-order arrivals are accepted within the retained
        // window. Eviction advances that namespace's retirement watermark;
        // expired IDs are rejected, never executed again. WAL/checkpoint replay
        // reconstructs both the cache and watermarks.
        CommandReceipt execute(const TradingCommand& command, std::uint64_t requestId = 0);
        void flush();
        void tick();
        void checkpoint();
        void close();
        void setEventSink(PersistentEventSink* sink);

        // Durable outbox read: includes archived checkpoint records plus WAL,
        // ordered by sequence. Store your external delivery cursor separately.
        // Callbacks must not mutate/reenter this engine. Reads are O(history).
        void visitCommitted(CommandSequence afterSequence, const std::function<void(const CommittedCommand&)>& visitor);

        const AssetManager& getAssetManager() const noexcept;
        const UserManager& getUserManager() const noexcept;
        const SymbolManager& getSymbolManager() const noexcept;
        const MatchingEngine& getMatchingEngine() const noexcept;
        const TradeBuffer& getTrades() const noexcept;
        CommandSequence lastSequence() const noexcept;
        CommandSequence durableThrough() const noexcept;
        bool isFaulted() const noexcept;
    };
}

#endif // ADVMATCH_PERSISTENT_TRADING_ENGINE_H
