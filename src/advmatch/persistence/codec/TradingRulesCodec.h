#ifndef ADVMATCH_TRADING_RULES_CODEC_H
#define ADVMATCH_TRADING_RULES_CODEC_H

#include "BinaryCodec.h"
#include "advmatch/trading/rules/TradingRules.h"

namespace advmatch::TradingRulesCodec
{
    inline void write(BinaryWriter& writer, const TradingRules& rules)
    {
        writer.number(rules.priceTick);
        writer.number(rules.quantityStep);
        writer.number(rules.minQuantity);
        writer.number(rules.minOrderNotional);
        writer.number(rules.fees.makerRate);
        writer.number(rules.fees.takerRate);
        writer.number(rules.risk.maxOrderQuantity);
        writer.number(rules.risk.maxOrderNotional);
        writer.number(rules.risk.maxPositionNotional);
        writer.number(rules.risk.maxAccountNotional);
        writer.number(rules.risk.maxActiveOrders);
        writer.number(rules.risk.maxLeverage);
        writer.enumeration(rules.selfTradePrevention);
        writer.boolean(rules.tickAwarePriceBand);
    }

    inline TradingRules read(BinaryReader& reader, bool extended = true)
    {
        TradingRules rules;
        rules.priceTick = reader.number<Price>();
        rules.quantityStep = reader.number<Quantity>();
        rules.minQuantity = reader.number<Quantity>();
        rules.minOrderNotional = reader.number<Balance>();
        rules.fees = {reader.number<MMRRate>(), reader.number<MMRRate>()};
        rules.risk = {reader.number<Quantity>(), reader.number<Balance>(), reader.number<Balance>(), reader.number<Balance>(), reader.number<std::uint32_t>(), reader.number<LeverageMultiplier>()};
        rules.selfTradePrevention = reader.enumeration(SelfTradePrevention::CancelBoth);
        rules.tickAwarePriceBand = extended ? reader.boolean() : false;
        return rules;
    }
}

#endif // ADVMATCH_TRADING_RULES_CODEC_H
