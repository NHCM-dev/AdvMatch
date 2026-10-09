#include "CommandExecutor.h"
#include "advmatch/matching/contracts/ContractEngine.h"

namespace advmatch
{
    namespace
    {
        CommandResult managementResult(bool accepted, std::uint64_t id = 0)
        {
            CommandResult result;
            result.accepted = accepted;
            result.entityId = id;
            result.state = static_cast<std::uint8_t>(accepted ? ManagementState::Success : ManagementState::Rejected);
            return result;
        }

        template <typename Result>
        CommandResult businessResult(CommandResultType type, const Result& value)
        {
            CommandResult result;
            result.type = type;
            result.state = static_cast<std::uint8_t>(value.state);
            result.accepted = static_cast<bool>(value);
            return result;
        }

        CommandResult contractResult(const ContractResult& value)
        {
            auto result = businessResult(CommandResultType::Contract, value);
            result.affected = value.affectedPositions;
            return result;
        }

        class ApprovedHistory : public HistorySink
        {
            DenseSet<OrderId> orders;
            DenseSet<PositionId> positions;

        public:
            explicit ApprovedHistory(const commands::ArchiveHistory& command)
                : orders(command.orders.begin(), command.orders.end()), positions(command.positions.begin(), command.positions.end())
            {
            }

            bool archiveOrder(const BasicOrder& order) noexcept override
            {
                return this->orders.contains(order.orderId);
            }

            bool archivePosition(const Position& position) noexcept override
            {
                return this->positions.contains(position.positionId);
            }
        };

        class Executor
        {
            TradingState& state;
            TradeBuffer& trades;

            ContractEngine& contracts()
            {
                return this->state.matching.getContractEngine();
            }

        public:
            Executor(TradingState& state, TradeBuffer& trades)
                : state(state), trades(trades)
            {
            }

            CommandResult operator()(const commands::CreateAsset& command)
            {
                if (command.scale >= 8)
                {
                    return managementResult(false);
                }

                const auto* asset = this->state.assets.createAsset(command.displayName, command.assetName, command.scale);
                return managementResult(asset != nullptr, asset != nullptr ? asset->assetId : 0);
            }

            CommandResult operator()(const commands::RemoveAsset& command)
            {
                // Asset metadata cannot disappear while any symbol or balance
                // still references it. This is a rare administrative scan.
                bool referenced = false;
                this->state.symbols.forEachSymbol([&](SymbolId, const Symbol& symbol)
                {
                    referenced = referenced || symbol.baseAssetId == command.assetId || symbol.quoteAssetId == command.assetId;
                });
                this->state.users.getAccountManager().forEachRetainedAccount([&](AccountId, const Account& account)
                {
                    referenced = referenced || account.queryActiveBalance(command.assetId).value_or(0) != 0 || account.queryReservedBalance(command.assetId).value_or(0) != 0;
                });
                referenced = referenced || this->state.matching.getSpotSettleEngine().feeBalance(command.assetId) != 0;
                return managementResult(!referenced && this->state.assets.removeAsset(command.assetId));
            }

            CommandResult operator()(const commands::CreateUser&)
            {
                const auto id = this->state.users.createUser();
                return managementResult(id != 0, id);
            }

            CommandResult operator()(const commands::RemoveUser& command)
            {
                return managementResult(this->state.users.removeUser(command.userId));
            }

            CommandResult operator()(const commands::CreateSymbol& command)
            {
                if (!this->state.assets.containsAsset(command.base) || !this->state.assets.containsAsset(command.quote))
                {
                    return managementResult(false);
                }

                const auto* symbol = this->state.symbols.createSymbol(command.name, command.type, command.base, command.quote);
                return managementResult(symbol != nullptr, symbol != nullptr ? symbol->symbolId : 0);
            }

            CommandResult operator()(const commands::RemoveSymbol& command)
            {
                // A live book retains the symbol and may own collateral. Keep
                // removal conservative; product closure is expire/deliver.
                bool hasBook = false;
                this->state.matching.forEachOrderBook([&](const OrderBook& book)
                {
                    hasBook = hasBook || book.symbolId() == command.symbolId;
                });
                return managementResult(!hasBook && this->state.symbols.removeSymbol(command.symbolId));
            }

