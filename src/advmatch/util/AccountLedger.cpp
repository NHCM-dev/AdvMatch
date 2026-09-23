#include "AccountLedger.h"

#include "../account/User.h"
#include "Utils.h"

#include <limits>
#include <stdexcept>

namespace advmatch::util
{
    bool AccountLedger::reserve(OrderNode& node, Balance requiredBalance)
    {
        if (requiredBalance < 0 || node.reservedBalance < 0)
        {
            return false;
        }

        if (requiredBalance <= node.reservedBalance)
        {
            return true;
        }

        const BalanceChange additionalChange = requiredBalance - node.reservedBalance;
        const PositiveBalanceChange additionalAmount = static_cast<PositiveBalanceChange>(additionalChange);
        if (!node.user->reserveBalance(node.reservationAssetId, additionalAmount))
        {
            return false;
        }

        node.reservedBalance = requiredBalance;
        return true;
    }

    void AccountLedger::releaseTo(OrderNode& node, Balance requiredBalance)
    {
        if (requiredBalance < 0 || node.reservedBalance < 0 || requiredBalance > node.reservedBalance)
        {
            throw std::logic_error("invalid reservation release target");
        }

        const BalanceChange releaseChange = node.reservedBalance - requiredBalance;
        const PositiveBalanceChange releaseAmount = static_cast<PositiveBalanceChange>(releaseChange);
        if (releaseAmount == 0)
        {
            return;
        }

        if (!node.user->releaseReservedBalance(node.reservationAssetId, releaseAmount))
        {
            throw std::logic_error("account reservation cannot be released");
        }

        node.reservedBalance = requiredBalance;
    }

    void AccountLedger::releaseAll(OrderNode& node)
    {
        AccountLedger::releaseTo(node, 0);
    }

    void AccountLedger::consume(OrderNode& node, PositiveBalanceChange amount)
    {
        if (node.reservedBalance < 0 || amount > static_cast<PositiveBalanceChange>(node.reservedBalance))
        {
            throw std::logic_error("account reservation is insufficient");
        }

        if (amount == 0)
        {
            return;
        }

        if (!node.user->consumeReservedBalance(node.reservationAssetId, amount))
        {
            throw std::logic_error("account reservation cannot be consumed");
        }

        const Balance consumedBalance = static_cast<Balance>(amount);
        node.reservedBalance -= consumedBalance;
    }

    void AccountLedger::credit(User& user, AssetId assetId, PositiveBalanceChange amount)
    {
        if (amount > static_cast<PositiveBalanceChange>(std::numeric_limits<BalanceChange>::max()))
        {
            throw std::overflow_error("account credit would overflow");
        }

        const BalanceChange balanceChange = static_cast<BalanceChange>(amount);
        const Balance activeBalance = user.queryActiveBalance(assetId).value_or(0);
        if (willAddOverflow(activeBalance, balanceChange) || !user.updateBalance(assetId, balanceChange))
        {
            throw std::overflow_error("account credit would overflow");
        }
    }
} // advmatch::util
