#include "AuditEngine.h"
#include "advmatch/matching/MatchingEngine.h"
#include "advmatch/matching/contracts/ContractEngine.h"
#include "advmatch/management/AssetManager.h"

#include <algorithm>
#include <tuple>

namespace advmatch::audit
{
    namespace
    {
        std::string decimal(WideBalance value)
        {
            const bool negative = value < 0;
            if (negative)
            {
                value = -value;
            }

            std::string result;
            do
            {
                result.push_back(static_cast<char>('0' + static_cast<unsigned>(value % 10)));
                value /= 10;
            }
            while (value != 0);

            if (negative)
            {
                result.push_back('-');
            }

            std::reverse(result.begin(), result.end());
            return result;
        }

        struct Fingerprint
        {
            std::uint64_t value = 14695981039346656037ULL;

            template <typename... Values>
            void add(Values... values)
            {
                const auto number = [&](std::uint64_t item)
                {
                    for (unsigned byte = 0; byte < 8; ++byte)
                    {
                        this->value ^= (item >> (byte * 8)) & 255;
                        this->value *= 1099511628211ULL;
                    }
                };
                (number(static_cast<std::uint64_t>(values)), ...);
            }

            void text(const std::string& text)
            {
                this->add(text.size());
                for (unsigned char byte : text)
                {
                    this->value ^= byte;
                    this->value *= 1099511628211ULL;
                }
            }
        };

        struct Totals
        {
            WideBalance active = 0;
            WideBalance reserved = 0;
            WideBalance fees = 0;
            WideBalance insurance = 0;
            WideBalance clearing = 0;
            WideBalance funding = 0;
        };

        struct CrossTotals
        {
            WideBalance initial = 0;
            WideBalance maintenance = 0;
            WideBalance closeFee = 0;
            WideBalance pnl = 0;
            WideBalance orderReservation = 0;
        };
    }

