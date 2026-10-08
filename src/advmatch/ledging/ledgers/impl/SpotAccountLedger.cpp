#include "SpotAccountLedger.h"

#include "advmatch/ledging/User.h"

#include <array>
#include <stdexcept>

namespace advmatch
{
    bool SpotAccountLedger::settle(OrderNode& buyer, OrderNode& seller, PositiveBalanceChange baseAmount, PositiveBalanceChange quoteAmount) const
    {
        if (buyer.reservedBalance < 0 || seller.reservedBalance < 0 ||
            quoteAmount > static_cast<PositiveBalanceChange>(buyer.reservedBalance) ||
            baseAmount > static_cast<PositiveBalanceChange>(seller.reservedBalance))
        {
            return false;
        }

        struct Change
        {
            Account* account;
            AssetId assetId;
            BalanceChange active;
            BalanceChange reserved;
            std::optional<Account::BalanceUpdate> prepared;
        };

        const auto base = static_cast<BalanceChange>(baseAmount);
        const auto quote = static_cast<BalanceChange>(quoteAmount);
        const bool sameAccount = buyer.account == seller.account;
        std::array<Change, 4> changes{{
            {buyer.account, buyer.quoteAssetId, sameAccount ? quote : 0, -quote, std::nullopt},
            {seller.account, seller.baseAssetId, sameAccount ? base : 0, -base, std::nullopt},
            {buyer.account, buyer.baseAssetId, base, 0, std::nullopt},
            {seller.account, seller.quoteAssetId, quote, 0, std::nullopt}}};
        const std::size_t count = sameAccount ? 2 : 4;
        for (std::size_t index = 0; index < count; ++index)
        {
            auto& change = changes[index];
            change.prepared = change.account->prepareBalanceChange(change.account->captureBalance(change.assetId), change.active, change.reserved);
            if (!change.prepared)
            {
                return false;
            }
        }

        // Debits precede credits. A credit can insert/promote the destination
        // asset but cannot invalidate an uncommitted token in that account.
        for (std::size_t index = 0; index < count; ++index)
        {
            auto& change = changes[index];
            if (!change.account->commitBalanceChange(*change.prepared))
            {
                throw std::logic_error("prepared spot settlement was invalidated");
            }
        }

        buyer.reservedBalance -= quote;
        seller.reservedBalance -= base;
        return true;
    }

    Account& SpotAccountLedger::accountFor(User& user) const noexcept
    {
        return user.getSpotAccount();
    }

    const Account& SpotAccountLedger::accountFor(const User& user) const noexcept
    {
        return user.getSpotAccount();
    }

    AccountType SpotAccountLedger::getAccountType() const noexcept
    {
        return AccountType::Spot;
    }
}
