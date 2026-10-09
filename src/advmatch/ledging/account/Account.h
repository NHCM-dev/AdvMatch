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
        friend class observe::TradingObserver;
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
        std::uint64_t balanceGeneration = 0;

        friend class AccountManager;

        void deactivate() noexcept;

        [[nodiscard]]
        AssetBalance* findBalance(AssetId assetId) noexcept;

        [[nodiscard]]
        const AssetBalance* findBalance(AssetId assetId) const noexcept;

        AssetBalance& addBalance(AssetId assetId, AssetBalance balance);
        void removeBalance(AssetId assetId);

    protected:
        virtual bool canWithdraw(AssetId assetId, PositiveBalanceChange amount) const noexcept;

        Account(AccountId accountId, UserId userId)
            : accountId(accountId), userId(userId)
        {
        }

    public:
        // Borrowed locations never escape these opaque tokens. Structural
        // mutations invalidate tokens; commit checks before dereferencing.
        class BalanceSnapshot
        {
            friend class Account;
            const Account* owner = nullptr;
            const AssetBalance* location = nullptr;
            AssetBalance before{};
            AssetId assetId = 0;
            std::uint64_t generation = 0;

        public:
            Balance active() const noexcept
            {
                return this->before.active;
            }

            Balance reserved() const noexcept
            {
                return this->before.reserved;
            }
        };

        class BalanceUpdate
        {
            friend class Account;
            BalanceSnapshot snapshot;
            AssetBalance after{};
        };

        BalanceSnapshot captureBalance(AssetId assetId) const noexcept;
        // Refresh values through the opaque location only after structural
        // validation. Insert/erase/promotion forces a fresh asset lookup.
        BalanceSnapshot recaptureBalance(const BalanceSnapshot& snapshot) const noexcept;
        std::optional<BalanceUpdate> prepareBalanceChange(const BalanceSnapshot& snapshot,
            BalanceChange activeChange, BalanceChange reservedChange) const noexcept;
        BalanceResult commitBalanceChange(const BalanceUpdate& update);

        virtual ~Account() = default;

        [[nodiscard]]
        bool canApplyBalanceChange(AssetId assetId, BalanceChange activeChange, BalanceChange reservedChange) const noexcept;

        [[nodiscard]]
        BalanceResult applyBalanceChange(AssetId assetId, BalanceChange activeChange, BalanceChange reservedChange);

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
