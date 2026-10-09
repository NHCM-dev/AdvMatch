#include "CommandCodec.h"
#include "ReferencePriceCodec.h"
#include "ConditionalOrderCodec.h"
#include "TradingRulesCodec.h"

namespace advmatch
{
    namespace
    {
        class Encoder
        {
            BinaryWriter& writer;

            void spec(const ContractSpec& value)
            {
                this->writer.number(value.contractMultiplier);
                this->writer.number(value.defaultLeverage);
                this->writer.number(value.maxLeverage);
                this->writer.number(value.maintenanceMarginRate);
                this->writer.number(value.makerFeeRate);
                this->writer.number(value.takerFeeRate);
                this->writer.number(value.liquidationFeeRate);
                this->writer.number(value.priceBandRate);
                this->writer.number(value.maxFundingRate);
                this->writer.number(value.maxPositionNotional);
                this->writer.number(value.expiryTimestamp);
                this->writer.number(value.fundingInterval);
                this->writer.number(value.firstFundingTimestamp);
            }

        public:
            void operator()(const commands::SetTradingHalt& value)
            {
                this->writer.number(value.symbolId);
                this->writer.boolean(value.halted);
                this->writer.number(value.timestamp);
                this->writer.string(value.reason);
            }

            void operator()(const commands::ConfigureTradingRules& value)
            {
                this->writer.number(value.symbolId);
                TradingRulesCodec::write(this->writer, value.rules);
            }

            void operator()(const commands::WithdrawSpotFees& value)
            {
                this->writer.number(value.accountId);
                this->writer.number(value.assetId);
                this->writer.number(value.amount);
            }

            explicit Encoder(BinaryWriter& writer)
                : writer(writer)
            {
            }

            void operator()(const commands::PlaceConditionalOrder& value)
            {
                ConditionalOrderCodec::write(this->writer, value.request);
            }

            void operator()(const commands::ModifyConditionalOrder& value)
            {
                this->writer.number(value.userId);
                this->writer.number(value.conditionalOrderId);
                this->writer.number(value.triggerPrice);
                this->writer.number(value.price);
                this->writer.number(value.quantity);
                this->writer.number(value.timestamp);
            }

            void operator()(const commands::CancelConditionalOrder& value)
            {
                this->writer.number(value.userId);
                this->writer.number(value.conditionalOrderId);
                this->writer.number(value.timestamp);
            }

            void operator()(const commands::CreateAsset& value)
            {
                writer.string(value.displayName);
                writer.string(value.assetName);
                writer.number(value.scale);
            }

            void operator()(const commands::RemoveAsset& value)
            {
                writer.number(value.assetId);
            }

            void operator()(const commands::CreateUser&)
            {
            }

            void operator()(const commands::RemoveUser& value)
            {
                writer.number(value.userId);
            }

            void operator()(const commands::CreateSymbol& value)
            {
                writer.string(value.name);
                writer.enumeration(value.type);
                writer.number(value.base);
                writer.number(value.quote);
            }

            void operator()(const commands::RemoveSymbol& value)
            {
                writer.number(value.symbolId);
            }

            void operator()(const commands::UpdateBalance& value)
            {
                writer.number(value.accountId);
                writer.number(value.assetId);
                writer.number(value.change);
            }

            void operator()(const commands::Transfer& value)
            {
                writer.number(value.sourceAccountId);
                writer.number(value.targetAccountId);
                writer.number(value.assetId);
                writer.number(value.amount);
            }

            void operator()(const commands::AddSpotBook& value)
            {
                writer.number(value.symbolId);
                writer.enumeration(value.implementation);
            }

            void operator()(const commands::AddContractBook& value)
            {
                writer.number(value.symbolId);
                spec(value.spec);
                writer.number(value.markPrice);
                writer.number(value.timestamp);
                writer.enumeration(value.implementation);
            }

            void operator()(const commands::PlaceOrder& value)
            {
                writer.number(value.userId);
                writer.number(value.symbolId);
                writer.enumeration(value.side);
                writer.enumeration(value.type);
                writer.enumeration(value.timeInForce);
                writer.number(value.price);
                writer.number(value.quantity);
                writer.number(value.timestamp);
                writer.boolean(value.contractOptions.has_value());
                if (value.contractOptions)
                {
                    const auto& options = *value.contractOptions;
                    writer.number(options.leverage);
                    writer.number(options.positionId);
                    writer.enumeration(options.marginMode);
                    writer.boolean(options.reduceOnly);
                    writer.number(options.priceLimit);
                }
            }

