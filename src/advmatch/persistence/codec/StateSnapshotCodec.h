#ifndef ADVMATCH_STATE_SNAPSHOT_CODEC_H
#define ADVMATCH_STATE_SNAPSHOT_CODEC_H

#include "../command/CommandExecutor.h"
#include "BinaryCodec.h"
#include "ConditionalOrderCodec.h"
#include "ReferencePriceCodec.h"
#include "TradingRulesCodec.h"
#include "advmatch/matching/contracts/ContractEngine.h"

namespace advmatch
{
    // Explicit fields and fixed-width encoding; no object memory or pointers are persisted.
    class StateSnapshotCodec
    {
        template <typename Archive, typename Value> static void scalar(Archive& archive, Value& value)
        {
            using Type = std::remove_cv_t<Value>;
            if constexpr (std::is_same_v<Archive, BinaryWriter>)
            {
                if constexpr (std::is_same_v<Type, bool>)
                {
                    archive.boolean(value);
                }
                else if constexpr (std::is_enum_v<Type>)
                {
                    archive.enumeration(value);
                }
                else if constexpr (std::is_same_v<Type, std::string>)
                {
                    archive.string(value);
                }
                else
                {
                    archive.number(value);
                }
            }
            else
            {
                if constexpr (std::is_same_v<Type, bool>)
                {
                    value = archive.boolean();
                }
                else if constexpr (std::is_enum_v<Type>)
                {
                    value = static_cast<Type>(archive.template number<std::uint8_t>());
                }
                else if constexpr (std::is_same_v<Type, std::string>)
                {
                    value = archive.string();
                }
                else
                {
                    value = archive.template number<Type>();
                }
            }
        }

        template <typename Archive, typename... Values> static void fields(Archive& archive, Values&... values)
        {
            (scalar(archive, values), ...);
        }

        template <typename Archive> static void spec(Archive& a, ContractSpec& v)
        {
            fields(a, v.contractMultiplier, v.defaultLeverage, v.maxLeverage, v.maintenanceMarginRate, v.makerFeeRate, v.takerFeeRate,
                   v.liquidationFeeRate, v.priceBandRate, v.maxFundingRate, v.maxPositionNotional, v.expiryTimestamp, v.fundingInterval,
                   v.firstFundingTimestamp);
        }

        template <typename Archive> static void halt(Archive& a, TradingHalt& v)
        {
            fields(a, v.halted, v.updatedTimestamp, v.reason);
        }

        template <typename Archive> static void position(Archive& a, Position& v)
        {
            fields(a, v.positionSide, v.averageEntryPrice, v.predictLiquidatePrice, v.predictBankruptPrice, v.positionSize, v.realizedPnL,
                   v.updatedUnrealizedPnl, v.initialMargin, v.mmr, v.maintenanceMargin, v.closed, v.entryNotional, v.estimatedCloseFee,
                   v.contractMultiplier, v.leverage, v.fundingPnL, v.paidFees, v.liquidating, v.managed, v.updatedTimestamp);
        }

        template <typename Archive> static void cross(Archive& a, CrossMarginState& v)
        {
            fields(a, v.initialMargin, v.maintenanceMargin, v.liquidationFeeReserve, v.unrealizedPnL, v.orderReservation, v.liquidating);
        }

        template <typename Archive> static void market(Archive& a, ContractMarket& v)
        {
            spec(a, v.spec);
            fields(a, v.status, v.markPrice, v.markTimestamp, v.commandTimestamp, v.nextFundingTimestamp, v.lastFundingTimestamp,
                   v.defaultSpotSymbolId, v.referenceTimestamp, v.lastFundingRate, v.deliveryPrice, v.insuranceBalance, v.clearingBalance,
                   v.feeBalance, v.fundingRoundingBalance, v.liquidityRevision, v.lastLiquidationRevision);
            if constexpr (std::is_same_v<Archive, BinaryWriter>)
            {
                ReferencePriceCodec::write(a, v.referenceConfig);
            }
            else
            {
                v.referenceConfig = ReferencePriceCodec::readReferencePriceConfig(a);
            }
            fields(a, v.referencePrices.indexPrice, v.referencePrices.markPrice, v.referencePrices.premiumRate,
                   v.referencePrices.fundingRate, v.referencePrices.deliveryPrice, v.referencePrices.sourceCount);
            if constexpr (std::is_same_v<Archive, BinaryWriter>)
            {
                a.template number<std::uint64_t>(v.externalPrices.size());
                for (auto& price : v.externalPrices)
                {
                    fields(a, price.sourceId, price.price, price.timestamp);
                }
            }
            else
            {
                v.externalPrices.resize(a.count(24));
                for (auto& price : v.externalPrices)
                {
                    fields(a, price.sourceId, price.price, price.timestamp);
                }
            }
        }

