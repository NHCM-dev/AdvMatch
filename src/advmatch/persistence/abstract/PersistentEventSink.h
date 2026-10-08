#ifndef ADVMATCH_PERSISTENT_EVENT_SINK_H
#define ADVMATCH_PERSISTENT_EVENT_SINK_H

#include "../model/CommittedCommand.h"

namespace advmatch
{
    class PersistentEventSink
    {
    public:
        virtual ~PersistentEventSink() = default;

        // Owner thread, after commit/selected flush. Buffered receipts may not
        // yet be durable. Do not reenter or block the trading engine.
        virtual void onCommitted(const CommandReceipt& receipt, std::span<const Trade> trades, std::span<const ContractEvent> events) noexcept = 0;
    };
}

#endif // ADVMATCH_PERSISTENT_EVENT_SINK_H