            void operator()(const commands::ModifyPrice& value)
            {
                writer.number(value.userId);
                writer.number(value.orderId);
                writer.number(value.price);
                writer.number(value.timestamp);
            }

            void operator()(const commands::ModifyQuantity& value)
            {
                writer.number(value.userId);
                writer.number(value.orderId);
                writer.number(value.change);
                writer.number(value.timestamp);
            }

            void operator()(const commands::CancelOrder& value)
            {
                writer.number(value.userId);
                writer.number(value.orderId);
                writer.number(value.timestamp);
            }

            void operator()(const commands::SetLeverage& value)
            {
                writer.number(value.positionId);
                writer.number(value.userId);
                writer.number(value.symbolId);
                writer.number(value.leverage);
                writer.number(value.timestamp);
            }

            void operator()(const commands::AddMargin& value)
            {
                writer.number(value.positionId);
                writer.number(value.userId);
                writer.number(value.symbolId);
                writer.number(value.amount);
            }

            void operator()(const commands::UpdateMarkPrice& value)
            {
                writer.number(value.symbolId);
                writer.number(value.price);
                writer.number(value.timestamp);
            }

            void operator()(const commands::FundInsurance& value)
            {
                writer.number(value.symbolId);
                writer.number(value.sourceAccountId);
                writer.number(value.amount);
            }

            void operator()(const commands::SettleFunding& value)
            {
                writer.number(value.symbolId);
                writer.number(value.rate);
                writer.number(value.timestamp);
            }

            void operator()(const commands::Expire& value)
            {
                writer.number(value.symbolId);
                writer.number(value.timestamp);
            }

            void operator()(const commands::Deliver& value)
            {
                writer.number(value.symbolId);
                writer.number(value.price);
                writer.number(value.timestamp);
            }

            void operator()(const commands::Liquidate& value)
            {
                writer.number(value.symbolId);
                writer.number(value.positionId);
                writer.number(value.timestamp);
            }

            void operator()(const commands::ProcessLiquidations& value)
            {
                writer.number(value.symbolId);
                writer.number(value.timestamp);
                writer.boolean(value.force);
                writer.number(value.maxCandidates);
            }

            void operator()(const commands::SetAutomaticLiquidationBudget& value)
            {
                writer.number(value.maxCandidates);
            }

            void operator()(const commands::ArchiveHistory& value)
            {
                writer.number(value.scanBudget);
                writer.number<std::uint64_t>(value.orders.size());
                for (auto id : value.orders)
                {
                    writer.number(id);
                }

                writer.number<std::uint64_t>(value.positions.size());
                for (auto id : value.positions)
                {
                    writer.number(id);
                }
            }

            void operator()(const commands::ConfigureReferencePrice& value)
            {
                writer.number(value.symbolId);
                ReferencePriceCodec::write(writer, value.config);
            }

            void operator()(const commands::ConfigureContractSchedule& value)
            {
                writer.number(value.symbolId);
                ReferencePriceCodec::write(writer, value.schedule);
                writer.number(value.timestamp);
            }

            void operator()(const commands::PublishExternalPrice& value)
            {
                writer.number(value.symbolId);
                ReferencePriceCodec::write(writer, value.price);
            }

            void operator()(const commands::RefreshReferencePrice& value)
            {
                writer.number(value.symbolId);
                writer.number(value.timestamp);
            }

            void operator()(const commands::UpdateReferencePrice& value)
            {
                writer.number(value.symbolId);
                ReferencePriceCodec::write(writer, value.prices);
                writer.number(value.timestamp);
            }
        };

        ContractSpec readSpec(BinaryReader& reader)
        {
            ContractSpec value;
            value.contractMultiplier = reader.number<Quantity>();
            value.defaultLeverage = reader.number<LeverageMultiplier>();
            value.maxLeverage = reader.number<LeverageMultiplier>();
            value.maintenanceMarginRate = reader.number<MMRRate>();
            value.makerFeeRate = reader.number<MMRRate>();
            value.takerFeeRate = reader.number<MMRRate>();
            value.liquidationFeeRate = reader.number<MMRRate>();
            value.priceBandRate = reader.number<MMRRate>();
            value.maxFundingRate = reader.number<MMRRate>();
            value.maxPositionNotional = reader.number<Balance>();
            value.expiryTimestamp = reader.number<Timestamp>();
            value.fundingInterval = reader.number<Timestamp>();
            value.firstFundingTimestamp = reader.number<Timestamp>();
            return value;
        }

