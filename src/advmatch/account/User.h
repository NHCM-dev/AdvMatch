#ifndef ADVMATCH_USER_H
#define ADVMATCH_USER_H
#include <memory>
#include <optional>

#include "../market/Asset.h"
#include "../util/Containers.h"
#include "../util/Utils.h"

namespace advmatch
{
    using UserId = std::uint64_t;
    class UserManager;

    class User
    {
        // Balance addresses are intentionally private and temporary. Adding or
        // removing an asset can move balances in either storage mode. Never
        // cache an AssetBalance pointer, reference, or iterator across a balance
        // mutation; find it again by AssetId after the mutation.
        static constexpr std::size_t InlineBalanceCapacity = 6;

        using InlineBalances = SmallVector<AssetBalanceEntry, InlineBalanceCapacity>;
        using OverflowBalances = DenseMap<AssetId, AssetBalance>;

        InlineBalances inlineBalances;
        std::unique_ptr<OverflowBalances> overflowBalances;

        const UserId userId;
        bool active = true;

        friend class UserManager;

        void deactivate() noexcept
        {
            this->active = false;
        }

        // Returned pointers remain valid only until the next operation that can
        // add or remove an asset balance from this User.
        [[nodiscard]]
        AssetBalance* findBalance(AssetId assetId) noexcept;
        [[nodiscard]]
        const AssetBalance* findBalance(AssetId assetId) const noexcept;

        AssetBalance& addBalance(AssetId assetId, AssetBalance balance);
        void removeBalance(AssetId assetId) noexcept;

    public:
        explicit User(UserId userId)
            : userId(userId)
        {}

        [[nodiscard]]
        std::optional<Balance> queryActiveBalance(AssetId assetId) const noexcept;
        [[nodiscard]]
        std::optional<Balance> queryLockedBalance(AssetId assetId) const noexcept;

        [[nodiscard]]
        BalanceResult updateBalance(AssetId assetId, BalanceChange balanceChange);

        [[nodiscard]]
        BalanceResult lockBalance(AssetId assetId, PositiveBalanceChange balanceChange);
        [[nodiscard]]
        BalanceResult unlockBalance(AssetId assetId, PositiveBalanceChange balanceChange);
        [[nodiscard]]
        BalanceResult consumeLockedBalance(AssetId assetId, PositiveBalanceChange balanceChange);

        [[nodiscard]]
        BalanceResult lockAsset(AssetId assetId);
        [[nodiscard]]
        BalanceResult unlockAsset(AssetId assetId);

        [[nodiscard]]
        UserId getUserId() const noexcept
        {
            return userId;
        }

        [[nodiscard]]
        std::size_t getAssetBalanceCount() const noexcept
        {
            if (this->overflowBalances)
            {
                return this->overflowBalances->size();
            }

            return this->inlineBalances.size();
        }

        [[nodiscard]]
        bool isActive() const noexcept
        {
            return this->active;
        }

    };
} // advmatch

#endif //ADVMATCH_USER_H