            CommandResult operator()(const commands::UpdateBalance& command)
            {
                auto* account = this->state.users.getAccountManager().getAccount(command.accountId);
                if (!this->state.assets.containsAsset(command.assetId))
                {
                    return businessResult(CommandResultType::Balance, BalanceResult(BalanceOpResult::NoSuchAsset));
                }

                const auto value = account != nullptr ? account->updateBalance(command.assetId, command.change) : BalanceResult(BalanceOpResult::InvalidAccount);
                if (value)
                {
                    this->state.matching.getAuditEngine().recordExternalFlow(command.assetId, command.change);
                }

                return businessResult(CommandResultType::Balance, value);
            }

            CommandResult operator()(const commands::Transfer& command)
            {
                if (!this->state.assets.containsAsset(command.assetId))
                {
                    return businessResult(CommandResultType::Balance, BalanceResult(BalanceOpResult::NoSuchAsset));
                }

                return businessResult(CommandResultType::Balance, this->state.users.getAccountManager().transfer(command.sourceAccountId, command.targetAccountId, command.assetId, command.amount));
            }

            CommandResult operator()(const commands::AddSpotBook& command)
            {
                return managementResult(this->state.matching.addSpotBook(command.symbolId, command.implementation));
            }

            CommandResult operator()(const commands::AddContractBook& command)
            {
                return contractResult(this->state.matching.addContractBook(command.symbolId, command.spec, command.markPrice, command.timestamp, command.implementation));
            }

            CommandResult operator()(const commands::PlaceOrder& command)
            {
                const auto value = command.contractOptions ? this->state.matching.placeContractOrder(command.userId, command.symbolId, command.side, command.type, command.timeInForce, command.price, command.quantity, command.timestamp, *command.contractOptions, this->trades) : this->state.matching.placeOrder(command.userId, command.symbolId, command.side, command.type, command.timeInForce, command.price, command.quantity, command.timestamp, this->trades);
                CommandResult result;
                result.type = CommandResultType::Place;
                result.accepted = static_cast<bool>(value);
                result.state = static_cast<std::uint8_t>(value.rejectReason.value_or(PlaceOrderState::Success));
                result.entityId = value.orderId;
                result.positionId = value.positionId;
                result.orderStatus = value.state.status;
                result.filled = value.state.filled;
                result.unfilled = value.state.unfilled;
                return result;
            }

            CommandResult operator()(const commands::ModifyPrice& command)
            {
                return businessResult(CommandResultType::Modify, this->state.matching.modifyPrice(command.userId, command.orderId, command.price, command.timestamp, this->trades));
            }

            CommandResult operator()(const commands::ModifyQuantity& command)
            {
                return businessResult(CommandResultType::Modify, this->state.matching.modifyQuantity(command.userId, command.orderId, command.change, command.timestamp));
            }

            CommandResult operator()(const commands::CancelOrder& command)
            {
                return businessResult(CommandResultType::Cancel, this->state.matching.cancelOrder(command.userId, command.orderId, command.timestamp));
            }

            CommandResult operator()(const commands::SetLeverage& command)
            {
                return contractResult(this->contracts().setLeverage(command.positionId, command.userId, command.symbolId, command.leverage, command.timestamp));
            }

            CommandResult operator()(const commands::AddMargin& command)
            {
                return contractResult(this->contracts().addMargin(command.positionId, command.userId, command.symbolId, command.amount));
            }

            CommandResult operator()(const commands::UpdateMarkPrice& command)
            {
                return contractResult(this->contracts().updateMarkPrice(command.symbolId, command.price, command.timestamp));
            }

            CommandResult operator()(const commands::FundInsurance& command)
            {
                auto* account = this->state.users.getAccountManager().getAccount(command.sourceAccountId);
                return contractResult(account != nullptr ? this->contracts().fundInsurance(command.symbolId, *account, command.amount) : ContractResult{ContractState::InsufficientBalance});
            }

            CommandResult operator()(const commands::SettleFunding& command)
            {
                return contractResult(this->contracts().settleFunding(command.symbolId, command.rate, command.timestamp));
            }

            CommandResult operator()(const commands::Expire& command)
            {
                return contractResult(this->contracts().expire(command.symbolId, command.timestamp));
            }

            CommandResult operator()(const commands::Deliver& command)
            {
                return contractResult(this->contracts().deliver(command.symbolId, command.price, command.timestamp));
            }

