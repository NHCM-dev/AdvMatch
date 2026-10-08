#ifndef ADVMATCH_OBSERVED_CONSUMER_H
#define ADVMATCH_OBSERVED_CONSUMER_H

#include "../cache/ObservationCache.h"
#include "advmatch/consuming/SingleThreadConsumer.h"

namespace advmatch::observe
{
    struct ObservationRequest
    {
        ObservationQuery query;
        std::shared_ptr<ObservationSlot> slot;
        std::vector<ObservationQuery> batch;
    };

    template <typename Command>
    using ObservedRequest = std::variant<Command, ObservationRequest>;

    template <typename Response>
    using ObservedResponse = std::variant<Response, std::shared_ptr<const ObservationResult>, std::shared_ptr<const ObservationBatch>>;

    // Derive, bindObserver() after members are initialized, then start(). Trade
    // commands and read requests share the same single consumer. No second
    // thread is allowed to read the mutable order/account containers.
    template <typename Command, typename Response>
    class ObservedConsumer : public consuming::SingleThreadConsumer<ObservedRequest<Command>, ObservedResponse<Response>>
    {
        TradingObserver* observer = nullptr;

        ObservedResponse<Response> consume(ObservedRequest<Command>&& request) final
        {
            if (this->observer == nullptr)
            {
                throw std::logic_error("bind an observer before starting the consumer");
            }

            if (auto* command = std::get_if<Command>(&request))
            {
                // Invalidate cursors even if a business command throws after a
                // partial mutation. Persistence additionally reports faulted.
                this->observer->notifyChanged();
                return ObservedResponse<Response>(std::in_place_index<0>, this->consumeCommand(std::move(*command)));
            }

            const auto& read = std::get<ObservationRequest>(request);
            if (!read.batch.empty())
            {
                if (read.slot != nullptr)
                {
                    throw std::invalid_argument("batch responses cannot be published into a single-query slot");
                }

                auto result = std::make_shared<const ObservationBatch>(this->observer->readBatch(read.batch));
                return ObservedResponse<Response>(std::in_place_index<2>, std::move(result));
            }

            auto result = std::make_shared<const ObservationResult>(this->observer->read(read.query));
            if (read.slot != nullptr)
            {
                read.slot->publish(result);
            }

            return ObservedResponse<Response>(std::in_place_index<1>, std::move(result));
        }

    protected:
        virtual Response consumeCommand(Command&& command) = 0;

        void bindObserver(TradingObserver& observer) noexcept
        {
            this->observer = &observer;
        }
    };
}

#endif // ADVMATCH_OBSERVED_CONSUMER_H