        template <typename Archive> static void order(Archive& a, OrderNode& n)
        {
            auto& v = n.basicOrder;
            fields(a, v.price, v.requestedQuantity, v.fillingStatus.status, v.fillingStatus.filled, v.fillingStatus.unfilled,
                   v.updatedTimestamp, v.firstFilledTimestamp, v.lastFilledTimestamp, v.completedTimestamp, n.reservationAssetId,
                   n.leverage, n.reduceOnly, n.liquidationOrder, n.executionPriceLimit, n.reservedBalance, n.riskPendingNotional,
                   n.riskAcquired, n.selfTradePrevention, n.makerFeeRate, n.takerFeeRate, n.paidFees);
        }

        template <typename Archive, typename Map> static void balanceMap(Archive& a, Map& values)
        {
            if constexpr (std::is_same_v<Archive, BinaryWriter>)
            {
                a.template number<std::uint64_t>(values.size());
                for (auto& [key, value] : values)
                {
                    auto id = key;
                    fields(a, id, value);
                }
            }
            else
            {
                const auto count = a.count(1);
                for (std::size_t i = 0; i < count; ++i)
                {
                    typename Map::key_type id{};
                    typename Map::mapped_type value{};
                    fields(a, id, value);
                    persistenceCheck(values.emplace(id, value).second, "duplicate snapshot balance");
                }
            }
        }

        static void writeAccount(BinaryWriter& a, Account& account)
        {
            a.boolean(account.active);
            a.number(account.balanceGeneration);
            a.number<std::uint64_t>(account.getAssetBalanceCount());
            account.forEachBalance(
                [&](AssetId id, const AssetBalance& value)
                {
                    a.number(id);
                    a.number(value.active);
                    a.number(value.reserved);
                });
        }

        static void readAccount(BinaryReader& a, Account& account)
        {
            account.active = a.boolean();
            const auto generation = a.number<std::uint64_t>();
            const auto count = a.count(20);
            for (std::size_t i = 0; i < count; ++i)
            {
                const auto id = a.number<AssetId>();
                const AssetBalance balance{a.number<Balance>(), a.number<Balance>()};
                persistenceCheck(!account.findBalance(id), "duplicate snapshot account asset");
                account.addBalance(id, balance);
            }
            account.balanceGeneration = generation;
        }

