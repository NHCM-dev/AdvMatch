#ifndef ADVMATCH_TYPES_H
#define ADVMATCH_TYPES_H

#include <absl/container/flat_hash_map.h>
#include <absl/container/node_hash_map.h>
#include <ankerl/unordered_dense.h>
#include <boost/container/small_vector.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>

#if !defined(__SIZEOF_INT128__) || defined(ADVMATCH_FORCE_PORTABLE_WIDE_BALANCE)
#include <boost/multiprecision/cpp_int.hpp>
#endif

namespace advmatch
{
    namespace observe
    {
        class TradingObserver;
    }

    using UserId = std::uint64_t;
    using AccountId = std::uint64_t;
    using AssetId = std::uint32_t;
    using SymbolId = std::uint32_t;
    using OrderId = std::uint64_t;
    using PositionId = std::uint64_t;

    using Price = std::uint64_t;
    using Quantity = std::uint64_t;
    using QuantityChange = std::int64_t;

    // Balance is a stored ledging value. BalanceChange is a signed delta that
    // can credit or debit a balance. PositiveBalanceChange is a non-negative
    // amount used by reserve, release, consume, and settlement operations.
#if defined(__SIZEOF_INT128__) && !defined(ADVMATCH_FORCE_PORTABLE_WIDE_BALANCE)
    using WideBalance = __int128;
#else
    using WideBalance = boost::multiprecision::int128_t;
#endif

    using Balance = std::int64_t;
    using BalanceChange = std::int64_t;
    using PositiveBalanceChange = std::uint64_t;

    using Scale = std::uint8_t;
    using LeverageMultiplier = std::uint16_t;
    // Fixed-point decimal rate; Position.h defines the scale.
    using MMRRate = std::uint32_t;

    // Milliseconds since the Unix epoch.
    using Timestamp = std::uint64_t;
    using CommandSequence = std::uint64_t;

    // Elements are stored inline until InlineCapacity is exceeded. Growing past
    // the capacity invalidates every pointer, reference, and iterator. Erasing
    // an element invalidates the erased element and every element after it.
    template <typename Value, std::size_t InlineCapacity>
    using SmallVector = boost::container::small_vector<Value, InlineCapacity>;

    // Values are stored contiguously. Insert and erase operations can move
    // elements, so pointers, references, and iterators must not be retained
    // across any mutation.
    template <typename Key, typename Value>
    using DenseMap = ankerl::unordered_dense::map<Key, Value>;

    template <typename Key, typename Hash = ankerl::unordered_dense::hash<Key>, typename KeyEqual = std::equal_to<Key>>
    using DenseSet = ankerl::unordered_dense::set<Key, Hash, KeyEqual>;

    // Values are stored directly in the hash table. A rehash invalidates every
    // pointer, reference, and iterator to an element. Use absl::node_hash_map or
    // an owning pointer as the mapped value when stable addresses are required.
    template <typename Key, typename Value>
    using FlatHashMap = absl::flat_hash_map<Key, Value>;

    // Values live in separately allocated nodes. References and pointers remain
    // valid across insertion and rehash, and become invalid only when their
    // element is erased or the container is destroyed.
    template <typename Key, typename Value>
    using NodeHashMap = absl::node_hash_map<Key, Value>;

    enum class PlaceOrderState : std::uint8_t;
    enum class ModifyOrderState : std::uint8_t;

    template <typename RejectReason>
    struct RiskDecision;

    using PlaceOrderRiskDecision = RiskDecision<PlaceOrderState>;
    using ModifyOrderRiskDecision = RiskDecision<ModifyOrderState>;
}

#endif // ADVMATCH_TYPES_H
