#ifndef ADVMATCH_TRADING_CONTROL_H
#define ADVMATCH_TRADING_CONTROL_H

#include "advmatch/util/ValueTypes.h"
#include <string>

namespace advmatch
{
    enum class TradingControlState : std::uint8_t
    {
        Success,
        InvalidSymbol,
        InvalidTimestamp,
        InvalidReason
    };

    struct TradingHalt
    {
        bool halted = false;
        Timestamp updatedTimestamp = 0;
        std::string reason;
    };

    struct TradingStatusView
    {
        // Zero selects the exchange. Global and symbol halts are independent.
        SymbolId symbolId = 0;
        bool halted = false;
        bool exchangeHalted = false;
        bool symbolHalted = false;
        Timestamp exchangeUpdatedTimestamp = 0;
        Timestamp symbolUpdatedTimestamp = 0;
        std::string exchangeReason;
        std::string symbolReason;
    };

    struct TradingControlResult
    {
        TradingControlState state = TradingControlState::Success;

        explicit operator bool() const noexcept
        {
            return this->state == TradingControlState::Success;
        }
    };
}

#endif // ADVMATCH_TRADING_CONTROL_H
