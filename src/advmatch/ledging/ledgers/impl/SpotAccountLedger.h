#ifndef ADVMATCH_SPOT_ACCOUNT_LEDGER_H
#define ADVMATCH_SPOT_ACCOUNT_LEDGER_H

#include "../AccountLedger.h"
#include "../SettlementContext.h"

namespace advmatch
{
    class SpotAccountLedger final : public AccountLedger
    {
    protected:
        [[nodiscard]]
        Account& accountFor(User& user) const noexcept override;

        [[nodiscard]]
        const Account& accountFor(const User& user) const noexcept override;

    public:
        struct SettlementPlan
        {
            std::array<Account*, 4> accounts{};
            std::array<std::optional<Account::BalanceUpdate>, 4> updates;
            std::size_t count = 0;
            Balance base = 0;
            Balance quote = 0;
        };

        std::optional<SettlementPlan> prepare(OrderNode& buyer, OrderNode& seller, PositiveBalanceChange baseAmount, PositiveBalanceChange quoteAmount, Balance buyerFee, Balance sellerFee, SettlementContext* context = nullptr) const;
        void commit(const SettlementPlan& plan, OrderNode& buyer, OrderNode& seller) const;

        bool settle(OrderNode& buyer, OrderNode& seller, PositiveBalanceChange baseAmount, PositiveBalanceChange quoteAmount,
            SettlementContext* context = nullptr, Balance buyerFee = 0, Balance sellerFee = 0) const;

        bool canSettle(OrderNode& buyer, OrderNode& seller, PositiveBalanceChange baseAmount, PositiveBalanceChange quoteAmount, Balance buyerFee, Balance sellerFee) const;

        [[nodiscard]]
        AccountType getAccountType() const noexcept override;
    };
}

#endif // ADVMATCH_SPOT_ACCOUNT_LEDGER_H
