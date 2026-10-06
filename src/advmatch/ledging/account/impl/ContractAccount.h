#ifndef ADVMATCH_CONTRACT_ACCOUNT_H
#define ADVMATCH_CONTRACT_ACCOUNT_H

#include "../Account.h"
#include "../../../trading/futures/Position.h"

namespace advmatch
{
    class ContractAccount : public Account
    {
    protected:
        ContractAccount(AccountId accountId, UserId userId)
            : Account(accountId, userId)
        {
        }

    public:
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
    };

    class StandardContractAccount final : public ContractAccount
    {
        NodeHashMap<PositionId, Position> positions;

    public:
        StandardContractAccount(AccountId accountId, UserId userId)
            : ContractAccount(accountId, userId)
        {
        }

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
    };
}

#endif // ADVMATCH_CONTRACT_ACCOUNT_H
