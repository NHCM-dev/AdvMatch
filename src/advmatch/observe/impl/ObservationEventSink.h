#ifndef ADVMATCH_OBSERVATION_EVENT_SINK_H
#define ADVMATCH_OBSERVATION_EVENT_SINK_H

#include "TradingObserver.h"
#include "advmatch/persistence/PersistentTradingEngine.h"

namespace advmatch::observe
{
    // Register on the persistence owner thread. Optional downstream preserves
    // the application's live event delivery while recording bounded tapes.
    class ObservationEventSink final : public PersistentEventSink
    {
        TradingObserver& observer;
        PersistentEventSink* downstream;

    public:
        explicit ObservationEventSink(TradingObserver& observer, PersistentEventSink* downstream = nullptr)
            : observer(observer), downstream(downstream)
        {
        }

        void onCommitted(const CommandReceipt& receipt, std::span<const Trade> trades, std::span<const ContractEvent> events) noexcept override
        {
            this->observer.notifyChanged();
            this->observer.recordBatch(receipt.sequence, trades, events);
            if (this->downstream != nullptr)
            {
                this->downstream->onCommitted(receipt, trades, events);
            }
        }
    };
}

#endif // ADVMATCH_OBSERVATION_EVENT_SINK_H
