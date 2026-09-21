#include "advmatch/account/User.h"
#include "advmatch/management/UserManager.h"
#include "advmatch/util/BigAllocs.h"
#include "advmatch/util/Containers.h"

#include <cstdint>
#include <cstdlib>
#include <vector>

#define CHECK(condition) \
    do \
    { \
        if (!(condition)) \
        { \
            std::abort(); \
        } \
    } while (false)

namespace
{
    struct TrackedValue
    {
        static inline int alive = 0;

        explicit TrackedValue(int value)
            : value(value)
        {
            ++alive;
        }

        ~TrackedValue()
        {
            --alive;
        }

        int value;
    };

    struct alignas(64) CacheLineValue
    {
        explicit CacheLineValue(std::uint64_t value)
            : value(value)
        {}

        std::uint64_t value;
    };
}

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

    BigAllocs<TrackedValue, 2, 4> values;
    CHECK(values.capacity() == 8);
    CHECK(values.empty());

    std::vector<TrackedValue*> allocated;
    allocated.reserve(9);
    for (int value = 0; value < 9; ++value)
    {
        allocated.push_back(values.emplace(value));
    }

    CHECK(values.size() == 9);
    CHECK(values.capacity() == 12);
    CHECK(TrackedValue::alive == 9);

    TrackedValue* released = allocated[3];
    CHECK(values.release(released));
    CHECK(!values.release(released));
    CHECK(values.size() == 8);
    CHECK(TrackedValue::alive == 8);

    TrackedValue* reused = values.emplace(99);
    CHECK(reused == released);
    CHECK(reused->value == 99);

    values.reserve(25);
    CHECK(values.capacity() == 28);
    CHECK(allocated.front()->value == 0);

    values.clear();
    CHECK(values.empty());
    CHECK(TrackedValue::alive == 0);

    CacheLineValue* aligned = nullptr;
    {
        BigAllocs<CacheLineValue, 1, 2> alignedValues;
        aligned = alignedValues.emplace(42);
        CHECK(reinterpret_cast<std::uintptr_t>(aligned) % alignof(CacheLineValue) == 0);
        CHECK(aligned->value == 42);
    }
}
