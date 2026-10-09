#include "TradingControlEngine.h"
#include "advmatch/matching/MatchingEngine.h"

namespace advmatch
{
    bool TradingControlEngine::halted(SymbolId symbolId) const noexcept
    {
        const Symbol* symbol = this->matching.symbolManager.getSymbol(symbolId);
        return this->exchangeHalt.halted || (symbol && symbol->tradingHalt.halted);
    }

    TradingControlResult TradingControlEngine::setHalt(SymbolId symbolId, bool halted, Timestamp timestamp, std::string reason)
    {
        Symbol* symbol = symbolId == 0 ? nullptr : this->matching.symbolManager.getSymbol(symbolId);
        if (symbolId != 0 && (!symbol || !this->matching.findBook(symbolId)))
        {
            return {TradingControlState::InvalidSymbol};
        }

        if (reason.size() > 256)
        {
            return {TradingControlState::InvalidReason};
        }

        TradingHalt& scope = symbol ? symbol->tradingHalt : this->exchangeHalt;
        if (timestamp < scope.updatedTimestamp)
        {
            return {TradingControlState::InvalidTimestamp};
        }

        const bool resuming = scope.halted && !halted;
        scope = {halted, timestamp, halted ? std::move(reason) : std::string{}};
        if (resuming)
        {
            if (symbol)
            {
                if (!this->halted(*symbol))
                {
                    this->matching.getConditionalOrderEngine().resume(symbolId, timestamp);
                }
            }
            else
            {
                this->matching.forEachOrderBook([&](const OrderBook& book)
                {
                    if (!this->halted(book.symbolId()))
                    {
                        this->matching.getConditionalOrderEngine().resume(book.symbolId(), timestamp);
                    }
                });
            }
        }

        return {};
    }

    std::optional<TradingStatusView> TradingControlEngine::query(SymbolId symbolId) const
    {
        const Symbol* symbol = symbolId == 0 ? nullptr : this->matching.symbolManager.getSymbol(symbolId);
        if (symbolId != 0 && (!symbol || !this->matching.findBook(symbolId)))
        {
            return std::nullopt;
        }

        const auto& local = symbol ? symbol->tradingHalt : TradingHalt{};
        return TradingStatusView{symbolId, this->exchangeHalt.halted || local.halted, this->exchangeHalt.halted, local.halted, this->exchangeHalt.updatedTimestamp, local.updatedTimestamp, this->exchangeHalt.reason, local.reason};
    }
}