        TradingCommand readCommand(BinaryReader& reader, std::uint16_t operation)
        {
            switch (operation)
            {
                case 0:
                    return commands::CreateAsset{reader.string(), reader.string(), reader.number<Scale>()};
                case 1:
                    return commands::RemoveAsset{reader.number<AssetId>()};
                case 2:
                    return commands::CreateUser{};
                case 3:
                    return commands::RemoveUser{reader.number<UserId>()};
                case 4:
                    return commands::CreateSymbol{reader.string(), reader.enumeration(SymbolType::Option), reader.number<AssetId>(), reader.number<AssetId>()};
                case 5:
                    return commands::RemoveSymbol{reader.number<SymbolId>()};
                case 6:
                    return commands::UpdateBalance{reader.number<AccountId>(), reader.number<AssetId>(), reader.number<BalanceChange>()};
                case 7:
                    return commands::Transfer{reader.number<AccountId>(), reader.number<AccountId>(), reader.number<AssetId>(), reader.number<PositiveBalanceChange>()};
                case 8:
                    return commands::AddSpotBook{reader.number<SymbolId>(), reader.enumeration(OrderBookImplementation::IndexedTree)};
                case 9:
                    return commands::AddContractBook{reader.number<SymbolId>(), readSpec(reader), reader.number<Price>(), reader.number<Timestamp>(), reader.enumeration(OrderBookImplementation::IndexedTree)};
                case 10:
                {
                    commands::PlaceOrder value{reader.number<UserId>(), reader.number<SymbolId>(), reader.enumeration(OrderSide::Bid), reader.enumeration(OrderType::Limit), reader.enumeration(TimeInForce::PostOnly), reader.number<Price>(), reader.number<Quantity>(), reader.number<Timestamp>()};
                    if (reader.boolean())
                    {
                        value.contractOptions = ContractOrderOptions{reader.number<LeverageMultiplier>(), reader.number<PositionId>(), reader.enumeration(MarginMode::Cross), reader.boolean(), reader.number<Price>()};
                    }

                    return value;
                }
                case 11:
                    return commands::ModifyPrice{reader.number<UserId>(), reader.number<OrderId>(), reader.number<Price>(), reader.number<Timestamp>()};
                case 12:
                    return commands::ModifyQuantity{reader.number<UserId>(), reader.number<OrderId>(), reader.number<QuantityChange>(), reader.number<Timestamp>()};
                case 13:
                    return commands::CancelOrder{reader.number<UserId>(), reader.number<OrderId>(), reader.number<Timestamp>()};
                case 14:
                    return commands::SetLeverage{reader.number<PositionId>(), reader.number<UserId>(), reader.number<SymbolId>(), reader.number<LeverageMultiplier>(), reader.number<Timestamp>()};
                case 15:
                    return commands::AddMargin{reader.number<PositionId>(), reader.number<UserId>(), reader.number<SymbolId>(), reader.number<BalanceChange>()};
                case 16:
                    return commands::UpdateMarkPrice{reader.number<SymbolId>(), reader.number<Price>(), reader.number<Timestamp>()};
                case 17:
                    return commands::FundInsurance{reader.number<SymbolId>(), reader.number<AccountId>(), reader.number<PositiveBalanceChange>()};
                case 18:
                    return commands::SettleFunding{reader.number<SymbolId>(), reader.number<std::int32_t>(), reader.number<Timestamp>()};
                case 19:
                    return commands::Expire{reader.number<SymbolId>(), reader.number<Timestamp>()};
                case 20:
                    return commands::Deliver{reader.number<SymbolId>(), reader.number<Price>(), reader.number<Timestamp>()};
                case 21:
                    return commands::Liquidate{reader.number<SymbolId>(), reader.number<PositionId>(), reader.number<Timestamp>()};
                case 22:
                    return commands::ProcessLiquidations{reader.number<SymbolId>(), reader.number<Timestamp>(), reader.boolean(), reader.number<std::uint64_t>()};
                case 23:
                    return commands::SetAutomaticLiquidationBudget{reader.number<std::uint64_t>()};
                case 24:
                {
                    commands::ArchiveHistory value{reader.number<std::uint64_t>(), {}, {}};
                    const auto orders = reader.count(sizeof(OrderId));
                    for (std::size_t index = 0; index < orders; ++index)
                    {
                        value.orders.push_back(reader.number<OrderId>());
                    }

                    const auto positions = reader.count(sizeof(PositionId));
                    for (std::size_t index = 0; index < positions; ++index)
                    {
                        value.positions.push_back(reader.number<PositionId>());
                    }

                    return value;
                }
                case 25:
                    return commands::ConfigureReferencePrice{reader.number<SymbolId>(), ReferencePriceCodec::readReferencePriceConfig(reader)};
                case 26:
                    return commands::ConfigureContractSchedule{reader.number<SymbolId>(), ReferencePriceCodec::readContractSchedule(reader), reader.number<Timestamp>()};
                case 27:
                    return commands::PublishExternalPrice{reader.number<SymbolId>(), ReferencePriceCodec::readExternalPrice(reader)};
                case 28:
                    return commands::RefreshReferencePrice{reader.number<SymbolId>(), reader.number<Timestamp>()};
                case 29:
                    return commands::UpdateReferencePrice{reader.number<SymbolId>(), ReferencePriceCodec::readReferencePrices(reader), reader.number<Timestamp>()};
                case 30:
                    return commands::PlaceConditionalOrder{ConditionalOrderCodec::read(reader)};
                case 31:
                    return commands::ModifyConditionalOrder{reader.number<UserId>(), reader.number<ConditionalOrderId>(), reader.number<Price>(), reader.number<Price>(), reader.number<Quantity>(), reader.number<Timestamp>()};
                case 32:
                    return commands::CancelConditionalOrder{reader.number<UserId>(), reader.number<ConditionalOrderId>(), reader.number<Timestamp>()};
                case 33:
                    return commands::ConfigureTradingRules{reader.number<SymbolId>(), TradingRulesCodec::read(reader)};
                case 34:
                    return commands::WithdrawSpotFees{reader.number<AccountId>(), reader.number<AssetId>(), reader.number<PositiveBalanceChange>()};
                case 35:
                    return commands::SetTradingHalt{reader.number<SymbolId>(), reader.boolean(), reader.number<Timestamp>(), reader.string()};
                default: throw PersistenceError(PersistenceFailure::UnsupportedVersion, "unknown trading operation");
            }
        }
    }

