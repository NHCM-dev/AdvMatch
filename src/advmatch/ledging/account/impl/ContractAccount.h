#ifndef ADVMATCH_CONTRACT_ACCOUNT_H
#define ADVMATCH_CONTRACT_ACCOUNT_H

#include "../Account.h"
#include <functional>
#include "../../../trading/futures/Position.h"
#include "advmatch/util/BigAllocs.h"

namespace advmatch
{
    struct ContractParticipant;
    using PositionPool = BigAllocs<Position>;

    struct CrossMarginState
    {
        Balance initialMargin = 0;
        Balance maintenanceMargin = 0;
        Balance liquidationFeeReserve = 0;
        BalanceChange unrealizedPnL = 0;
        Balance orderReservation = 0;
        bool liquidating = false;
        // Only trading participants for this account and collateral asset.
        // The nodes belong to ContractEngine and are linked/unlinked by the
        // settlement lifecycle. Unrelated products/assets are never scanned.
        ContractParticipant* firstParticipant = nullptr;
        CrossMarginState* nextLiquidationVisited = nullptr;
        bool liquidationVisited = false;
    };

    class ContractAccount : public Account
    {
        friend class observe::TradingObserver;
        NodeHashMap<AssetId, CrossMarginState> crossMargins;
        // These maps index stable Position nodes; never borrow addresses of
        // index entries across insertion/erasure.
        DenseMap<PositionId, const Position*> activePositions;
        DenseMap<PositionId, const Position*> tradingPositions;
        std::size_t managedPositionCount = 0;
        std::size_t managedOrderCount = 0;
        friend class ContractSettleEngine;

    protected:
        virtual const Position* observedPositionAt(std::size_t index) const noexcept = 0;
        bool canWithdraw(AssetId assetId, PositiveBalanceChange amount) const noexcept override;

        ContractAccount(AccountId accountId, UserId userId)
            : Account(accountId, userId)
        {
        }

    public:
        // Stable state address. Matching engines update these cached totals;
        // queries and transfers never scan all positions in an account.
        template <typename Visitor>
        void forEachActivePosition(Visitor&& visitor) const
        {
            for (const auto& [id, position] : this->activePositions)
            {
                visitor(id, *position);
            }
        }

        // Includes open positions and positions with pending orders.
        template <typename Visitor>
        void forEachTradingPosition(Visitor&& visitor) const
        {
            for (const auto& [id, position] : this->tradingPositions)
            {
                visitor(id, *position);
            }
        }

        bool hasTradingExposure() const noexcept
        {
            return this->managedPositionCount != 0 || this->managedOrderCount != 0;
        }

        CrossMarginState& crossMargin(AssetId assetId);
        const CrossMarginState* findCrossMargin(AssetId assetId) const noexcept;

        [[nodiscard]]
        AccountType getAccountType() const noexcept final
        {
            return AccountType::Contract;
        }

        [[nodiscard]]
        virtual Position* findPosition(PositionId positionId) noexcept = 0;

        [[nodiscard]]
        virtual const Position* findPosition(PositionId positionId) const noexcept = 0;

        [[nodiscard]]
        virtual bool recordPosition(Position position) = 0;

        [[nodiscard]]
        virtual bool removePosition(PositionId positionId) = 0;

        [[nodiscard]]
        virtual std::size_t getPositionCount() const noexcept = 0;

        // Run on the owning thread; do not mutate positions in the callback.
        virtual void forEachPosition(const std::function<void(PositionId, const Position&)>& visitor) const = 0;
    };

    class StandardContractAccount final : public ContractAccount
    {
        friend class observe::TradingObserver;
        // Owning pools never move Position objects. Only pointer indexes rehash.
        std::unique_ptr<PositionPool> ownedPositionPool;
        PositionPool* positionPool;
        DenseMap<PositionId, Position*> positions;

        const Position* observedPositionAt(std::size_t index) const noexcept override
        {
            return index < this->positions.size() ? this->positions.values()[index].second : nullptr;
        }

    public:
        StandardContractAccount(AccountId accountId, UserId userId, PositionPool* positionPool = nullptr)
            : ContractAccount(accountId, userId), positionPool(positionPool)
        {
        }

        ~StandardContractAccount() override;

        [[nodiscard]]
        Position* findPosition(PositionId positionId) noexcept override;

        [[nodiscard]]
        const Position* findPosition(PositionId positionId) const noexcept override;

        [[nodiscard]]
        bool recordPosition(Position position) override;

        [[nodiscard]]
        bool removePosition(PositionId positionId) override;

        [[nodiscard]]
        std::size_t getPositionCount() const noexcept override;
        void forEachPosition(const std::function<void(PositionId, const Position&)>& visitor) const override
        {
            for (const auto& [positionId, position] : this->positions)
            {
                visitor(positionId, *position);
            }
        }

    };
}

#endif // ADVMATCH_CONTRACT_ACCOUNT_H
