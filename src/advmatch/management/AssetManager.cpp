#include "AssetManager.h"

#include <utility>

namespace advmatch
{
    Asset* AssetManager::createAsset(std::string displayName, std::string assetName, Scale scale)
    {
        if (displayName.empty() || assetName.empty() || this->nextAssetId == 0)
        {
            return nullptr;
        }

        if (this->assetsByName.contains(assetName))
        {
            return nullptr;
        }

        const AssetId assetId = this->nextAssetId;
        auto result = this->assets.
                            emplace(assetId, Asset(std::move(displayName), std::move(assetName), assetId, scale));

        if (!result.second)
        {
            return nullptr;
        }

        try
        {
            this->assetsByName.emplace(result.first->second.assetName, assetId);
        }
        catch (...)
        {
            this->assets.erase(result.first);
            throw;
        }

        ++this->nextAssetId;

        return &result.first->second;
    }

    bool AssetManager::removeAsset(AssetId assetId)
    {
        const auto iterator = this->assets.find(assetId);

        if (iterator == this->assets.end())
        {
            return false;
        }

        this->assetsByName.erase(iterator->second.assetName);
        this->assets.erase(iterator);

        return true;
    }

    bool AssetManager::containsAsset(AssetId assetId) const noexcept
    {
        return this->assets.contains(assetId);
    }

    bool AssetManager::containsAssetName(const std::string& assetName) const noexcept
    {
        return this->assetsByName.contains(assetName);
    }

    Asset* AssetManager::getAsset(AssetId assetId) noexcept
    {
        const auto iterator = this->assets.find(assetId);

        if (iterator == this->assets.end())
        {
            return nullptr;
        }

        return &iterator->second;
    }

    const Asset* AssetManager::getAsset(AssetId assetId) const noexcept
    {
        const auto iterator = this->assets.find(assetId);

        if (iterator == this->assets.end())
        {
            return nullptr;
        }

        return &iterator->second;
    }

    Asset* AssetManager::getAssetByName(const std::string& assetName) noexcept
    {
        const auto iterator = this->assetsByName.find(assetName);

        if (iterator == this->assetsByName.end())
        {
            return nullptr;
        }

        return this->getAsset(iterator->second);
    }

    const Asset* AssetManager::getAssetByName(const std::string& assetName) const noexcept
    {
        const auto iterator = this->assetsByName.find(assetName);

        if (iterator == this->assetsByName.end())
        {
            return nullptr;
        }

        return this->getAsset(iterator->second);
    }

    std::size_t AssetManager::getAssetCount() const noexcept
    {
        return this->assets.size();
    }
} // advmatch
