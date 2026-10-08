#include "advmatch/observe/ObservationCache.h"
#include "advmatch/persistence/CommandExecutor.h"

#include <chrono>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <stdexcept>

namespace
{
    using namespace advmatch;
    using namespace advmatch::observe;
    using Clock = std::chrono::steady_clock;

    double seconds(Clock::time_point start)
    {
        return std::chrono::duration<double>(Clock::now() - start).count();
    }

    void benchmark(std::size_t orders, std::size_t queries, bool indexed)
    {
        TradingState state;
        constexpr std::size_t UserCount = 1000;
        state.users.reserve(UserCount);
        state.matching.reserve(orders, 1, orders);
        if (!state.symbols.createSymbol("TEST", SymbolType::Spot, 1, 2))
        {
            throw std::runtime_error("symbol creation failed");
        }

        if (!state.matching.addSpotBook(1))
        {
            throw std::runtime_error("book creation failed");
        }

        state.matching.reservePriceLevels(1, 0, 1000);
        for (std::size_t index = 0; index < UserCount; ++index)
        {
            const auto userId = state.users.createUser();
            if (!state.users.getUser(userId)->getSpotAccount().updateBalance(1, std::numeric_limits<Balance>::max() / 2))
            {
                throw std::runtime_error("balance preparation failed");
            }
        }

        ObservationConfig config;
        config.indexUserOpenOrders = indexed;
        config.expectedUsersWithOrders = UserCount;
        config.tradeHistoryCapacity = 0;
        config.contractEventCapacity = 0;
        TradingObserver observer(state.assets, state.users, state.symbols, state.matching, config);
        TradeBuffer trades;
        auto start = Clock::now();
        for (std::size_t index = 0; index < orders; ++index)
        {
            const auto result = state.matching.placeOrder(index % UserCount + 1, 1, OrderSide::Ask, OrderType::Limit, TimeInForce::GTC, 100 + index % 1000, 1, index + 1, trades);
            if (!result)
            {
                throw std::runtime_error("benchmark order rejected");
            }
        }

        const auto writeSeconds = seconds(start);
        observer.notifyChanged();
        std::uint64_t checksum = 0;
        start = Clock::now();
        for (std::size_t index = 0; index < queries; ++index)
        {
            const auto value = observer.order((index * 7919ULL) % orders + 1);
            checksum += value->price + value->filling.unfilled;
        }

        const auto orderSeconds = seconds(start);
        ObservationQuery depth;
        depth.kind = ReadKind::Depth;
        depth.symbolId = 1;
        depth.side = OrderSide::Ask;
        depth.limit = 20;
        std::array<DepthView, 20> buffer;
        const auto depthQueries = std::max<std::size_t>(1, queries / 20);
        start = Clock::now();
        for (std::size_t index = 0; index < depthQueries; ++index)
        {
            const auto page = observer.readDepth(depth, buffer);
            checksum += page.count + buffer[0].quantity;
        }

        const auto depthSeconds = seconds(start);
        ObservationCache cache(1);
        auto handle = cache.create(depth);
        start = Clock::now();
        handle->refresh(observer);
        const auto refreshMicroseconds = seconds(start) * 1'000'000;
        start = Clock::now();
        for (std::size_t index = 0; index < queries; ++index)
        {
            auto image = handle->load();
            checksum += image->rows<DepthView>().front().quantity;
        }

        const auto cacheSeconds = seconds(start);
        std::printf("index=%s orders=%zu queries=%zu write_tps=%.0f order_reads_s=%.0f depth20_reads_s=%.0f cache_loads_s=%.0f refresh_us=%.2f checksum=%llu\n",
            indexed ? "on" : "off", orders, queries, orders / writeSeconds, queries / orderSeconds, depthQueries / depthSeconds,
            queries / cacheSeconds, refreshMicroseconds, static_cast<unsigned long long>(checksum));
    }
}

int main(int argc, char** argv)
{
    try
    {
        const auto orders = argc > 1 ? std::stoull(argv[1]) : 50'000;
        const auto queries = argc > 2 ? std::stoull(argv[2]) : 1'000'000;
        if (orders == 0 || queries == 0 || orders > 100'000'000 || queries > 1'000'000'000)
        {
            throw std::invalid_argument("expected orders in [1,100000000], queries in [1,1000000000]");
        }

        benchmark(orders, queries, false);
        benchmark(orders, queries, true);
        std::puts("Optional indexing adds write work. Cache refresh cost is measured separately from reader loads; no fills, network, WAL or settlement are modeled here.");
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "Observation benchmark failed: %s\n", error.what());
        return 1;
    }
}
