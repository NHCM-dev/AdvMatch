#ifndef ADVMATCH_SDK_ASSET_H
#define ADVMATCH_SDK_ASSET_H

#include "Entity.h"

namespace advmatch::sdk
{
    class Asset final : public Entity<AssetView, AssetId, ReadKind::Asset>
    {
    public:
        using Entity::Entity;

        CompletableFuture<CommandReceipt> remove(std::uint64_t requestId = 0) const
        {
            return detail::Results::checked(this->api, commands::RemoveAsset{this->id()}, requestId);
        }
    };
}

#endif // ADVMATCH_SDK_ASSET_H
