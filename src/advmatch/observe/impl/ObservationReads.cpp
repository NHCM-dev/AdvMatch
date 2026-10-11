#include "TradingObserver.h"
#include "advmatch/matching/contracts/ContractEngine.h"

// Keep dispatch and pagination with TradingObserver's value-copy operations.
// Templates are internal to this implementation, not a second public API.
#include "ObservationReadHelpers.h"

namespace advmatch::observe
{
    void TradingObserver::readData(ObservationResult& result) const
    {
        const auto& query = result.query;
        auto& page = result.page;
        const auto& contracts = this->matching.getContractEngine();
        const auto& accountSlots = this->users.accountManager.accounts;

        auto scanAccountRows = [&](auto& output, auto countRows, auto copyRow)
        {
            std::array<const Account*, 2> selected{};
            std::span<Account* const> all(accountSlots);
            std::size_t count = all.size();
            bool scoped = false;
            if (query.accountId != 0)
            {
                selected[0] = this->retainedAccount(query.accountId);
                count = 1;
                scoped = true;
            }
            else if (query.userId != 0)
            {
                const auto found = this->users.users.find(query.userId);
                if (found == this->users.users.end())
                {
                    page.state = ReadState::NotFound;
                    return;
                }

                selected = {&found->second.getSpotAccount(), &found->second.getContractAccount()};
                count = 2;
                scoped = true;
            }

            std::size_t accountIndex = static_cast<std::size_t>(query.cursor.anchor);
            std::size_t rowIndex = static_cast<std::size_t>(query.cursor.secondary);
            if (query.cursor.anchor > count)
            {
                page.state = ReadState::CursorExpired;
                return;
            }

            while (accountIndex < count && page.count < query.limit && page.examined < query.scanBudget)
            {
                const auto* account = scoped ? selected[accountIndex] : all[accountIndex];
                const bool eligible = account != nullptr && (query.includeInactive || account->isActive()) &&
                    (query.userId == 0 || account->getUserId() == query.userId);
                const std::size_t rowCount = eligible ? countRows(*account) : 0;
                if (rowIndex > rowCount)
                {
                    page.state = ReadState::CursorExpired;
                    return;
                }

                if (rowCount == 0)
                {
                    ++page.examined;
                }

                while (rowIndex < rowCount && page.count < query.limit && page.examined < query.scanBudget)
                {
                    ++page.examined;
                    auto value = copyRow(*account, rowIndex++);
                    if (value)
                    {
                        output.push_back(std::move(*value));
                        ++page.count;
                    }
                }

                if (rowIndex == rowCount)
                {
                    ++accountIndex;
                    rowIndex = 0;
                }
            }

            page.next.anchor = accountIndex;
            page.next.secondary = rowIndex;
            page.hasMore = accountIndex < count;
        };

        switch (query.kind)
        {
            case ReadKind::OrdersNewest:
            case ReadKind::PositionsNewest:
            {
                if (!this->executionIndexHealthy)
                {
                    page.state = ReadState::IndexUnavailable;
                    return;
                }
                UserId userId = query.userId;
                AccountId accountId = query.accountId;
                if (accountId != 0)
                {
                    const auto* account = this->retainedAccount(accountId);
                    if (!account || (userId != 0 && userId != account->getUserId()))
                    {
                        page.state = ReadState::NotFound;
                        return;
                    }
                    userId = account->getUserId();
                }
                if (query.kind == ReadKind::OrdersNewest)
                {
                    auto& output = detail::rows<OrderView>(result);
                    const auto visit = [&](OrderId id)
                    {
                        const auto value = this->order(id);
                        if (!value || !detail::matchesOrder(*value, query)) return false;
                        output.push_back(*value);
                        return true;
                    };
                    if (userId == 0) detail::scanNewest(this->matching.orders.size(), query, page, visit);
                    else if (const auto found = this->historyOrders.find(userId); found != this->historyOrders.end())
                        detail::scanHistoryIndex(found->second, query, page, visit);
                    return;
                }
                auto& output = detail::rows<PositionView>(result);
                if (accountId == 0 && userId != 0)
                {
                    const auto* owner = this->users.getUser(userId);
                    if (!owner)
                    {
                        page.state = ReadState::NotFound;
                        return;
                    }
                    accountId = owner->getContractAccount().getAccountId();
                }
                const auto visit = [&](PositionId id)
                {
                    const auto value = this->position(id, accountId);
                    if (!value || !detail::matchesPosition(*value, query) ||
                        (query.positionFilter == PositionFilter::ClosedExecuted && !this->executedPositions.contains(id))) return false;
                    output.push_back(*value);
                    return true;
                };
                if (userId == 0) detail::scanNewest(contracts.nextPositionId - 1, query, page, visit);
                else if (const auto found = this->historyPositions.find(userId); found != this->historyPositions.end())
                    detail::scanHistoryIndex(found->second, query, page, visit);
                return;
            }
            case ReadKind::ConditionalOrdersNewest:
            {
                auto& output = detail::rows<ConditionalOrderView>(result);
                const auto& engine = this->matching.getConditionalOrderEngine();
                const auto visit = [&](ConditionalOrderId id)
                {
                    const auto value = engine.query(id);
                    if (!value || (query.userId != 0 && value->request.userId != query.userId) ||
                        (query.symbolId != 0 && value->request.symbolId != query.symbolId) ||
                        (query.orderId != 0 && value->conditionalOrderId != query.orderId) ||
                        (query.price != 0 && value->request.triggerPrice != query.price) ||
                        (!query.includeTerminal && value->status != ConditionalOrderStatus::Waiting)) return false;
                    const auto* owner = this->users.getUser(value->request.userId);
                    const auto account = owner ? (value->request.contractOptions ? owner->getContractAccount().getAccountId() : owner->getSpotAccount().getAccountId()) : 0;
                    if (query.accountId != 0 && account != query.accountId) return false;
                    output.push_back(*value);
                    return true;
                };
                UserId userId = query.userId;
                if (query.accountId != 0)
                {
                    const auto* account = this->retainedAccount(query.accountId);
                    if (!account || (userId != 0 && userId != account->getUserId()))
                    {
                        page.state = ReadState::NotFound;
                        return;
                    }
                    userId = account->getUserId();
                }
                if (!this->executionIndexHealthy)
                {
                    page.state = ReadState::IndexUnavailable;
                    return;
                }
                if (userId == 0) detail::scanNewest(engine.size(), query, page, visit);
                else if (const auto found = this->historyConditionals.find(userId); found != this->historyConditionals.end())
                    detail::scanHistoryIndex(found->second, query, page, visit);
                return;
            }
            case ReadKind::TradesNewest:
            {
                auto& output = detail::rows<TradeView>(result);
                const auto oldest = this->nextTradeId - std::min<std::uint64_t>(this->nextTradeId - 1, this->trades.size());
                if (this->trades.empty() || this->nextTradeId == 1) return;
                if (query.cursor.started && query.cursor.anchor != 0 && query.cursor.anchor < oldest)
                {
                    page.state = ReadState::CursorExpired;
                    return;
                }
                detail::scanNewest(this->nextTradeId - 1, query, page, [&](std::uint64_t id)
                {
                    if (id < oldest) return false;
                    const auto& value = this->trades[(id - 1) % this->trades.size()];
                    if ((query.symbolId != 0 && query.symbolId != value.symbolId) ||
                        (query.userId != 0 && query.userId != value.makerUserId && query.userId != value.takerUserId) ||
                        (query.accountId != 0 && query.accountId != value.makerAccountId && query.accountId != value.takerAccountId) ||
                        (query.orderId != 0 && query.orderId != value.trade.makerOrderId && query.orderId != value.trade.takerOrderId)) return false;
                    output.push_back(value);
                    return true;
                });
                if (page.next.anchor < oldest)
                {
                    page.next.anchor = 0;
                    page.hasMore = false;
                }
                return;
            }
            case ReadKind::TradingStatus:
                detail::point(result, this->matching.getTradingControlEngine().query(query.symbolId));
                return;
            case ReadKind::Audit:
                detail::point(result, std::optional(this->matching.getAuditEngine().report(this->assets, query.limit)));
                return;
            case ReadKind::UserRisk:
                detail::point(result, this->matching.getTradingRuleEngine().query(query.symbolId, query.userId));
                return;
            case ReadKind::SpotFeeBalance:
                detail::point(result, this->assets.containsAsset(query.assetId) ? std::optional(FeeBalanceView{query.assetId, this->matching.getSpotSettleEngine().feeBalance(query.assetId)}) : std::nullopt);
                return;
            case ReadKind::ConditionalOrder:
            case ReadKind::ConditionalOrders:
            {
                const auto& engine = this->matching.getConditionalOrderEngine();
                const auto matches = [&](const ConditionalOrderView& value)
                {
                    const User* user = this->users.getUser(value.request.userId);
                    const AccountId accountId = user ? (value.request.contractOptions ? user->getContractAccount().getAccountId() : user->getSpotAccount().getAccountId()) : 0;
                    return (query.userId == 0 || query.userId == value.request.userId) && (query.symbolId == 0 || query.symbolId == value.request.symbolId) && (query.accountId == 0 || query.accountId == accountId) && (query.orderId == 0 || query.orderId == value.conditionalOrderId) && (query.price == 0 || query.price == value.request.triggerPrice) && (query.includeTerminal || value.status == ConditionalOrderStatus::Waiting);
                };

                if (query.kind == ReadKind::ConditionalOrder)
                {
                    auto value = engine.query(query.orderId);
                    detail::point(result, value && matches(*value) ? value : std::nullopt);
                    return;
                }

                auto& output = detail::rows<ConditionalOrderView>(result);
                UserId scopedUser = query.userId;
                if (query.accountId != 0)
                {
                    const auto* account = this->retainedAccount(query.accountId);
                    if (account) scopedUser = account->userId;
                }
                const auto ids = engine.userOrderIds(scopedUser, query.includeTerminal);
                detail::scanSlots(scopedUser == 0 ? engine.size() : ids.size(), query, page, [&](std::size_t index)
                {
                    auto value = engine.query(scopedUser == 0 ? index + 1 : ids[index]);
                    if (value && matches(*value))
                    {
                        output.push_back(std::move(*value));
                        return true;
                    }

                    return false;
                });
                return;
            }
            case ReadKind::Overview:
                result.data = this->overview();
                page.count = 1;
                return;
            case ReadKind::Asset:
                detail::point(result, this->asset(query.assetId));
                return;
            case ReadKind::User:
            {
                auto value = this->user(query.userId);
                detail::point(result, value && (query.includeInactive || value->active) ? value : std::nullopt);
                return;
            }
            case ReadKind::Account:
            {
                auto value = this->account(query.accountId);
                detail::point(result, value && (query.includeInactive || value->active) && (query.userId == 0 || value->userId == query.userId) ? value : std::nullopt);
                return;
            }
            case ReadKind::Balance:
            {
                const auto owner = this->account(query.accountId);
                detail::point(result, owner && (query.includeInactive || owner->active) && (query.userId == 0 || owner->userId == query.userId) ? this->balance(query.accountId, query.assetId) : std::nullopt);
                return;
            }
            case ReadKind::CrossMargin:
            {
                const auto owner = this->account(query.accountId);
                detail::point(result, owner && (query.includeInactive || owner->active) && (query.userId == 0 || owner->userId == query.userId) ? this->crossMargin(query.accountId, query.assetId) : std::nullopt);
                return;
            }
            case ReadKind::Symbol:
            {
                auto value = this->symbol(query.symbolId);
                detail::point(result, value && (query.includeInactive || value->active) ? value : std::nullopt);
                return;
            }
            case ReadKind::Book:
                detail::point(result, this->book(query.symbolId));
                return;
            case ReadKind::Order:
            {
                auto value = this->order(query.orderId);
                detail::point(result, value && detail::matchesOrder(*value, query) ? value : std::nullopt);
                return;
            }
            case ReadKind::Position:
            {
                auto value = this->position(query.positionId, query.accountId);
                detail::point(result, value && detail::matchesPosition(*value, query) ? value : std::nullopt);
                return;
            }
            case ReadKind::Market:
                detail::point(result, this->market(query.symbolId));
                return;
            case ReadKind::Level:
                detail::point(result, this->level(query.symbolId, query.side, query.price));
                return;
            case ReadKind::Assets:
            {
                auto& output = detail::rows<AssetView>(result);
                detail::scanNodes(this->assets.assets, query, page, [&](AssetId id, const Asset& value)
                {
                    if (query.assetId != 0 && query.assetId != id)
                    {
                        return false;
                    }

                    output.push_back({id, value.scale, value.displayName, value.assetName});
                    return true;
                });
                return;
            }
            case ReadKind::Users:
            {
                auto& output = detail::rows<UserView>(result);
                detail::scanNodes(this->users.users, query, page, [&](UserId id, const User& value)
                {
                    if ((!query.includeInactive && !value.isActive()) || (query.userId != 0 && id != query.userId))
                    {
                        return false;
                    }

                    output.push_back(*this->user(id));
                    return true;
                });
                return;
            }
            case ReadKind::Accounts:
            {
                auto& output = detail::rows<AccountView>(result);
                scanAccountRows(output, [](const Account&)
                {
                    return std::size_t{1};
                }, [&](const Account& account, std::size_t) -> std::optional<AccountView>
                {
                    return this->copyAccount(account);
                });
                return;
            }
            case ReadKind::Symbols:
            {
                auto& output = detail::rows<SymbolView>(result);
                detail::scanNodes(this->symbols.symbols, query, page, [&](SymbolId id, const Symbol& value)
                {
                    if ((!query.includeInactive && !value.isActive()) || (query.symbolId != 0 && id != query.symbolId))
                    {
                        return false;
                    }

                    output.push_back({id, value.symbolType, value.baseAssetId, value.quoteAssetId, value.isActive(), value.name, value.tradingRules});
                    return true;
                });
                return;
            }
            case ReadKind::Books:
            {
                auto& output = detail::rows<BookView>(result);
                detail::scanSlots(this->matching.orderBooks.size(), query, page, [&](std::size_t index)
                {
                    const auto* book = this->matching.orderBooks[index].get();
                    if (book == nullptr || (query.symbolId != 0 && book->symbolId() != query.symbolId))
                    {
                        return false;
                    }

                    output.push_back(*this->book(book->symbolId()));
                    return true;
                });
                return;
            }
            case ReadKind::Depth:
            {
                auto& output = detail::rows<DepthView>(result);
                output.resize(query.limit);
                page = this->readDepth(query, output);
                output.resize(page.count);
                return;
            }
            case ReadKind::Orders:
            {
                auto& output = detail::rows<OrderView>(result);
                detail::scanSlots(this->matching.orders.size(), query, page, [&](std::size_t index)
                {
                    const auto* node = this->matching.orders[index];
                    if (node == nullptr)
                    {
                        return false;
                    }

                    auto value = this->copyOrder(*node);
                    if (!detail::matchesOrder(value, query))
                    {
                        return false;
                    }

                    output.push_back(value);
                    return true;
                });
                return;
            }
            case ReadKind::UserOpenOrders:
            {
                auto& output = detail::rows<OrderView>(result);
                if (!this->indexHealthy)
                {
                    page.state = ReadState::IndexUnavailable;
                    return;
                }

                const auto owner = this->user(query.userId);
                if (!owner || (!query.includeInactive && !owner->active))
                {
                    page.state = ReadState::NotFound;
                    return;
                }

                const auto found = this->userOrders.find(query.userId);
                if (found == this->userOrders.end())
                {
                    page.state = this->user(query.userId) ? ReadState::Success : ReadState::NotFound;
                    return;
                }

                const auto& values = found->second.values();
                detail::scanSlots(values.size(), query, page, [&](std::size_t index)
                {
                    const auto value = this->order(values[index]);
                    if (value && detail::matchesOrder(*value, query))
                    {
                        output.push_back(*value);
                        return true;
                    }

                    return false;
                });
                return;
            }
            case ReadKind::BookOrders:
            {
                auto& output = detail::rows<OrderView>(result);
                const auto* book = this->matching.findBook(query.symbolId);
                if (book == nullptr)
                {
                    page.state = ReadState::NotFound;
                    return;
                }

                const PriceLevel* level = book->observedBestLevel(query.side);
                const OrderNode* node = level != nullptr ? level->head : nullptr;
                if (query.cursor.started && query.cursor.anchor != 0)
                {
                    node = this->matching.findNode(query.cursor.anchor);
                    if (node == nullptr || node->priceLevel == nullptr || node->basicOrder.symbolId != query.symbolId || node->basicOrder.orderSide != query.side)
                    {
                        page.state = ReadState::CursorExpired;
                        return;
                    }

                    level = node->priceLevel;
                    node = node->next;
                    if (node == nullptr)
                    {
                        level = book->observedNextLevel(query.side, level->price);
                        node = level != nullptr ? level->head : nullptr;
                    }
                }

                while (node != nullptr && page.count < query.limit && page.examined < query.scanBudget)
                {
                    ++page.examined;
                    page.next.anchor = node->basicOrder.orderId;
                    const auto value = this->copyOrder(*node);
                    if (detail::matchesOrder(value, query))
                    {
                        output.push_back(value);
                        ++page.count;
                    }

                    level = node->priceLevel;
                    node = node->next;
                    if (node == nullptr)
                    {
                        level = book->observedNextLevel(query.side, level->price);
                        node = level != nullptr ? level->head : nullptr;
                    }
                }

                page.hasMore = node != nullptr;
                return;
            }
            case ReadKind::Balances:
            {
                auto& output = detail::rows<BalanceView>(result);
                scanAccountRows(output, [](const Account& account)
                {
                    return account.getAssetBalanceCount();
                }, [&](const Account& account, std::size_t index) -> std::optional<BalanceView>
                {
                    const auto assetId = account.overflowBalances ? account.overflowBalances->values()[index].first : account.inlineBalances[index].assetId;
                    if (query.assetId != 0 && query.assetId != assetId)
                    {
                        return std::nullopt;
                    }

                    const auto& balance = account.overflowBalances ? account.overflowBalances->values()[index].second : account.inlineBalances[index].balance;
                    return this->copyBalance(account, assetId, balance);
                });
                return;
            }
            case ReadKind::CrossMargins:
            {
                auto& output = detail::rows<CrossMarginView>(result);
                const auto* owner = this->retainedAccount(query.accountId);
                if (owner == nullptr || owner->getAccountType() != AccountType::Contract || (!query.includeInactive && !owner->isActive()) || (query.userId != 0 && owner->getUserId() != query.userId))
                {
                    page.state = ReadState::NotFound;
                    return;
                }

                const auto& account = static_cast<const ContractAccount&>(*owner);
                detail::scanNodes(account.crossMargins, query, page, [&](AssetId id, const CrossMarginState& value)
                {
                    if (query.assetId != 0 && id != query.assetId)
                    {
                        return false;
                    }

                    output.push_back(this->copyCrossMargin(account, id, value));
                    return true;
                });
                return;
            }
            case ReadKind::AccountPositions:
            case ReadKind::Positions:
            case ReadKind::ManagedPositions:
            {
                auto& output = detail::rows<PositionView>(result);
                if (query.kind == ReadKind::ManagedPositions && query.userId == 0 && query.accountId == 0)
                {
                    detail::scanSlots(contracts.positionIndex.size(), query, page, [&](std::size_t index)
                    {
                        const auto* participant = contracts.positionIndex[index];
                        const auto value = this->copyPosition(*participant->position, participant->account->getAccountId());
                        if (detail::matchesPosition(value, query))
                        {
                            output.push_back(value);
                            return true;
                        }

                        return false;
                    });
                    return;
                }

                scanAccountRows(output, [&](const Account& account) -> std::size_t
                {
                    if (account.getAccountType() != AccountType::Contract)
                    {
                        return 0;
                    }

                    const auto& contract = static_cast<const ContractAccount&>(account);
                    if (query.kind == ReadKind::ManagedPositions && query.positionFilter == PositionFilter::Open)
                    {
                        return contract.activePositions.size();
                    }

                    if (query.kind == ReadKind::ManagedPositions && query.positionFilter == PositionFilter::Trading)
                    {
                        return contract.tradingPositions.size();
                    }

                    return contract.getPositionCount();
                }, [&](const Account& account, std::size_t index) -> std::optional<PositionView>
                {
                    const auto& contract = static_cast<const ContractAccount&>(account);
                    const auto* position = query.kind == ReadKind::ManagedPositions && query.positionFilter == PositionFilter::Open ? contract.activePositions.values()[index].second :
                        query.kind == ReadKind::ManagedPositions && query.positionFilter == PositionFilter::Trading ? contract.tradingPositions.values()[index].second : contract.observedPositionAt(index);
                    if (position == nullptr)
                    {
                        return std::nullopt;
                    }

                    const auto value = this->copyPosition(*position, account.getAccountId());
                    const bool managed = query.kind != ReadKind::ManagedPositions || value.managed;
                    return managed && detail::matchesPosition(value, query) ? std::optional<PositionView>(value) : std::nullopt;
                });
                return;
            }
            case ReadKind::MarketPositions:
            {
                auto& output = detail::rows<PositionView>(result);
                const auto* market = contracts.findMarket(query.symbolId);
                if (market == nullptr)
                {
                    page.state = ReadState::NotFound;
                    return;
                }

                auto visit = [&](const ContractParticipant* participant)
                {
                    const auto value = this->copyPosition(*participant->position, participant->account->getAccountId());
                    if (detail::matchesPosition(value, query))
                    {
                        output.push_back(value);
                        return true;
                    }

                    return false;
                };
                if (query.positionFilter != PositionFilter::Trading)
                {
                    const auto& index = query.positionFilter == PositionFilter::Open ? market->activePositions : market->participants;
                    detail::scanSlots(index.size(), query, page, [&](std::size_t offset)
                    {
                        return visit(index.values()[offset].second);
                    });
                    return;
                }

                // Union the active-position and order-participant indexes.
                // No temporary set and no scan over completed positions.
                std::size_t phase = static_cast<std::size_t>(query.cursor.anchor);
                std::size_t offset = static_cast<std::size_t>(query.cursor.secondary);
                if (phase > 2)
                {
                    page.state = ReadState::CursorExpired;
                    return;
                }

                while (phase < 2 && page.count < query.limit && page.examined < query.scanBudget)
                {
                    const auto& index = phase == 0 ? market->activePositions : market->orderParticipants;
                    if (offset > index.size())
                    {
                        page.state = ReadState::CursorExpired;
                        return;
                    }

                    while (offset < index.size() && page.count < query.limit && page.examined < query.scanBudget)
                    {
                        const auto& [id, participant] = index.values()[offset++];
                        ++page.examined;
                        if (phase == 0 || !market->activePositions.contains(id))
                        {
                            page.count += visit(participant);
                        }
                    }

                    if (offset == index.size())
                    {
                        ++phase;
                        offset = 0;
                    }
                }

                page.next.anchor = phase;
                page.next.secondary = offset;
                page.hasMore = phase < 2;
                return;
            }
            case ReadKind::Markets:
            {
                auto& output = detail::rows<MarketView>(result);
                detail::scanNodes(contracts.markets, query, page, [&](SymbolId id, const ContractMarket&)
                {
                    if (query.symbolId != 0 && id != query.symbolId)
                    {
                        return false;
                    }

                    output.push_back(*this->market(id));
                    return true;
                });
                return;
            }
            case ReadKind::PendingLiquidations:
            {
                auto& output = detail::rows<PositionView>(result);
                const auto* market = contracts.findMarket(query.symbolId);
                if (market == nullptr)
                {
                    page.state = ReadState::NotFound;
                    return;
                }

                const auto* link = market->pendingLiquidations.head;
                if (query.cursor.started && query.cursor.anchor != 0)
                {
                    const auto* found = contracts.participantIndex.get(query.cursor.anchor);
                    if (!found || found->market != market || !found->liquidationLink.linked)
                    {
                        page.state = ReadState::CursorExpired;
                        return;
                    }

                    link = found->liquidationLink.next;
                }

                while (link != nullptr && page.count < query.limit && page.examined < query.scanBudget)
                {
                    ++page.examined;
                    if (link->participant != nullptr)
                    {
                        const auto* participant = link->participant;
                        page.next.anchor = participant->position->positionId;
                        const auto value = this->copyPosition(*participant->position, participant->account->getAccountId());
                        if (detail::matchesPosition(value, query))
                        {
                            output.push_back(value);
                            ++page.count;
                        }
                    }

                    link = link->next;
                }

                page.hasMore = link != nullptr;
                return;
            }
            case ReadKind::Trades:
            {
                auto& output = detail::rows<TradeView>(result);
                detail::scanTape(this->trades, this->nextTradeId, query, page, [&](const TradeView& value)
                {
                    if ((query.symbolId == 0 || value.symbolId == query.symbolId) &&
                        (query.userId == 0 || query.userId == value.makerUserId || query.userId == value.takerUserId) &&
                        (query.accountId == 0 || query.accountId == value.makerAccountId || query.accountId == value.takerAccountId) &&
                        (query.orderId == 0 || query.orderId == value.trade.makerOrderId || query.orderId == value.trade.takerOrderId))
                    {
                        output.push_back(value);
                        return true;
                    }

                    return false;
                });
                return;
            }
            case ReadKind::ContractEvents:
            {
                auto& output = detail::rows<ContractEventView>(result);
                detail::scanTape(this->events, this->nextEventId, query, page, [&](const ContractEventView& value)
                {
                    if ((query.symbolId == 0 || value.event.symbolId == query.symbolId) &&
                        (query.userId == 0 || value.event.userId == query.userId) &&
                        (query.accountId == 0 || value.accountId == query.accountId) &&
                        (query.positionId == 0 || value.event.positionId == query.positionId) &&
                        (query.orderId == 0 || value.event.orderId == query.orderId || value.event.counterOrderId == query.orderId))
                    {
                        output.push_back(value);
                        return true;
                    }

                    return false;
                });
                return;
            }
        }

        page.state = ReadState::InvalidQuery;
    }
}