    public:
        static void write(BinaryWriter& a, TradingState& state)
        {
            auto& m = state.matching;
            auto& c = *m.contracts;
            fields(a, state.assets.nextAssetId, state.users.nextUserId, state.users.accountManager.nextAccountId,
                   state.symbols.nextSymbolId, m.nextOrderId, c.nextPositionId, c.automaticLiquidationBudget, m.historyCursor,
                   m.historyPositionTurn, c.historyCursor);
            a.number<std::uint64_t>(state.assets.assets.size());
            for (auto& [id, v] : state.assets.assets)
            {
                fields(a, v.assetId, v.displayName, v.assetName, v.scale);
            }
            a.number<std::uint64_t>(state.users.users.size());
            for (auto& [id, user] : state.users.users)
            {
                a.number(id);
                a.boolean(user.active);
                a.number(user.spotAccount->getAccountId());
                a.number(user.contractAccount->getAccountId());
                writeAccount(a, *user.spotAccount);
                writeAccount(a, *user.contractAccount);
                auto& account = *user.contractAccount;
                a.number<std::uint64_t>(account.crossMargins.size());
                for (auto& [asset, value] : account.crossMargins)
                {
                    a.number(asset);
                    cross(a, value);
                    std::vector<PositionId> chain;
                    for (auto* p = value.firstParticipant; p; p = p->crossNext)
                    {
                        chain.push_back(p->position->positionId);
                    }
                    a.number<std::uint64_t>(chain.size());
                    for (auto id : chain)
                    {
                        a.number(id);
                    }
                }
                a.number<std::uint64_t>(account.getPositionCount());
                account.forEachPosition(
                    [&](PositionId positionId, const Position& stored)
                    {
                        auto v = stored;
                        fields(a, v.positionId, v.userId, v.symbolId, v.marginMode, v.createdTimestamp);
                        position(a, v);
                    });
            }
            a.number<std::uint64_t>(state.symbols.symbols.size());
            for (auto& [id, v] : state.symbols.symbols)
            {
                fields(a, v.symbolId, v.name, v.symbolType, v.baseAssetId, v.quoteAssetId, v.active);
                TradingRulesCodec::write(a, v.tradingRules);
                halt(a, v.tradingHalt);
            }
            fields(a, c.settlement.insurancePrefilter, c.settlement.insuranceRejections, c.settlement.liquidationRetryCache,
                   c.settlement.retryCacheHits);
            halt(a, m.tradingControl->exchangeHalt);
            balanceMap(a, m.settleEngine.fees);
            a.number<std::uint64_t>(m.auditEngine->externalFlows.size());
            for (auto& [id, flow] : m.auditEngine->externalFlows)
            {
                a.number(id);
                a.boolean(flow < 0);
                WideBalance magnitude = flow < 0 ? -flow : flow;
                a.number<std::uint64_t>(static_cast<std::uint64_t>(magnitude));
                a.number<std::uint64_t>(static_cast<std::uint64_t>(magnitude >> 64));
            }
            a.number<std::uint64_t>(m.orderBooks.size());
            for (std::size_t i = 0; i < m.orderBooks.size(); ++i)
            {
                a.boolean(m.orderBooks[i] != nullptr);
                if (!m.orderBooks[i])
                {
                    continue;
                }
                a.enumeration(m.orderBooks[i]->snapshotImplementation);
                a.number(m.bookUpdateTimestamps[i]);
                auto* v = c.findMarket(static_cast<SymbolId>(i + 1));
                a.boolean(v != nullptr);
                if (v)
                {
                    market(a, *v);
                }
            }
            // User risk totals are retained exactly; order nodes rebind stable pointers on restore.
            a.number<std::uint64_t>(m.tradingRules->userStates.size());
            for (auto& users : m.tradingRules->userStates)
            {
                a.number<std::uint64_t>(users ? users->size() : 0);
                if (users)
                {
                    for (auto& [id, v] : *users)
                    {
                        a.number(id);
                        fields(a, v.activeOrders, v.pendingNotional, v.positionNotional);
                    }
                }
            }
            a.number<std::uint64_t>(c.positionIndex.size());
            for (auto* participant : c.positionIndex)
            {
                a.number(participant->position->positionId);
                a.number(participant->position->userId);
                a.number(participant->position->symbolId);
                a.number<std::uint64_t>(participant->orderReferences);
                a.boolean(participant->riskState != nullptr);
                a.boolean(participant->crossLinked);
                a.number<std::uint64_t>(participant->openOrderCount);
                for (auto* node = participant->firstOrder; node; node = node->contractNext)
                {
                    a.number(node->basicOrder.orderId);
                }
                a.boolean(participant->liquidationRetry != nullptr);
                if (participant->liquidationRetry)
                {
                    for (auto v : participant->liquidationRetry->dependencies)
                    {
                        a.number(v);
                    }
                }
            }
            for (auto& book : m.orderBooks)
            {
                auto* v = book ? c.findMarket(book->symbolId()) : nullptr;
                a.number<std::uint64_t>(v ? v->pendingLiquidations.count : 0);
                if (v)
                {
                    for (auto* link = v->pendingLiquidations.head; link; link = link->next)
                    {
                        a.number(link->participant->position->positionId);
                    }
                }
            }
            a.number<std::uint64_t>(m.orders.size());
            for (auto* n : m.orders)
            {
                a.boolean(n != nullptr);
                if (!n)
                {
                    continue;
                }
                auto& v = n->basicOrder;
                fields(a, v.orderId, v.userId, v.symbolId, v.orderSide, v.orderType, v.timeInForce, v.createdTimestamp);
                a.number(n->account->getAccountId());
                a.number(n->contractParticipant ? n->contractParticipant->position->positionId : PositionId{0});
                a.boolean(n->riskState != nullptr);
                order(a, *n);
            }
            for (auto& book : m.orderBooks)
            {
                std::vector<OrderId> ids;
                if (book)
                {
                    for (auto side : {OrderSide::Bid, OrderSide::Ask})
                    {
                        book->forEachOrder(side, [&](const BasicOrder& v) { ids.push_back(v.orderId); });
                    }
                }
                a.number<std::uint64_t>(ids.size());
                for (auto id : ids)
                {
                    a.number(id);
                }
            }
            auto& conditional = *m.conditionalOrders;
            a.number<std::uint64_t>(conditional.markets.size());
            for (std::size_t i = 0; i < conditional.markets.size(); ++i)
            {
                a.boolean(conditional.markets[i] != nullptr);
                if (conditional.markets[i])
                {
                    a.number(conditional.markets[i]->commandTimestamp);
                }
                for (auto& point : conditional.prices[i])
                {
                    fields(a, point.price, point.timestamp);
                }
            }
            a.number<std::uint64_t>(conditional.orders.size());
            for (auto* n : conditional.orders)
            {
                auto& v = n->view;
                ConditionalOrderCodec::write(a, v.request);
                fields(a, v.conditionalOrderId, v.status, v.orderId, v.positionId, v.updatedTimestamp, v.triggeredPrice);
                a.boolean(v.rejectReason.has_value());
                if (v.rejectReason)
                {
                    a.enumeration(*v.rejectReason);
                }
            }
            // Preserve trigger FIFO after modifications (ID order is not necessarily bucket order).
            for (auto& v : conditional.markets)
            {
                std::vector<ConditionalOrderId> ids;
                if (v)
                {
                    for (auto* indexes : {&v->rising, &v->falling})
                    {
                        for (auto& index : *indexes)
                        {
                            for (auto* entry = index.first(); entry; entry = entry->next)
                            {
                                for (auto* n = entry->value.head; n; n = n->next)
                                {
                                    ids.push_back(n->view.conditionalOrderId);
                                }
                            }
                        }
                    }
                }
                a.number<std::uint64_t>(ids.size());
                for (auto id : ids)
                {
                    a.number(id);
                }
            }
        }

