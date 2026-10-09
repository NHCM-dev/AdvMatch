#ifndef ADVMATCH_TRADING_CONTROL_ENGINE_H
#define ADVMATCH_TRADING_CONTROL_ENGINE_H

#include "advmatch/trading/TradingControl.h"
#include "advmatch/trading/Symbol.h"
#include <optional>

namespace advmatch
{
    class MatchingEngine;

    // Called only on the matching owner thread. Admission checks are constant
    // work; exchange-wide resume visits books to re-evaluate waiting triggers.
    class TradingControlEngine
    {
        MatchingEngine& matching;
        TradingHalt exchangeHalt;

    public:
        explicit TradingControlEngine(MatchingEngine& matching)
            : matching(matching)
        {
        }

        bool halted(const Symbol& symbol) const noexcept
        {
            return this->exchangeHalt.halted || symbol.tradingHalt.halted;
        }

        bool halted(SymbolId symbolId) const noexcept;
        TradingControlResult setHalt(SymbolId symbolId, bool halted, Timestamp timestamp, std::string reason);
        std::optional<TradingStatusView> query(SymbolId symbolId = 0) const;
    };
}

#endif // ADVMATCH_TRADING_CONTROL_ENGINE_H
