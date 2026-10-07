#include "AccountLedger.h"

#include "advmatch/ledging/account/Account.h"
#include "advmatch/ledging/User.h"
#include "../../util/Utils.h"

#include <limits>
#include <stdexcept>

namespace advmatch
{
    void AccountLedger::reservationChanged(OrderNode&, Balance) const noexcept
    {
    }

    bool AccountLedger::reserve(OrderNode& node, Balance requiredBalance) const
    {
        if (!node.account || node.account->getAccountType() != this->getAccountType() ||
            requiredBalance < 0 || node.reservedBalance < 0)
        {
            return false;
        }

        if (requiredBalance <= node.reservedBalance)
        {
            return true;
        }

        const BalanceChange additionalChange = requiredBalance - node.reservedBalance;
        const PositiveBalanceChange additionalAmount = static_cast<PositiveBalanceChange>(additionalChange);
        if (!node.account->reserveBalance(node.reservationAssetId, additionalAmount))
        {
            return false;
        }

        const Balance previousBalance = node.reservedBalance;
        node.reservedBalance = requiredBalance;
        this->reservationChanged(node, previousBalance);
        return true;
    }

    void AccountLedger::releaseTo(OrderNode& node, Balance requiredBalance) const
    {
        if (!node.account || node.account->getAccountType() != this->getAccountType() || requiredBalance < 0 ||
            node.reservedBalance < 0 || requiredBalance > node.reservedBalance)
        {
            throw std::logic_error("invalid reservation release target");
        }

        const BalanceChange releaseChange = node.reservedBalance - requiredBalance;
        const PositiveBalanceChange releaseAmount = static_cast<PositiveBalanceChange>(releaseChange);
        if (releaseAmount == 0)
        {
            return;
        }

        if (!node.account->releaseReservedBalance(node.reservationAssetId, releaseAmount))
        {
            throw std::logic_error("ledging reservation cannot be released");
        }

        const Balance previousBalance = node.reservedBalance;
        node.reservedBalance = requiredBalance;
        this->reservationChanged(node, previousBalance);
    }

    void AccountLedger::releaseAll(OrderNode& node) const
    {
        this->releaseTo(node, 0);
    }

    void AccountLedger::consume(OrderNode& node, PositiveBalanceChange amount) const
    {
        if (!node.account || node.account->getAccountType() != this->getAccountType() || node.reservedBalance < 0 ||
            amount > static_cast<PositiveBalanceChange>(node.reservedBalance))
        {
            throw std::logic_error("ledging reservation is insufficient");
        }

        if (amount == 0)
        {
            return;
        }

        if (!node.account->consumeReservedBalance(node.reservationAssetId, amount))
        {
            throw std::logic_error("ledging reservation cannot be consumed");
        }

        const Balance previousBalance = node.reservedBalance;
        node.reservedBalance -= static_cast<Balance>(amount);
        this->reservationChanged(node, previousBalance);
    }

    void AccountLedger::credit(User& user, AssetId assetId, PositiveBalanceChange amount) const
    {
        if (amount > static_cast<PositiveBalanceChange>(std::numeric_limits<BalanceChange>::max()))
        {
            throw std::overflow_error("ledging credit would overflow");
        }

        Account& account = this->accountFor(user);
        const BalanceChange balanceChange = static_cast<BalanceChange>(amount);
        const Balance activeBalance = account.queryActiveBalance(assetId).value_or(0);
        if (util::willAddOverflow(activeBalance, balanceChange) || !account.updateBalance(assetId, balanceChange))
        {
            throw std::overflow_error("ledging credit would overflow");
        }
    }

    const Account& AccountLedger::getAccount(const User& user) const noexcept
    {
        return this->accountFor(user);
    }
}
