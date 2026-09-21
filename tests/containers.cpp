#include "advmatch/account/User.h"
#include "advmatch/management/UserManager.h"
#include "advmatch/util/Containers.h"

#include <cstdlib>

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

    SmallVector<int, 10> smallValues;
    for (int value = 0; value < 10; ++value)
    {
        smallValues.push_back(value);
    }
    CHECK(smallValues.size() == 10);

    DenseMap<int, int> denseValues;
    denseValues.emplace(1, 10);
    CHECK(denseValues.at(1) == 10);

    FlatHashMap<int, int> flatValues;
    flatValues.emplace(2, 20);
    CHECK(flatValues.at(2) == 20);

    NodeHashMap<int, int> stableValues;
    stableValues.emplace(1, 10);
    int* stableValue = &stableValues.find(1)->second;
    stableValues.reserve(1024);
    for (int value = 2; value <= 1000; ++value)
    {
        stableValues.emplace(value, value * 10);
    }
    CHECK(&stableValues.find(1)->second == stableValue);
    CHECK(*stableValue == 10);

    UserManager users;
    User* firstUser = users.getUser(users.createUser());
    CHECK(firstUser);
    users.reserve(1024);
    for (int index = 0; index < 1000; ++index)
    {
        static_cast<void>(users.createUser());
    }
    CHECK(users.getUser(firstUser->getUserId()) == firstUser);

    User user(1);
    for (AssetId assetId = 1; assetId <= 6; ++assetId)
    {
        CHECK(user.updateBalance(assetId, static_cast<Balance>(assetId * 10)));
    }

    CHECK(user.getAssetBalanceCount() == 6);
    CHECK(user.lockBalance(6, 25));
    CHECK(user.queryActiveBalance(6) == 35);
    CHECK(user.queryLockedBalance(6) == 25);

    CHECK(user.updateBalance(7, 70));
    CHECK(user.updateBalance(8, 80));
    CHECK(user.getAssetBalanceCount() == 8);
    CHECK(user.unlockBalance(6, 25));

    for (AssetId assetId = 1; assetId <= 8; ++assetId)
    {
        CHECK(user.queryActiveBalance(assetId) == static_cast<Balance>(assetId * 10));
    }

    for (AssetId assetId = 1; assetId <= 8; ++assetId)
    {
        CHECK(user.updateBalance(assetId, -static_cast<Balance>(assetId * 10)));
    }

    CHECK(user.getAssetBalanceCount() == 0);
    CHECK(user.updateBalance(99, 100));
    CHECK(user.queryActiveBalance(99) == 100);
}
