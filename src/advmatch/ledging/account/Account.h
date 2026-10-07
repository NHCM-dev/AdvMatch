#ifndef ADVMATCH_ACCOUNT_H
#define ADVMATCH_ACCOUNT_H

#include "../../trading/Asset.h"
#include "../../util/Containers.h"
#include "../../util/Types.h"

#include <memory>
#include <optional>

namespace advmatch
{
    class AccountManager;

    enum class AccountType : std::uint8_t
    {
        Spot,
        Contract
    };

    class Account
    {
        static constexpr std::size_t InlineBalanceCapacity = 10;

        // Most accounts keep their balances inline. Promotion to DenseMap
        // moves every balance, and DenseMap mutations can move elements again.
        // Never retain an AssetBalance pointer, reference, or iterator across
        // any balance mutation.
        SmallVector<AssetBalanceEntry, InlineBalanceCapacity> inlineBalances;
        std::unique_ptr<DenseMap<AssetId, AssetBalance>> overflowBalances;

        const AccountId accountId;
        const UserId userId;
        bool active = true;

        friend class AccountManager;

        void deactivate() noexcept;

        [[nodiscard]]
        AssetBalance* findBalance(AssetId assetId) noexcept;

        [[nodiscard]]
        const AssetBalance* findBalance(AssetId assetId) const noexcept;

        AssetBalance& addBalance(AssetId assetId, AssetBalance balance);
        void removeBalance(AssetId assetId);

    protected:
        Account(AccountId accountId, UserId userId)
            : accountId(accountId), userId(userId)
        {
        }

    public:
        virtual ~Account() = default;

        Account(const Account&) = delete;
        Account& operator=(const Account&) = delete;
        Account(Account&&) = delete;
        Account& operator=(Account&&) = delete;

        [[nodiscard]]
        virtual AccountType getAccountType() const noexcept = 0;

        [[nodiscard]]
        std::optional<Balance> queryActiveBalance(AssetId assetId) const noexcept;

        [[nodiscard]]
        std::optional<Balance> queryReservedBalance(AssetId assetId) const noexcept;

        [[nodiscard]]
        BalanceResult updateBalance(AssetId assetId, BalanceChange balanceChange);

        [[nodiscard]]
        BalanceResult reserveBalance(AssetId assetId, PositiveBalanceChange amount);

        [[nodiscard]]
        BalanceResult releaseReservedBalance(AssetId assetId, PositiveBalanceChange amount);

        [[nodiscard]]
        BalanceResult consumeReservedBalance(AssetId assetId, PositiveBalanceChange amount);

        [[nodiscard]]
        BalanceResult reserveAsset(AssetId assetId);

        [[nodiscard]]
        BalanceResult releaseAsset(AssetId assetId);

        [[nodiscard]]
        AccountId getAccountId() const noexcept;

        [[nodiscard]]
        UserId getUserId() const noexcept;

        [[nodiscard]]
        std::size_t getAssetBalanceCount() const noexcept;

        [[nodiscard]]
        bool isActive() const noexcept;
        // Balances are borrowed only for this call. Do not mutate the account
        // or retain references in the callback. Asset ordering is unspecified.
        template <typename Visitor>
        void forEachBalance(Visitor&& visitor) const
        {
            if (this->overflowBalances)
            {
                for (const auto& [assetId, balance] : *this->overflowBalances)
                {
                    visitor(assetId, balance);
                }
            }
            else
            {
                for (const auto& entry : this->inlineBalances)
                {
                    visitor(entry.assetId, entry.balance);
                }
            }
        }

    };
}

#endif // ADVMATCH_ACCOUNT_H
