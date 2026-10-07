#ifndef ADVMATCH_ASSETMANAGER_H
#define ADVMATCH_ASSETMANAGER_H

#include "../trading/Asset.h"
#include "advmatch/util/Containers.h"

#include <string>

namespace advmatch
{
    class AssetManager
    {
        NodeHashMap<AssetId, Asset> assets;
        DenseMap<std::string, AssetId> assetsByName;
        AssetId nextAssetId = 1;

    public:
        AssetManager() = default;

        void reserve(std::size_t assetCount)
        {
            this->assets.reserve(assetCount);
            this->assetsByName.reserve(assetCount);
        }

        [[nodiscard]]
        Asset* createAsset(std::string displayName, std::string assetName, Scale scale);

        [[nodiscard]]
        bool removeAsset(AssetId assetId);

        [[nodiscard]]
        bool containsAsset(AssetId assetId) const noexcept;

        [[nodiscard]]
        bool containsAssetName(const std::string& assetName) const noexcept;

        // NodeHashMap keeps Asset addresses valid across insertions and
        // rehashes. Removing that asset invalidates its address.
        [[nodiscard]]
        Asset* getAsset(AssetId assetId) noexcept;

        [[nodiscard]]
        const Asset* getAsset(AssetId assetId) const noexcept;

        [[nodiscard]]
        Asset* getAssetByName(const std::string& assetName) noexcept;

        [[nodiscard]]
        const Asset* getAssetByName(const std::string& assetName) const noexcept;

        [[nodiscard]]
        std::size_t getAssetCount() const noexcept;

        auto begin()
        {
            return this->assets.begin();
        }

        auto end()
        {
            return this->assets.end();
        }

        auto begin() const
        {
            return this->assets.begin();
        }

        auto end() const
        {
            return this->assets.end();
        }
        // Run on the owning thread. The callback must not mutate this manager.
        template <typename Visitor>
        void forEachAsset(Visitor&& visitor) const
        {
            for (const auto& [id, value] : this->assets)
            {
                visitor(id, value);
            }
        }

    };
} // advmatch

#endif // ADVMATCH_ASSETMANAGER_H
