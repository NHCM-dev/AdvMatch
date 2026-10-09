#include "SpotAccountLedger.h"

#include "advmatch/ledging/User.h"

#include <array>
#include <stdexcept>

namespace advmatch
{
    bool SpotAccountLedger::settle(OrderNode& buyer, OrderNode& seller, PositiveBalanceChange baseAmount, PositiveBalanceChange quoteAmount,
        SettlementContext* context, Balance buyerFee, Balance sellerFee) const
    {
        const auto plan = this->prepare(buyer, seller, baseAmount, quoteAmount, buyerFee, sellerFee, context);
        if (!plan)
        {
            return false;
        }

        this->commit(*plan, buyer, seller);
        return true;
    }

    bool SpotAccountLedger::canSettle(OrderNode& buyer, OrderNode& seller, PositiveBalanceChange baseAmount, PositiveBalanceChange quoteAmount, Balance buyerFee, Balance sellerFee) const
    {
        return this->prepare(buyer, seller, baseAmount, quoteAmount, buyerFee, sellerFee).has_value();
    }

    std::optional<SpotAccountLedger::SettlementPlan> SpotAccountLedger::prepare(OrderNode& buyer, OrderNode& seller, PositiveBalanceChange baseAmount, PositiveBalanceChange quoteAmount, Balance buyerFee, Balance sellerFee, SettlementContext* context) const
    {
        if (buyerFee < 0 || sellerFee < 0 || static_cast<PositiveBalanceChange>(buyerFee) > baseAmount || static_cast<PositiveBalanceChange>(sellerFee) > quoteAmount || buyer.reservedBalance < 0 || seller.reservedBalance < 0 ||
            quoteAmount > static_cast<PositiveBalanceChange>(buyer.reservedBalance) ||
            baseAmount > static_cast<PositiveBalanceChange>(seller.reservedBalance))
        {
            return std::nullopt;
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
            {buyer.account, buyer.quoteAssetId, sameAccount ? quote - sellerFee : 0, -quote, std::nullopt},
            {seller.account, seller.baseAssetId, sameAccount ? base - buyerFee : 0, -base, std::nullopt},
            {buyer.account, buyer.baseAssetId, base - buyerFee, 0, std::nullopt},
            {seller.account, seller.quoteAssetId, quote - sellerFee, 0, std::nullopt}}};
        const std::size_t count = sameAccount ? 2 : 4;
        SettlementPlan plan;
        plan.base = base;
        plan.quote = quote;
        plan.count = count;
        for (std::size_t index = 0; index < count; ++index)
        {
            auto& change = changes[index];
            const auto balance = context ? context->capture(index, *change.account, change.assetId) : change.account->captureBalance(change.assetId);
            change.prepared = change.account->prepareBalanceChange(balance, change.active, change.reserved);
            if (!change.prepared)
            {
                return std::nullopt;
            }

            plan.accounts[index] = change.account;
            plan.updates[index] = *change.prepared;
        }

        return plan;
    }

    void SpotAccountLedger::commit(const SettlementPlan& plan, OrderNode& buyer, OrderNode& seller) const
    {
        // Debits precede credits. A credit can insert/promote the destination
        // asset but cannot invalidate an uncommitted token in that account.
        for (std::size_t index = 0; index < plan.count; ++index)
        {
            if (!plan.accounts[index]->commitBalanceChange(*plan.updates[index]))
            {
                throw std::logic_error("prepared spot settlement was invalidated");
            }
        }

        buyer.reservedBalance -= plan.quote;
        seller.reservedBalance -= plan.base;
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
