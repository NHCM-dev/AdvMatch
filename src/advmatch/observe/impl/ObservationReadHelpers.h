#ifndef ADVMATCH_OBSERVATION_READ_HELPERS_H
#define ADVMATCH_OBSERVATION_READ_HELPERS_H

#include "../model/ObservationTypes.h"

#include <array>
#include <algorithm>
#include <limits>

namespace advmatch::observe::detail
{
    template <typename Map, typename Visit>
    void scanNodes(const Map& values, const ObservationQuery& query, ReadPage& page, Visit&& visit)
    {
        auto iterator = values.begin();
        if (query.cursor.started && query.cursor.anchor != 0)
        {
            using Key = typename Map::key_type;
            if (query.cursor.anchor > std::numeric_limits<Key>::max())
            {
                page.state = ReadState::CursorExpired;
                return;
            }

            iterator = values.find(static_cast<Key>(query.cursor.anchor));
            if (iterator == values.end())
            {
                page.state = ReadState::CursorExpired;
                return;
            }

            ++iterator;
        }

        while (iterator != values.end() && page.count < query.limit && page.examined < query.scanBudget)
        {
            page.next.anchor = iterator->first;
            ++page.examined;
            page.count += visit(iterator->first, iterator->second);
            ++iterator;
        }

        page.hasMore = iterator != values.end();
    }

    template <typename Visit>
    void scanSlots(std::size_t size, const ObservationQuery& query, ReadPage& page, Visit&& visit)
    {
        std::size_t index = static_cast<std::size_t>(query.cursor.anchor);
        if (query.cursor.anchor > size)
        {
            page.state = ReadState::CursorExpired;
            return;
        }

        while (index < size && page.count < query.limit && page.examined < query.scanBudget)
        {
            ++page.examined;
            page.count += visit(index++);
        }

        page.next.anchor = index;
        page.hasMore = index < size;
    }

    template <typename Values, typename Visit>
    void scanHistoryIndex(const Values& values, const ObservationQuery& query, ReadPage& page, Visit&& visit)
    {
        const auto boundary = std::make_pair(static_cast<Timestamp>(query.cursor.anchor), query.cursor.secondary);
        auto iterator = query.cursor.started ? std::lower_bound(values.begin(), values.end(), boundary) : values.end();
        while (iterator != values.begin() && page.count < query.limit && page.examined < query.scanBudget)
        {
            const auto& [timestamp, id] = *--iterator;
            ++page.examined;
            page.count += visit(id);
            page.next.anchor = timestamp;
            page.next.secondary = id;
        }
        page.hasMore = iterator != values.begin();
    }

    // Descending immutable IDs: appends cannot shift the continuation boundary.
    // A newer first page starts a new traversal; existing cursors exclude new rows.
    template <typename Visit>
    void scanNewest(std::uint64_t upper, const ObservationQuery& query, ReadPage& page, Visit&& visit)
    {
        auto id = query.cursor.started ? query.cursor.anchor : upper;
        if (id > upper)
        {
            page.state = ReadState::CursorExpired;
            return;
        }
        while (id != 0 && page.count < query.limit && page.examined < query.scanBudget)
        {
            ++page.examined;
            page.count += visit(id--);
        }
        page.next.anchor = id;
        page.hasMore = id != 0;
    }

    template <typename Row, typename Visit>
    void scanTape(const std::vector<Row>& values, std::uint64_t nextId, const ObservationQuery& query, ReadPage& page, Visit&& visit)
    {
        if (values.empty())
        {
            page.state = ReadState::HistoryUnavailable;
            return;
        }

        const auto oldest = nextId - std::min<std::uint64_t>(nextId - 1, values.size());
        auto id = query.cursor.started ? query.cursor.anchor : oldest;
        if (id < oldest || id > nextId)
        {
            page.state = ReadState::CursorExpired;
            return;
        }

        while (id < nextId && page.count < query.limit && page.examined < query.scanBudget)
        {
            ++page.examined;
            page.count += visit(values[(id - 1) % values.size()]);
            ++id;
        }

        page.next.anchor = id;
        page.hasMore = id < nextId;
    }

    inline bool matchesOrder(const OrderView& value, const ObservationQuery& query) noexcept
    {
        return (query.userId == 0 || value.userId == query.userId) && (query.accountId == 0 || value.accountId == query.accountId) &&
            (query.symbolId == 0 || value.symbolId == query.symbolId) && (query.price == 0 || value.price == query.price) && (query.includeTerminal || !isOrderStatusTerminal(value.filling.status));
    }

    inline bool matchesPosition(const PositionView& value, const ObservationQuery& query) noexcept
    {
        const bool exposure = query.positionFilter == PositionFilter::All ||
            (query.positionFilter == PositionFilter::ClosedExecuted && value.closed) ||
            (query.positionFilter == PositionFilter::Open && value.size != 0) ||
            (query.positionFilter == PositionFilter::Trading && (value.size != 0 || value.openOrderCount != 0));
        return exposure && (query.userId == 0 || value.userId == query.userId) &&
            (query.accountId == 0 || value.accountId == query.accountId) && (query.symbolId == 0 || value.symbolId == query.symbolId);
    }

    template <typename Row>
    std::vector<Row>& rows(ObservationResult& result)
    {
        auto& values = result.data.emplace<std::vector<Row>>();
        values.reserve(result.query.limit);
        return values;
    }

    template <typename Row>
    void point(ObservationResult& result, std::optional<Row> value)
    {
        auto& output = result.data.emplace<std::vector<Row>>();
        output.reserve(1);
        result.page.examined = 1;
        if (value)
        {
            output.push_back(std::move(*value));
            result.page.count = 1;
        }
        else
        {
            result.page.state = ReadState::NotFound;
        }
    }
}

#endif // ADVMATCH_OBSERVATION_READ_HELPERS_H
