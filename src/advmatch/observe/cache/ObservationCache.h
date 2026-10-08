#ifndef ADVMATCH_OBSERVATION_CACHE_H
#define ADVMATCH_OBSERVATION_CACHE_H

#include "../impl/TradingObserver.h"

#include <atomic>
#include <memory>
#include <stdexcept>

namespace advmatch::observe
{
    class ObservationCache;

    // An immutable query plus an atomically published, owning response. Readers
    // never access the live engine or the publisher's containers. The standard
    // does not guarantee atomic<shared_ptr> is lock-free on every platform.
    class ObservationSlot
    {
        struct Budget
        {
            const std::size_t maximum;
            std::atomic<std::size_t> used{0};

            explicit Budget(std::size_t maximum)
                : maximum(maximum)
            {
            }
        };

        friend class ObservationCache;
        const ObservationQuery selection;
        std::shared_ptr<Budget> budget;
        std::atomic<std::shared_ptr<const ObservationResult>> published;

        ObservationSlot(ObservationQuery query, std::shared_ptr<Budget> budget)
            : selection(std::move(query)), budget(std::move(budget))
        {
        }

    public:
        ~ObservationSlot()
        {
            this->budget->used.fetch_sub(1, std::memory_order_relaxed);
        }

        ObservationSlot(const ObservationSlot&) = delete;
        ObservationSlot& operator=(const ObservationSlot&) = delete;

        const ObservationQuery& query() const noexcept
        {
            return this->selection;
        }

        std::shared_ptr<const ObservationResult> load() const noexcept
        {
            return this->published.load(std::memory_order_acquire);
        }

        bool publish(std::shared_ptr<const ObservationResult> result) noexcept
        {
            if (result == nullptr || result->query != this->selection)
            {
                return false;
            }

            auto previous = this->published.load(std::memory_order_acquire);
            for (;;)
            {
                if (previous != nullptr)
                {
                    const auto& oldStamp = previous->page.stamp;
                    const auto& newStamp = result->page.stamp;
                    if (newStamp.session < oldStamp.session || (newStamp.session == oldStamp.session &&
                        (newStamp.sequence < oldStamp.sequence || newStamp.revision < oldStamp.revision || newStamp.durableThrough < oldStamp.durableThrough || (oldStamp.sourceFaulted && !newStamp.sourceFaulted))))
                    {
                        return false;
                    }
                }

                if (this->published.compare_exchange_weak(previous, result, std::memory_order_release, std::memory_order_acquire))
                {
                    return true;
                }
            }
        }

        // Owner thread only. Allocation/copying happens at explicit refresh,
        // never in reader load() or implicitly after each trading command.
        std::shared_ptr<const ObservationResult> refresh(const TradingObserver& observer)
        {
            auto result = std::make_shared<const ObservationResult>(observer.read(this->selection));
            this->publish(result);
            return result;
        }
    };

    class ObservationCache
    {
        std::shared_ptr<ObservationSlot::Budget> budget;

    public:
        explicit ObservationCache(std::size_t maximumSlots = 1024)
            : budget(std::make_shared<ObservationSlot::Budget>(maximumSlots))
        {
        }

        std::shared_ptr<ObservationSlot> create(ObservationQuery query)
        {
            auto used = this->budget->used.load(std::memory_order_relaxed);
            do
            {
                if (used >= this->budget->maximum)
                {
                    throw std::length_error("observation cache slot limit reached");
                }
            } while (!this->budget->used.compare_exchange_weak(used, used + 1, std::memory_order_acq_rel));

            // shared_ptr allocates its control block separately. A failed
            // construction releases the reservation exactly once.
            ObservationSlot* slot = nullptr;
            try
            {
                slot = new ObservationSlot(std::move(query), this->budget);
            }
            catch (...)
            {
                this->budget->used.fetch_sub(1, std::memory_order_relaxed);
                throw;
            }

            return std::shared_ptr<ObservationSlot>(slot);
        }

        std::size_t size() const noexcept
        {
            return this->budget->used.load(std::memory_order_relaxed);
        }
    };
}

#endif // ADVMATCH_OBSERVATION_CACHE_H
