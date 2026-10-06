#include "advmatch/ledging/User.h"
#include "advmatch/management/UserManager.h"
#include "advmatch/util/BigAllocs.h"
#include "advmatch/util/Containers.h"

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <utility>
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

        TrackedValue(const TrackedValue& other)
            : value(other.value)
        {
            ++alive;
        }

        TrackedValue(TrackedValue&& other) noexcept
            : value(other.value)
        {
            other.value = -1;
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

    SpotAccount& account = firstUser->getSpotAccount();
    for (AssetId assetId = 1; assetId <= 6; ++assetId)
    {
        CHECK(account.updateBalance(assetId, static_cast<BalanceChange>(assetId * 10)));
    }

    CHECK(account.getAssetBalanceCount() == 6);
    CHECK(account.reserveBalance(6, 25));
    CHECK(account.queryActiveBalance(6) == 35);
    CHECK(account.queryReservedBalance(6) == 25);

    CHECK(account.updateBalance(7, 70));
    CHECK(account.updateBalance(8, 80));
    CHECK(account.getAssetBalanceCount() == 8);
    CHECK(account.releaseReservedBalance(6, 25));

    for (AssetId assetId = 1; assetId <= 8; ++assetId)
    {
        CHECK(account.queryActiveBalance(assetId) == static_cast<Balance>(assetId * 10));
    }

    for (AssetId assetId = 1; assetId <= 8; ++assetId)
    {
        CHECK(account.updateBalance(assetId, -static_cast<BalanceChange>(assetId * 10)));
    }

    CHECK(account.getAssetBalanceCount() == 0);
    CHECK(account.updateBalance(99, 100));
    CHECK(account.queryActiveBalance(99) == 100);

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

    {
        const TrackedValue original(123);
        TrackedValue* copied = values.insert(original);
        CHECK(copied != &original);
        CHECK(copied->value == 123);
        CHECK(original.value == 123);
        CHECK(values.size() == 1);
        CHECK(TrackedValue::alive == 2);

        values.reserve(100);
        CHECK(copied->value == 123);
        CHECK(values.release(copied));
        CHECK(TrackedValue::alive == 1);

        TrackedValue source(456);
        TrackedValue* moved = values.insert(std::move(source));
        CHECK(moved == copied);
        CHECK(moved->value == 456);
        CHECK(source.value == -1);
        CHECK(values.size() == 1);
        CHECK(TrackedValue::alive == 3);

        values.clear();
        CHECK(values.empty());
        CHECK(TrackedValue::alive == 2);
    }
    CHECK(TrackedValue::alive == 0);

    {
        BigAllocs<std::unique_ptr<TrackedValue>, 1, 2> ownedValues;
        auto owned = std::make_unique<TrackedValue>(789);
        TrackedValue* originalAddress = owned.get();
        auto* stored = ownedValues.insert(std::move(owned));
        CHECK(!owned);
        CHECK(stored->get() == originalAddress);
        CHECK(TrackedValue::alive == 1);
        CHECK(ownedValues.release(stored));
        CHECK(ownedValues.empty());
        CHECK(TrackedValue::alive == 0);
    }

    CacheLineValue* aligned = nullptr;
    {
        BigAllocs<CacheLineValue, 1, 2> alignedValues;
        aligned = alignedValues.emplace(42);
        CHECK(reinterpret_cast<std::uintptr_t>(aligned) % alignof(CacheLineValue) == 0);
        CHECK(aligned->value == 42);
    }
}
