#ifndef ADVMATCH_USER_H
#define ADVMATCH_USER_H
#include <map>
#include <optional>

#include "../market/Asset.h"
#include "../util/Utils.h"

namespace advmatch
{
    using UserId = std::uint64_t;

    class UserManager;

    class User
    {
        std::map<AssetId, AssetBalance> balances;
        const UserId userId;

    public:
        class ConstructorKey
        {
            friend class UserManager;

            ConstructorKey() = default;
        };

        explicit User(ConstructorKey, UserId userId)
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
        BalanceResult lockAsset(AssetId assetId);
        [[nodiscard]]
        BalanceResult unlockAsset(AssetId assetId);

        [[nodiscard]]
        UserId getUserId() const noexcept;
    };
} // advmatch

#endif //ADVMATCH_USER_H