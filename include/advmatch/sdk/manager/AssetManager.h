#ifndef ADVMATCH_SDK_ASSET_MANAGER_H
#define ADVMATCH_SDK_ASSET_MANAGER_H

#include "Manager.h"

namespace advmatch::sdk
{
    class AssetManager final : public EntityManager<Asset, ReadKind::Assets>
    {
    public:
        using EntityManager::EntityManager;

        CompletableFuture<Asset> create(std::string displayName, std::string assetName, Scale scale, std::uint64_t requestId = 0) const
        {
            return detail::Results::created<Asset>(this->api, commands::CreateAsset{std::move(displayName), std::move(assetName), scale}, requestId);
        }
    };
}

#endif // ADVMATCH_SDK_ASSET_MANAGER_H