            CommandResult operator()(const commands::Liquidate& command)
            {
                return contractResult(this->contracts().liquidate(command.symbolId, command.positionId, command.timestamp, this->trades));
            }

            CommandResult operator()(const commands::ProcessLiquidations& command)
            {
                return contractResult(this->contracts().processLiquidations(command.symbolId, command.timestamp, command.force, static_cast<std::size_t>(command.maxCandidates)));
            }

            CommandResult operator()(const commands::SetAutomaticLiquidationBudget& command)
            {
                this->contracts().setAutomaticLiquidationBudget(static_cast<std::size_t>(command.maxCandidates));
                return managementResult(true);
            }

            CommandResult operator()(const commands::ArchiveHistory& command)
            {
                ApprovedHistory sink(command);
                const auto value = this->state.matching.archiveHistory(sink, static_cast<std::size_t>(command.scanBudget));
                auto result = managementResult(true);
                result.type = CommandResultType::Archive;
                result.scannedOrders = value.scannedOrders;
                result.scannedPositions = value.scannedPositions;
                result.archivedOrders = value.archivedOrders;
                result.archivedPositions = value.archivedPositions;
                return result;
            }

            CommandResult operator()(const commands::ConfigureReferencePrice& command)
            {
                return contractResult(this->contracts().configureReference(command.symbolId, command.config));
            }

            CommandResult operator()(const commands::ConfigureContractSchedule& command)
            {
                return contractResult(this->contracts().configureSchedule(command.symbolId, command.schedule, command.timestamp));
            }

            CommandResult operator()(const commands::PublishExternalPrice& command)
            {
                return contractResult(this->contracts().publishPrice(command.symbolId, command.price));
            }

            CommandResult operator()(const commands::RefreshReferencePrice& command)
            {
                return contractResult(this->contracts().refreshReference(command.symbolId, command.timestamp));
            }

            CommandResult operator()(const commands::UpdateReferencePrice& command)
            {
                return contractResult(this->contracts().updateReference(command.symbolId, command.prices, command.timestamp));
            }

            CommandResult conditionalResult(const ConditionalOrderResult& value)
            {
                auto result = businessResult(CommandResultType::Conditional, value);
                result.entityId = value.conditionalOrderId;
                return result;
            }

            CommandResult operator()(const commands::PlaceConditionalOrder& command)
            {
                return this->conditionalResult(this->state.matching.getConditionalOrderEngine().place(command.request));
            }

            CommandResult operator()(const commands::ModifyConditionalOrder& command)
            {
                return this->conditionalResult(this->state.matching.getConditionalOrderEngine().modify(command.userId, command.conditionalOrderId, command.triggerPrice, command.price, command.quantity, command.timestamp));
            }

            CommandResult operator()(const commands::CancelConditionalOrder& command)
            {
                return this->conditionalResult(this->state.matching.getConditionalOrderEngine().cancel(command.userId, command.conditionalOrderId, command.timestamp));
            }

            CommandResult operator()(const commands::ConfigureTradingRules& command)
            {
                return businessResult(CommandResultType::TradingRule, this->state.matching.getTradingRuleEngine().configure(command.symbolId, command.rules));
            }

            CommandResult operator()(const commands::WithdrawSpotFees& command)
            {
                auto* account = this->state.users.getAccountManager().getAccount(command.accountId);
                const auto value = account && this->state.assets.containsAsset(command.assetId) ? this->state.matching.withdrawSpotFees(*account, command.assetId, command.amount) : TradingRuleResult{TradingRuleState::InvalidAccount};
                return businessResult(CommandResultType::TradingRule, value);
            }

            CommandResult operator()(const commands::SetTradingHalt& command)
            {
                return businessResult(CommandResultType::TradingControl, this->state.matching.getTradingControlEngine().setHalt(command.symbolId, command.halted, command.timestamp, command.reason));
            }
        };
    }

    CommandResult executeTradingCommand(TradingState& state, const TradingCommand& command, TradeBuffer& trades)
    {
        trades.clear();
        auto result = std::visit(Executor(state, trades), command);
        state.matching.getConditionalOrderEngine().process();
        state.matching.getConditionalOrderEngine().collectTrades(trades);
        return result;
    }
}