        static void read(BinaryReader& a, TradingState& state)
        {
            auto& m = state.matching;
            auto& c = *m.contracts;
            std::vector<std::pair<CrossMarginState*, std::vector<PositionId>>> crossLinks;
            AssetId nextAsset{};
            UserId nextUser{};
            AccountId nextAccount{};
            SymbolId nextSymbol{};
            fields(a, nextAsset, nextUser, nextAccount, nextSymbol, m.nextOrderId, c.nextPositionId, c.automaticLiquidationBudget,
                   m.historyCursor, m.historyPositionTurn, c.historyCursor);
            auto count = a.count(1);
            for (std::size_t i = 0; i < count; ++i)
            {
                AssetId id{};
                std::string display, name;
                Scale scale{};
                fields(a, id, display, name, scale);
                state.assets.nextAssetId = id;
                auto* asset = state.assets.createAsset(display, name, scale);
                persistenceCheck(asset && asset->assetId == id, "invalid snapshot asset");
            }
            state.assets.nextAssetId = nextAsset;
            count = a.count(1);
            for (std::size_t i = 0; i < count; ++i)
            {
                const auto id = a.number<UserId>();
                const bool active = a.boolean();
                const auto spotId = a.number<AccountId>(), contractId = a.number<AccountId>();
                persistenceCheck(contractId == spotId + 1, "invalid snapshot account IDs");
                state.users.nextUserId = id;
                state.users.accountManager.nextAccountId = spotId;
                persistenceCheck(state.users.createUser() == id, "invalid snapshot user");
                auto& user = state.users.users.at(id);
                user.active = active;
                if (!active)
                {
                    --state.users.activeUserCount;
                }
                readAccount(a, *user.spotAccount);
                readAccount(a, *user.contractAccount);
                if (!user.spotAccount->active)
                {
                    --state.users.accountManager.activeAccountCount;
                }
                if (!user.contractAccount->active)
                {
                    --state.users.accountManager.activeAccountCount;
                }
                auto& account = *user.contractAccount;
                const auto margins = a.count(1);
                for (std::size_t j = 0; j < margins; ++j)
                {
                    auto asset = a.number<AssetId>();
                    auto& margin = account.crossMargin(asset);
                    cross(a, margin);
                    std::vector<PositionId> ids;
                    const auto linked = a.count(8);
                    for (std::size_t k = 0; k < linked; ++k)
                    {
                        ids.push_back(a.number<PositionId>());
                    }
                    crossLinks.emplace_back(&margin, std::move(ids));
                }
                const auto positions = a.count(1);
                for (std::size_t j = 0; j < positions; ++j)
                {
                    PositionId pid{};
                    UserId uid{};
                    SymbolId sid{};
                    MarginMode mode{};
                    Timestamp timestamp{};
                    fields(a, pid, uid, sid, mode, timestamp);
                    Position value(pid, uid, sid, PositionSide::Long, mode, 1, 1, timestamp);
                    position(a, value);
                    persistenceCheck(account.recordPosition(value), "invalid snapshot position");
                }
            }
            state.users.nextUserId = nextUser;
            state.users.accountManager.nextAccountId = nextAccount;
            state.users.userIndex.resize(static_cast<std::size_t>(nextUser - 1), nullptr);
            for (auto& [id, user] : state.users.users)
            {
                state.users.userIndex.at(id - 1) = &user;
            }
            count = a.count(1);
            std::vector<std::pair<Symbol*, bool>> symbols;
            for (std::size_t i = 0; i < count; ++i)
            {
                SymbolId id{};
                std::string name;
                SymbolType type{};
                AssetId base{}, quote{};
                bool active{};
                fields(a, id, name, type, base, quote, active);
                state.symbols.nextSymbolId = id;
                auto* value = state.symbols.createSymbol(name, type, base, quote);
                persistenceCheck(value != nullptr, "invalid snapshot symbol");
                value->tradingRules = TradingRulesCodec::read(a);
                halt(a, value->tradingHalt);
                symbols.emplace_back(value, active);
            }
            state.symbols.nextSymbolId = nextSymbol;
            state.symbols.symbolIndex.resize(static_cast<std::size_t>(nextSymbol - 1), nullptr);
            for (auto& [id, symbol] : state.symbols.symbols)
            {
                state.symbols.symbolIndex.at(id - 1) = &symbol;
            }
            fields(a, c.settlement.insurancePrefilter, c.settlement.insuranceRejections, c.settlement.liquidationRetryCache,
                   c.settlement.retryCacheHits);
            halt(a, m.tradingControl->exchangeHalt);
            balanceMap(a, m.settleEngine.fees);
            count = a.count(21);
            for (std::size_t i = 0; i < count; ++i)
            {
                auto id = a.number<AssetId>();
                bool negative = a.boolean();
                const auto low = a.number<std::uint64_t>(), high = a.number<std::uint64_t>();
                WideBalance value = (static_cast<WideBalance>(high) << 64) + low;
                m.auditEngine->externalFlows.emplace(id, negative ? -value : value);
            }
            count = a.count(1);
            m.orderBooks.resize(count);
            m.bookServices.resize(count);
            m.bookUpdateTimestamps.resize(count);
            for (std::size_t i = 0; i < count; ++i)
            {
                if (!a.boolean())
                {
                    continue;
                }
                const auto implementation = a.enumeration(OrderBookImplementation::IndexedTree);
                const auto timestamp = a.number<Timestamp>();
                const auto id = static_cast<SymbolId>(i + 1);
                auto* symbol = state.symbols.getSymbol(id);
                persistenceCheck(symbol != nullptr, "missing snapshot book symbol");
                auto rules = symbol->tradingRules;
                if (a.boolean())
                {
                    ContractMarket image;
                    market(a, image);
                    persistenceCheck(
                        static_cast<bool>(m.addContractBook(id, image.spec, std::max<Price>(1, image.markPrice), 0, implementation)),
                        "invalid snapshot contract book");
                    auto& target = *c.findMarket(id);
                    BinaryWriter copy;
                    market(copy, image);
                    BinaryReader reader(copy.view());
                    market(reader, target);
                }
                else
                {
                    persistenceCheck(m.addSpotBook(id, implementation), "invalid snapshot spot book");
                }
                symbol->tradingRules = rules;
                m.bookUpdateTimestamps[i] = timestamp;
            }
            for (auto [symbol, active] : symbols)
            {
                if (!active)
                {
                    static_cast<void>(state.symbols.removeSymbol(symbol->symbolId));
                }
            }
            count = a.count(1);
            m.tradingRules->userStates.resize(count);
            for (auto& users : m.tradingRules->userStates)
            {
                auto size = a.count(32);
                if (size)
                {
                    users = std::make_unique<TradingRuleEngine::UserStates>();
                }
                for (std::size_t j = 0; j < size; ++j)
                {
                    auto id = a.number<UserId>();
                    auto& v = (*users)[id];
                    fields(a, v.activeOrders, v.pendingNotional, v.positionNotional);
                }
            }
            std::vector<std::pair<ContractParticipant*, std::vector<OrderId>>> links;
            count = a.count(1);
            for (std::size_t i = 0; i < count; ++i)
            {
                const auto id = a.number<PositionId>(), user = a.number<UserId>();
                auto symbol = a.number<SymbolId>();
                auto* v = c.findMarket(symbol);
                persistenceCheck(v, "missing snapshot market");
                auto& p = v->createParticipant(id);
                p.market = v;
                p.account = state.users.users.at(user).contractAccount;
                p.position = p.account->findPosition(id);
                persistenceCheck(p.position, "missing snapshot position");
                p.crossMargin = &p.account->crossMargin(v->symbol->quoteAssetId);
                p.indexSlot = c.positionIndex.size();
                c.positionIndex.push_back(&p);
                c.participantIndex.insert(id, p);
                p.orderReferences = a.number<std::uint64_t>();
                if (a.boolean())
                {
                    p.riskState = &(*m.tradingRules->userStates.at(symbol - 1))[user];
                }
                if (a.boolean())
                {
                    ContractSettleEngine::linkTradingParticipant(p);
                }
                const auto orders = a.count(8);
                std::vector<OrderId> ids;
                for (std::size_t j = 0; j < orders; ++j)
                {
                    ids.push_back(a.number<OrderId>());
                }
                links.emplace_back(&p, std::move(ids));
                if (a.boolean())
                {
                    p.liquidationRetry = c.settlement.retryPool.emplace();
                    for (auto& dependency : p.liquidationRetry->dependencies)
                    {
                        dependency = a.number<std::uint64_t>();
                    }
                }
                if (p.position->positionSize)
                {
                    p.account->activePositions[id] = p.position;
                    p.account->tradingPositions[id] = p.position;
                    ++p.account->managedPositionCount;
                    v->activePositions[id] = &p;
                }
            }
            for (auto& book : m.orderBooks)
            {
                auto size = a.count(8);
                auto* v = book ? c.findMarket(book->symbolId()) : nullptr;
                for (std::size_t j = 0; j < size; ++j)
                {
                    auto* p = c.findParticipant(a.number<PositionId>());
                    persistenceCheck(v && p && p->market == v, "invalid snapshot liquidation queue");
                    v->pendingLiquidations.enqueue(*p, p->liquidationLink);
                }
            }
            count = a.count(1);
            m.orders.resize(count);
            for (std::size_t i = 0; i < count; ++i)
            {
                if (!a.boolean())
                {
                    continue;
                }
                OrderId id{};
                UserId user{};
                SymbolId symbol{};
                OrderSide side{};
                OrderType type{};
                TimeInForce tif{};
                Timestamp created{};
                fields(a, id, user, symbol, side, type, tif, created);
                auto account = a.number<AccountId>();
                auto participant = a.number<PositionId>();
                bool risk = a.boolean();
                auto& s = state.symbols.symbols.at(symbol);
                persistenceCheck(account > 0 && account <= state.users.accountManager.accounts.size(), "invalid snapshot order account");
                auto* n = m.orderPool.emplace(id, user, symbol, side, type, tif, 1, 1, created, state.users.users.at(user),
                                              *state.users.accountManager.accounts[account - 1], s, s.baseAssetId, s.quoteAssetId);
                m.orders[i] = n;
                persistenceCheck(id == i + 1, "invalid snapshot order slot");
                order(a, *n);
                if (participant)
                {
                    n->contractParticipant = c.findParticipant(participant);
                    persistenceCheck(n->contractParticipant, "missing order participant");
                    n->contractMarket = n->contractParticipant->market;
                }
                if (risk)
                {
                    n->riskState = &(*m.tradingRules->userStates.at(symbol - 1))[user];
                }
            }
            for (auto& [margin, ids] : crossLinks)
            {
                margin->firstParticipant = nullptr;
                for (auto id : ids)
                {
                    auto* p = c.findParticipant(id);
                    persistenceCheck(p && p->crossMargin == margin, "invalid cross margin link");
                    p->crossLinked = false;
                    p->crossPrevious = nullptr;
                    p->crossNext = nullptr;
                }
                for (auto it = ids.rbegin(); it != ids.rend(); ++it)
                {
                    ContractSettleEngine::linkTradingParticipant(*c.findParticipant(*it));
                }
            }
            for (auto& [participant, ids] : links)
            {
                for (auto it = ids.rbegin(); it != ids.rend(); ++it)
                {
                    auto* n = m.findNode(*it);
                    persistenceCheck(n && n->contractParticipant == participant, "invalid snapshot contract link");
                    ContractSettleEngine::attach(*n);
                }
            }
            for (auto& book : m.orderBooks)
            {
                const auto size = a.count(8);
                for (std::size_t j = 0; j < size; ++j)
                {
                    auto* n = m.findNode(a.number<OrderId>());
                    persistenceCheck(book && n && n->basicOrder.symbolId == book->symbolId(), "invalid snapshot resting order");
                    book->restoreResting(*n);
                }
            }
            auto& conditional = *m.conditionalOrders;
            count = a.count(1);
            conditional.markets.resize(count);
            conditional.prices.resize(count);
            for (std::size_t i = 0; i < count; ++i)
            {
                if (a.boolean())
                {
                    conditional.markets[i] = std::make_unique<ConditionalOrderEngine::Market>();
                    conditional.markets[i]->commandTimestamp = a.number<Timestamp>();
                }
                for (auto& point : conditional.prices[i])
                {
                    fields(a, point.price, point.timestamp);
                }
            }
            count = a.count(1);
            for (std::size_t i = 0; i < count; ++i)
            {
                auto* n = conditional.pool.emplace();
                conditional.orders.push_back(n);
                auto& v = n->view;
                v.request = ConditionalOrderCodec::read(a);
                fields(a, v.conditionalOrderId, v.status, v.orderId, v.positionId, v.updatedTimestamp, v.triggeredPrice);
                persistenceCheck(v.conditionalOrderId == i + 1, "invalid snapshot conditional slot");
                if (a.boolean())
                {
                    v.rejectReason = static_cast<PlaceOrderState>(a.number<std::uint8_t>());
                }
                conditional.userHistory[v.request.userId].push_back(v.conditionalOrderId);
                if (v.status == ConditionalOrderStatus::Waiting)
                {
                    conditional.userWaiting[v.request.userId].insert(v.conditionalOrderId);
                }
            }
            for (auto& v : conditional.markets)
            {
                auto size = a.count(8);
                for (std::size_t j = 0; j < size; ++j)
                {
                    auto* n = conditional.find(a.number<ConditionalOrderId>());
                    persistenceCheck(v && n && n->view.status == ConditionalOrderStatus::Waiting, "invalid snapshot trigger index");
                    auto& indexes = conditional.rising(n->view.request) ? v->rising : v->falling;
                    auto& index = indexes[static_cast<std::size_t>(n->view.request.source)];
                    conditional.attach(*n, index, index.getOrCreate(n->view.request.triggerPrice));
                }
            }
        }
    };
} // namespace advmatch
#endif
