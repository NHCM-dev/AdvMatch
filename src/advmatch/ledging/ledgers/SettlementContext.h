#ifndef ADVMATCH_SETTLEMENT_CONTEXT_H
#define ADVMATCH_SETTLEMENT_CONTEXT_H

#include "advmatch/ledging/account/Account.h"

#include <array>

namespace advmatch
{
    // Reusable on the owner thread; each capture refreshes balance state.
    // Accounts must
    // outlive the context. Only opaque tokens are cached, never balance pointers.
    // Fixed slots avoid hashing/allocation: buyer quote, seller base, buyer base,
    // seller quote. A different participant/asset replaces that slot.
    class SettlementContext
    {
        struct Resolution
        {
            const Account* account = nullptr;
            AssetId assetId = 0;
            Account::BalanceSnapshot balance;
        };

        std::array<Resolution, 4> resolutions;

    public:
        Account::BalanceSnapshot capture(std::size_t slot, const Account& account, AssetId assetId) noexcept
        {
            Resolution& resolution = this->resolutions[slot];
            if (resolution.account != &account || resolution.assetId != assetId)
            {
                resolution.account = &account;
                resolution.assetId = assetId;
                resolution.balance = account.captureBalance(assetId);
            }
            else
            {
                resolution.balance = account.recaptureBalance(resolution.balance);
            }

            return resolution.balance;
        }
    };
}

#endif // ADVMATCH_SETTLEMENT_CONTEXT_H
