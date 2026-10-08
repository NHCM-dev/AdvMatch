#ifndef ADVMATCH_HISTORY_SINK_H
#define ADVMATCH_HISTORY_SINK_H

#include "advmatch/trading/Order.h"
#include "advmatch/trading/futures/Position.h"

#include <cstddef>

namespace advmatch
{
    // Owner-thread maintenance callback. Return true only after accepting the
    // value into your history store. Return false to retain it for retry. Do
    // not reenter the engine. Position pointers expire after accepted archival.
    class HistorySink
    {
    public:
        virtual ~HistorySink() = default;
        virtual bool archiveOrder(const BasicOrder& order) noexcept = 0;
        virtual bool archivePosition(const Position& position) noexcept = 0;
    };

    struct HistoryArchiveResult
    {
        std::size_t scannedOrders = 0;
        std::size_t scannedPositions = 0;
        std::size_t archivedOrders = 0;
        std::size_t archivedPositions = 0;
    };
}

#endif // ADVMATCH_HISTORY_SINK_H