    AuditReport AuditEngine::report(const AssetManager& assets, std::size_t maxIssues) const
    {
        AuditReport report;
        Fingerprint hash;
        DenseMap<AssetId, Totals> totals;
        DenseMap<AccountId, DenseMap<AssetId, WideBalance>> reservations;
        DenseMap<AccountId, DenseMap<AssetId, CrossTotals>> cross;
        DenseMap<TradingRiskState*, TradingRiskState> risks;
        const auto issue = [&](AuditIssueType type, std::uint64_t id, AssetId assetId, const char* message)
        {
            ++report.issueCount;
            report.consistent = false;
            if (report.issues.size() < maxIssues)
            {
                report.issues.push_back({type, id, assetId, message});
            }
        };

        assets.forEachAsset([&](AssetId id, const Asset&) { totals.try_emplace(id); });
        for (const auto& [id, flow] : this->externalFlows)
        {
            totals.try_emplace(id);
        }

        for (const OrderNode* node : this->matching.orders)
        {
            if (!node)
            {
                continue;
            }

            ++report.checkedOrders;
            const auto& order = node->basicOrder;
            hash.add(order.orderId, order.userId, order.symbolId, order.orderSide, order.orderType, order.timeInForce, order.getPrice(), order.getQuantity(), order.getFilledQuantity(), order.getUnfilledQuantity(), order.getStatus(), order.getCreatedTimestamp(), order.getUpdatedTimestamp(), order.getFirstFilledTimestamp(), order.getLastFilledTimestamp(), order.getCompletedTimestamp(), node->reservedBalance, node->leverage, node->reduceOnly, node->liquidationOrder, node->executionPriceLimit, node->makerFeeRate, node->takerFeeRate, node->paidFees, node->selfTradePrevention, node->riskPendingNotional, node->riskAcquired);
            if (node->reservedBalance != 0)
            {
                reservations[node->account->getAccountId()][node->reservationAssetId] += node->reservedBalance;
            }

            if (node->reservedBalance < 0 || (isOrderStatusTerminal(order.getStatus()) && (node->reservedBalance != 0 || node->priceLevel)) || (node->priceLevel && (order.orderType != OrderType::Limit || order.getUnfilledQuantity() == 0)))
            {
                issue(AuditIssueType::Reservation, order.orderId, node->reservationAssetId, "invalid order reservation or resting state");
            }

            if (node->contractParticipant && node->contractParticipant->position->marginMode == MarginMode::Cross)
            {
                cross[node->account->getAccountId()][node->quoteAssetId].orderReservation += node->reservedBalance;
            }

            if (node->riskAcquired)
            {
                auto& state = risks[node->riskState];
                ++state.activeOrders;
                state.pendingNotional += node->riskPendingNotional;
            }
        }

        const auto& contracts = this->matching.getContractEngine();
        std::vector<const ContractParticipant*> positions;
        for (const auto* participant : contracts.positionIndex)
        {
            positions.push_back(participant);
        }

        std::sort(positions.begin(), positions.end(), [](const auto* left, const auto* right) { return left->position->positionId < right->position->positionId; });
        for (const auto* participant : positions)
        {
            const auto& position = *participant->position;
            ++report.checkedPositions;
            hash.add(position.positionId, position.userId, position.symbolId, position.positionSide, position.marginMode, position.positionSize, position.entryNotional, position.initialMargin, position.maintenanceMargin, position.estimatedCloseFee, position.updatedUnrealizedPnl, position.realizedPnL, position.fundingPnL, position.paidFees, position.closed, position.liquidating, position.leverage, position.createdTimestamp, position.updatedTimestamp, participant->openOrderCount, participant->orderReferences);
            if (position.marginMode == MarginMode::Isolated && position.initialMargin != 0)
            {
                reservations[participant->account->getAccountId()][participant->market->symbol->quoteAssetId] += position.initialMargin;
            }
            else if (position.marginMode == MarginMode::Cross)
            {
                auto& state = cross[participant->account->getAccountId()][participant->market->symbol->quoteAssetId];
                state.initial += position.initialMargin;
                state.maintenance += position.maintenanceMargin;
                state.closeFee += position.estimatedCloseFee;
                state.pnl += position.updatedUnrealizedPnl;
            }

            if (participant->riskState)
            {
                auto& state = risks[participant->riskState];
                const WideBalance notional = static_cast<WideBalance>(state.positionNotional) + position.entryNotional;
                if (notional > std::numeric_limits<Balance>::max())
                {
                    issue(AuditIssueType::Risk, position.positionId, participant->market->symbol->quoteAssetId, "risk aggregate overflow");
                }
                else
                {
                    state.positionNotional = static_cast<Balance>(notional);
                }
            }
        }

        this->matching.userManager.getAccountManager().forEachRetainedAccount([&](AccountId id, const Account& account)
        {
            ++report.checkedAccounts;
            hash.add(id, account.getUserId(), account.getAccountType(), account.isActive());
            std::vector<std::tuple<AssetId, Balance, Balance>> balances;
            account.forEachBalance([&](AssetId assetId, const auto& balance) { balances.emplace_back(assetId, balance.active, balance.reserved); });
            std::sort(balances.begin(), balances.end());
            if (const auto expected = reservations.find(id); expected != reservations.end())
            {
                for (const auto& [assetId, amount] : expected->second)
                {
                    if (!account.queryReservedBalance(assetId) && amount != 0)
                    {
                        issue(AuditIssueType::Reservation, id, assetId, "reservation has no account balance");
                    }
                }
            }

            for (const auto& [assetId, active, reserved] : balances)
            {
                hash.add(assetId, active, reserved);
                auto& total = totals[assetId];
                total.active += active;
                total.reserved += reserved;
                if (active < 0 || reserved < 0)
                {
                    issue(AuditIssueType::NegativeBalance, id, assetId, "negative user balance");
                }

                const auto owner = reservations.find(id);
                const WideBalance expected = owner != reservations.end() && owner->second.contains(assetId) ? owner->second.at(assetId) : 0;
                if (reserved != expected)
                {
                    issue(AuditIssueType::Reservation, id, assetId, "reserved balance differs from orders and isolated margins");
                }

                if (account.getAccountType() == AccountType::Contract)
                {
                    const auto* state = static_cast<const ContractAccount&>(account).findCrossMargin(assetId);
                    const auto found = cross.find(id);
                    const CrossTotals expected = found != cross.end() && found->second.contains(assetId) ? found->second.at(assetId) : CrossTotals{};
                    if (state && (state->initialMargin != expected.initial || state->maintenanceMargin != expected.maintenance || state->liquidationFeeReserve != expected.closeFee || state->unrealizedPnL != expected.pnl || state->orderReservation != expected.orderReservation))
                    {
                        issue(AuditIssueType::Reservation, id, assetId, "cross-margin aggregates differ from positions and orders");
                    }
                }
            }
        });

        for (const auto& states : this->matching.getTradingRuleEngine().userStates)
        {
            if (!states)
            {
                continue;
            }

            for (const auto& [userId, state] : *states)
            {
                const auto found = risks.find(const_cast<TradingRiskState*>(&state));
                const TradingRiskState expected = found == risks.end() ? TradingRiskState{} : found->second;
                if (state.activeOrders != expected.activeOrders || state.pendingNotional != expected.pendingNotional || state.positionNotional != expected.positionNotional)
                {
                    issue(AuditIssueType::Risk, userId, 0, "risk counters differ from retained orders and positions");
                }
            }
        }

        std::vector<const Symbol*> symbols;
        for (const auto& [id, symbol] : this->matching.symbolManager)
        {
            symbols.push_back(&symbol);
        }

        std::sort(symbols.begin(), symbols.end(), [](const auto* left, const auto* right) { return left->symbolId < right->symbolId; });
        for (const auto* symbol : symbols)
        {
            const auto& rules = symbol->tradingRules;
            hash.add(symbol->symbolId, symbol->symbolType, symbol->baseAssetId, symbol->quoteAssetId, symbol->isActive(), rules.priceTick, rules.quantityStep, rules.minQuantity, rules.minOrderNotional, rules.fees.makerRate, rules.fees.takerRate, rules.risk.maxOrderQuantity, rules.risk.maxOrderNotional, rules.risk.maxPositionNotional, rules.risk.maxAccountNotional, rules.risk.maxActiveOrders, rules.risk.maxLeverage, rules.selfTradePrevention);
            hash.text(symbol->name);
            if (const auto* market = contracts.findMarket(symbol->symbolId))
            {
                auto& total = totals[symbol->quoteAssetId];
                total.fees += market->feeBalance;
                total.insurance += market->insuranceBalance;
                total.clearing += market->clearingBalance;
                total.funding += market->fundingRoundingBalance;
                hash.add(market->status, market->markPrice, market->markTimestamp, market->commandTimestamp, market->nextFundingTimestamp, market->lastFundingTimestamp, market->deliveryPrice, market->feeBalance, market->insuranceBalance, market->clearingBalance, market->fundingRoundingBalance, market->referencePrices.indexPrice, market->referencePrices.markPrice, market->referencePrices.premiumRate, market->referencePrices.fundingRate, market->referenceTimestamp);
            }

            const auto* book = this->matching.findBook(symbol->symbolId);
            if (!book)
            {
                continue;
            }

            for (OrderSide side : {OrderSide::Bid, OrderSide::Ask})
            {
                DenseMap<Price, std::pair<WideBalance, std::size_t>> levels;
                book->forEachOrder(side, [&](const BasicOrder& order)
                {
                    auto& value = levels[order.getPrice()];
                    value.first += order.getUnfilledQuantity();
                    ++value.second;
                    hash.add(order.orderId);
                    const auto* node = this->matching.findNode(order.orderId);
                    if (!node || !node->priceLevel || node->priceLevel->price != order.getPrice() || order.orderSide != side)
                    {
                        issue(AuditIssueType::Book, symbol->symbolId, 0, "book order does not reference its price level");
                    }
                });
                book->forEachLevel(side, [&](const Quote& quote)
                {
                    const auto found = levels.find(quote.price);
                    if (found == levels.end() || found->second.first != quote.quantity || found->second.second != quote.orderCount)
                    {
                        issue(AuditIssueType::Book, symbol->symbolId, 0, "price-level aggregates differ from FIFO orders");
                    }

                    levels.erase(quote.price);
                    hash.add(quote.price, quote.quantity, quote.orderCount);
                });
                if (!levels.empty())
                {
                    issue(AuditIssueType::Book, symbol->symbolId, 0, "book has an order without an indexed price level");
                }
            }
        }

        const auto& conditional = this->matching.getConditionalOrderEngine();
        for (std::size_t index = 0; index < conditional.size(); ++index)
        {
            const auto value = *conditional.query(index + 1);
            const auto& request = value.request;
            hash.add(value.conditionalOrderId, request.userId, request.symbolId, request.side, request.type, request.timeInForce, request.price, request.quantity, request.timestamp, request.kind, request.source, request.triggerPrice, request.ocoOrderId, value.status, value.orderId, value.positionId, value.updatedTimestamp, value.triggeredPrice, value.rejectReason.has_value());
            if (value.rejectReason)
            {
                hash.add(*value.rejectReason);
            }
        }

        std::vector<AssetId> ids;
        for (const auto& [id, value] : totals)
        {
            ids.push_back(id);
        }

        std::sort(ids.begin(), ids.end());
        for (AssetId id : ids)
        {
            const auto& total = totals.at(id);
            const WideBalance flow = this->externalFlows.contains(id) ? this->externalFlows.at(id) : 0;
            const Balance spotFees = this->matching.getSpotSettleEngine().feeBalance(id);
            const WideBalance actual = total.active + total.reserved + total.fees + total.insurance + total.clearing + total.funding + spotFees;
            const bool conserved = actual == flow;
            report.assets.push_back({id, decimal(flow), decimal(total.active), decimal(total.reserved), decimal(spotFees), decimal(total.fees), decimal(total.insurance), decimal(total.clearing), decimal(total.funding), decimal(actual), decimal(actual - flow), conserved});
            if (!conserved)
            {
                report.conserved = false;
                issue(AuditIssueType::Conservation, id, id, "account and house assets differ from net external flows");
            }

            hash.add(id, spotFees);
            hash.text(decimal(flow));
            hash.text(decimal(actual));
        }

        report.stateHash = hash.value;
        return report;
    }
}