    std::vector<std::byte> encodeCommand(const TradingCommand& command, std::uint64_t requestId)
    {
        BinaryWriter writer;
        writer.number<std::uint16_t>(1);
        writer.number(requestId);
        writer.number<std::uint16_t>(static_cast<std::uint16_t>(command.index()));
        std::visit(Encoder(writer), command);
        return writer.take();
    }

    CommandEnvelope decodeCommand(std::span<const std::byte> bytes)
    {
        BinaryReader reader(bytes);
        persistenceCheck(reader.number<std::uint16_t>() == 1, "unsupported command schema", PersistenceFailure::UnsupportedVersion);
        const auto requestId = reader.number<std::uint64_t>();
        auto command = readCommand(reader, reader.number<std::uint16_t>());
        reader.finish();
        return {requestId, std::move(command)};
    }

    std::vector<std::byte> encodeResult(const CommandResult& result)
    {
        BinaryWriter writer;
        writer.enumeration(result.type);
        writer.number(result.state);
        writer.boolean(result.accepted);
        writer.number(result.entityId);
        writer.number(result.positionId);
        writer.enumeration(result.orderStatus);
        writer.number(result.filled);
        writer.number(result.unfilled);
        writer.number(result.affected);
        writer.number(result.scannedOrders);
        writer.number(result.scannedPositions);
        writer.number(result.archivedOrders);
        writer.number(result.archivedPositions);
        return writer.take();
    }

    CommandResult decodeResult(std::span<const std::byte> bytes)
    {
        BinaryReader reader(bytes);
        CommandResult result;
        result.type = reader.enumeration(CommandResultType::TradingControl);
        result.state = reader.number<std::uint8_t>();
        result.accepted = reader.boolean();
        result.entityId = reader.number<std::uint64_t>();
        result.positionId = reader.number<PositionId>();
        result.orderStatus = reader.enumeration(OrderStatus::Cancelled);
        result.filled = reader.number<Quantity>();
        result.unfilled = reader.number<Quantity>();
        result.affected = reader.number<std::uint64_t>();
        result.scannedOrders = reader.number<std::uint64_t>();
        result.scannedPositions = reader.number<std::uint64_t>();
        result.archivedOrders = reader.number<std::uint64_t>();
        result.archivedPositions = reader.number<std::uint64_t>();
        reader.finish();
        return result;
    }
}
