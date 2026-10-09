#ifndef ADVMATCH_TRADING_COMMAND_VALIDATOR_H
#define ADVMATCH_TRADING_COMMAND_VALIDATOR_H

#include "../model/TradingCommand.h"
#include "advmatch/persistence/codec/BinaryCodec.h"

namespace advmatch::api::command
{
    // Wire/schema validation belongs at admission, business rules stay in the
    // factories and risk engines. This avoids encode/decode for in-memory use.
    class TradingCommandValidator
    {
        template <typename Enum>
        static void enumeration(Enum value, Enum maximum)
        {
            persistenceCheck(static_cast<std::uint8_t>(value) <= static_cast<std::uint8_t>(maximum), "invalid enum");
        }

    public:
        static bool validateWithoutEncoding(const TradingCommand& command)
        {
            return std::visit([](const auto& value)
            {
                using Command = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<Command, commands::CreateAsset> || std::is_same_v<Command, commands::CreateSymbol> ||
                    std::is_same_v<Command, commands::ArchiveHistory> || std::is_same_v<Command, commands::ConfigureReferencePrice>)
                {
                    // Variable payloads still need their exact configured byte
                    // limit checked. All other commands have fixed wire sizes.
                    return false;
                }
                else
                {
                    if constexpr (std::is_same_v<Command, commands::PlaceConditionalOrder>)
                    {
                        enumeration(value.request.side, OrderSide::Bid);
                        enumeration(value.request.type, OrderType::Limit);
                        enumeration(value.request.timeInForce, TimeInForce::PostOnly);
                        enumeration(value.request.kind, ConditionalOrderKind::TakeProfit);
                        enumeration(value.request.source, TriggerPriceSource::Index);
                        if (value.request.contractOptions)
                        {
                            enumeration(value.request.contractOptions->marginMode, MarginMode::Cross);
                        }
                    }
                    else if constexpr (std::is_same_v<Command, commands::PlaceOrder>)
                    {
                        enumeration(value.side, OrderSide::Bid);
                        enumeration(value.type, OrderType::Limit);
                        enumeration(value.timeInForce, TimeInForce::PostOnly);
                        if (value.contractOptions)
                        {
                            enumeration(value.contractOptions->marginMode, MarginMode::Cross);
                        }
                    }
                    else if constexpr (std::is_same_v<Command, commands::AddSpotBook> || std::is_same_v<Command, commands::AddContractBook>)
                    {
                        enumeration(value.implementation, OrderBookImplementation::IndexedTree);
                    }
                    else if constexpr (std::is_same_v<Command, commands::ConfigureTradingRules>)
                    {
                        enumeration(value.rules.selfTradePrevention, SelfTradePrevention::CancelBoth);
                    }

                    return true;
                }
            }, command);
        }
    };
}

#endif // ADVMATCH_TRADING_COMMAND_VALIDATOR_H
