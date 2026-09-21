#include "advmatch/management/AssetManager.h"

#include <cstdlib>
#include <stdexcept>

#define CHECK(condition) \
    do \
    { \
        if (!(condition)) \
        { \
            std::abort(); \
        } \
    } while (false)

int main()
{
    using namespace advmatch;

    AssetManager assets;
    Asset* bitcoin = assets.createAsset("Bitcoin", "BTC", 7);
    Asset* tether = assets.createAsset("Tether", "USDT", 6);
    const AssetId bitcoinId = bitcoin ? bitcoin->assetId : 0;

    CHECK(bitcoin && bitcoin->assetId == 1);
    CHECK(bitcoin->displayName == "Bitcoin" && bitcoin->assetName == "BTC" && bitcoin->scale == 7);
    CHECK(tether && tether->assetId == 2);
    CHECK(assets.getAssetCount() == 2);
    CHECK(assets.containsAsset(bitcoinId));
    CHECK(assets.containsAssetName("USDT"));
    CHECK(assets.getAsset(bitcoinId) == bitcoin);
    CHECK(assets.getAssetByName("USDT") == tether);
    CHECK(!assets.createAsset("Another Bitcoin", "BTC", 7));
    CHECK(!assets.createAsset("", "EMPTY", 2));
    CHECK(!assets.createAsset("Empty", "", 2));

    bool invalidScaleRejected = false;

    try
    {
        static_cast<void>(assets.createAsset("Invalid", "INVALID", 8));
    }
    catch (const std::range_error&)
    {
        invalidScaleRejected = true;
    }

    CHECK(invalidScaleRejected);
    CHECK(!assets.containsAssetName("INVALID"));
    CHECK(assets.removeAsset(bitcoinId));
    CHECK(!assets.removeAsset(bitcoinId));
    CHECK(!assets.containsAsset(bitcoinId));
    CHECK(!assets.containsAssetName("BTC"));
    CHECK(!assets.getAsset(bitcoinId));
    CHECK(!assets.getAssetByName("BTC"));
    CHECK(assets.getAssetCount() == 1);
}
