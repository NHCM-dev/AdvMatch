#ifndef ADVMATCH_CONDITIONAL_ORDER_CODEC_H
#define ADVMATCH_CONDITIONAL_ORDER_CODEC_H

#include "BinaryCodec.h"
#include "advmatch/trading/conditional/ConditionalOrder.h"

namespace advmatch::ConditionalOrderCodec
{
    inline void write(BinaryWriter& writer, const ConditionalOrderRequest& value)
    {
        writer.number(value.userId);
        writer.number(value.symbolId);
        writer.enumeration(value.side);
        writer.enumeration(value.type);
        writer.enumeration(value.timeInForce);
        writer.number(value.price);
        writer.number(value.quantity);
        writer.number(value.timestamp);
        writer.enumeration(value.kind);
        writer.enumeration(value.source);
        writer.number(value.triggerPrice);
        writer.boolean(value.contractOptions.has_value());
        if (value.contractOptions)
        {
            writer.number(value.contractOptions->leverage);
            writer.number(value.contractOptions->positionId);
            writer.enumeration(value.contractOptions->marginMode);
            writer.boolean(value.contractOptions->reduceOnly);
            writer.number(value.contractOptions->priceLimit);
        }

        writer.number(value.ocoOrderId);
    }

    inline ConditionalOrderRequest read(BinaryReader& reader)
    {
        ConditionalOrderRequest value;
        value.userId = reader.number<UserId>();
        value.symbolId = reader.number<SymbolId>();
        value.side = reader.enumeration(OrderSide::Bid);
        value.type = reader.enumeration(OrderType::Limit);
        value.timeInForce = reader.enumeration(TimeInForce::FOK);
        value.price = reader.number<Price>();
        value.quantity = reader.number<Quantity>();
        value.timestamp = reader.number<Timestamp>();
        value.kind = reader.enumeration(ConditionalOrderKind::TakeProfit);
        value.source = reader.enumeration(TriggerPriceSource::Index);
        value.triggerPrice = reader.number<Price>();
        if (reader.boolean())
        {
            value.contractOptions = ContractOrderOptions{reader.number<LeverageMultiplier>(), reader.number<PositionId>(), reader.enumeration(MarginMode::Cross), reader.boolean(), reader.number<Price>()};
        }

        value.ocoOrderId = reader.number<ConditionalOrderId>();
        return value;
    }
}

#endif // ADVMATCH_CONDITIONAL_ORDER_CODEC_H
