#ifndef ADVMATCH_SINGLE_THREAD_CONSUMER_H
#define ADVMATCH_SINGLE_THREAD_CONSUMER_H

#include <disruptor/BlockingWaitStrategy.h>
#include <disruptor/BusySpinWaitStrategy.h>
#include <disruptor/RingBuffer.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "advmatch/consuming/RequestTicket.h"

namespace advmatch::consuming
{
    // Derive from this class and implement consume(). All calls to consume()
    // run on one worker thread, in ring sequence order. Call stop() from the
    // derived destructor before derived state is destroyed.
    template <typename Command, typename Response, typename WaitStrategy = disruptor::BlockingWaitStrategy>
    class SingleThreadConsumer
    {
        enum class Lifecycle : std::uint8_t
        {
            Created,
            Running,
            Stopping,
            Stopped
        };

        enum class EventType : std::uint8_t
        {
            Empty,
            Ticket,
            Detached,
            Stop
        };

        struct Event
        {
            std::optional<Command> command;
            ResponseSlot<Response>* responseSlot = nullptr;
            EventType type = EventType::Empty;
        };

        struct EventFactory : disruptor::EventFactory<Event>
        {
            Event newInstance() override
            {
                return {};
            }
        };

        using Ring = disruptor::MultiProducerRingBuffer<Event, WaitStrategy>;
        using Barrier = decltype(std::declval<Ring&>().newBarrier());

        WaitStrategy waitStrategy;
        std::shared_ptr<Ring> ring;
        Barrier barrier;
        disruptor::Sequence consumed;
        std::thread worker;
        std::thread::id workerId;
        std::mutex lifecycleMutex;
        std::atomic<Lifecycle> lifecycle{Lifecycle::Created};
        std::atomic<std::uint32_t> activeSubmitters{0};
        std::size_t progressBatchSize = 1;

        void beginSubmit()
        {
            if (this->workerId == std::this_thread::get_id())
            {
                throw std::logic_error("consumer cannot post from its worker thread");
            }

            if (this->lifecycle.load(std::memory_order_acquire) != Lifecycle::Running)
            {
                throw std::logic_error("consumer is not accepting commands");
            }

            this->activeSubmitters.fetch_add(1, std::memory_order_acq_rel);
            if (this->lifecycle.load(std::memory_order_acquire) != Lifecycle::Running)
            {
                this->endSubmit();
                throw std::logic_error("consumer is not accepting commands");
            }
        }

        void endSubmit() noexcept
        {
            if (this->activeSubmitters.fetch_sub(1, std::memory_order_acq_rel) == 1)
            {
                this->activeSubmitters.notify_all();
            }
        }

        std::int64_t claimNext() noexcept
        {
            return this->ring->next();
        }

        void run() noexcept
        {
            std::int64_t next = this->consumed.get() + 1;

            for (;;)
            {
                const auto available = this->barrier->waitFor(next);
                std::int64_t flushAt = next + static_cast<std::int64_t>(this->progressBatchSize) - 1;
                for (; next <= available; ++next)
                {
                    Event& event = this->ring->get(next);
                    if (event.type == EventType::Stop)
                    {
                        this->onStopped();
                        this->consumed.set(next);
                        return;
                    }

                    if (event.type == EventType::Detached)
                    {
                        try
                        {
                            consume(std::move(*event.command));
                        }
                        catch (...)
                        {
                            this->onDetachedFailure(std::current_exception());
                        }

                        event.command.reset();
                    }

                    if (event.type == EventType::Ticket)
                    {
                        ResponseSlot<Response>* responseSlot = event.responseSlot;
                        try
                        {
                            if constexpr (std::is_void_v<Response>)
                            {
                                consume(std::move(*event.command));
                                responseSlot->complete();
                            }
                            else
                            {
                                responseSlot->complete(consume(std::move(*event.command)));
                            }
                        }
                        catch (...)
                        {
                            responseSlot->fail(std::current_exception());
                        }

                        event.command.reset();
                        event.responseSlot = nullptr;
                    }

                    event.type = EventType::Empty;
                    // Flush bounded chunks and always release progress before
                    // waiting for another publication, including tiny rings.
                    if (next == available || next >= flushAt)
                    {
                        this->consumed.set(next);
                        flushAt = next + static_cast<std::int64_t>(this->progressBatchSize);
                    }
                }
            }
        }

    protected:
        virtual Response consume(Command&& command) = 0;

        virtual void onStopped() noexcept
        {
        }

        virtual void onDetachedFailure(std::exception_ptr) noexcept
        {
            std::terminate();
        }

        void stopBeforeDerivedDestruction() noexcept
        {
            try
            {
                this->stop();
            }
            catch (...)
            {
                std::terminate();
            }
        }

    public:
        SingleThreadConsumer() = default;

        // Configure only before start(). The worker and producers share it.
        WaitStrategy& waitStrategyConfig() noexcept
        {
            return this->waitStrategy;
        }

        virtual ~SingleThreadConsumer() noexcept
        {
            this->stopBeforeDerivedDestruction();
        }

        SingleThreadConsumer(const SingleThreadConsumer&) = delete;
        SingleThreadConsumer& operator=(const SingleThreadConsumer&) = delete;

        void start(int ringSize, std::size_t progressBatchSize = 0)
        {
            std::lock_guard lock(this->lifecycleMutex);
            if (this->lifecycle.load(std::memory_order_relaxed) != Lifecycle::Created)
            {
                throw std::logic_error("consumer can only be started once");
            }

            if (ringSize <= 0 || progressBatchSize > static_cast<std::size_t>(ringSize))
            {
                throw std::invalid_argument("progress batch must fit in a positive ring size");
            }

            this->progressBatchSize = progressBatchSize == 0
                ? std::min<std::size_t>(32, std::max(1, ringSize / 4)) : progressBatchSize;
            this->ring = Ring::createMultiProducer(std::make_shared<EventFactory>(), ringSize, this->waitStrategy);
            this->ring->addGatingSequences(this->consumed);
            this->barrier = this->ring->newBarrier();
            this->worker = std::thread(&SingleThreadConsumer::run, this);
            this->workerId = this->worker.get_id();
            this->lifecycle.store(Lifecycle::Running, std::memory_order_release);
        }

        RequestTicket<Response> post(Command command, ResponseSlot<Response>& responseSlot)
        {
            static_assert(std::is_nothrow_move_constructible_v<Command>,
                          "fast commands must be nothrow move constructible");

            this->beginSubmit();
            try
            {
                responseSlot.prepare();

                const auto sequence = this->claimNext();
                Event& event = this->ring->get(sequence);
                event.command.emplace(std::move(command));
                event.responseSlot = &responseSlot;
                event.type = EventType::Ticket;
                this->ring->publish(sequence);

                this->endSubmit();
                return RequestTicket<Response>(&responseSlot);
            }
            catch (...)
            {
                this->endSubmit();
                throw;
            }
        }

        // A future-owning command does not need an external ResponseSlot.
        // Queue exhaustion returns false immediately; command execution must
        // route failures to its own completion source (or override the hook).
        bool tryPostDetached(Command& command) requires std::is_void_v<Response>
        {
            static_assert(std::is_nothrow_move_constructible_v<Command>);
            this->beginSubmit();
            const auto sequence = this->ring->tryNext();
            if (!sequence)
            {
                this->endSubmit();
                return false;
            }

            Event& event = this->ring->get(sequence.value());
            event.command.emplace(std::move(command));
            event.responseSlot = nullptr;
            event.type = EventType::Detached;
            this->ring->publish(sequence.value());
            this->endSubmit();
            return true;
        }

        // Claims and publishes several adjacent ring entries at once. This
        // reduces cursor contention and consumer wakeups under many producers.
        // Commands and response slots must remain alive until every ticket is
        // consumed. Keep publishBatchSize well below the ring size so one
        // producer cannot monopolize the complete queue.
        void postBatch(std::span<Command> commands, std::span<ResponseSlot<Response>> responseSlots,
                       std::vector<RequestTicket<Response>>& tickets, std::size_t publishBatchSize)
        {
            static_assert(std::is_nothrow_move_constructible_v<Command>,
                          "fast commands must be nothrow move constructible");

            if (commands.size() != responseSlots.size())
            {
                throw std::invalid_argument("commands and response slots must have the same size");
            }

            if (!tickets.empty())
            {
                throw std::invalid_argument("ticket output must be empty");
            }

            if (commands.empty())
            {
                return;
            }

            if (publishBatchSize == 0 || publishBatchSize > static_cast<std::size_t>(this->ring->getBufferSize()))
            {
                throw std::invalid_argument("publish batch size must fit in the ring buffer");
            }

            tickets.reserve(commands.size());
            this->beginSubmit();
            std::size_t prepared = 0;
            try
            {
                for (; prepared < responseSlots.size(); ++prepared)
                {
                    responseSlots[prepared].prepare();
                }
            }
            catch (...)
            {
                for (std::size_t index = 0; index < prepared; ++index)
                {
                    responseSlots[index].release();
                }

                this->endSubmit();
                throw;
            }

            std::size_t offset = 0;
            while (offset < commands.size())
            {
                const std::size_t count = std::min(publishBatchSize, commands.size() - offset);
                const std::int64_t highSequence = this->ring->next(static_cast<int>(count));
                const std::int64_t lowSequence = highSequence - static_cast<std::int64_t>(count) + 1;

                for (std::size_t index = 0; index < count; ++index)
                {
                    Event& event = this->ring->get(lowSequence + static_cast<std::int64_t>(index));
                    event.command.emplace(std::move(commands[offset + index]));
                    event.responseSlot = &responseSlots[offset + index];
                    event.type = EventType::Ticket;
                    tickets.push_back(RequestTicket<Response>(&responseSlots[offset + index]));
                }

                this->ring->publish(lowSequence, highSequence);
                offset += count;
            }

            this->endSubmit();
        }

        void stop()
        {
            std::unique_lock lock(this->lifecycleMutex);
            if (this->workerId == std::this_thread::get_id())
            {
                throw std::logic_error("consumer cannot stop itself");
            }

            if (this->lifecycle.load(std::memory_order_acquire) != Lifecycle::Running)
            {
                return;
            }

            this->lifecycle.store(Lifecycle::Stopping, std::memory_order_release);
            std::uint32_t submitters = this->activeSubmitters.load(std::memory_order_acquire);
            while (submitters != 0)
            {
                this->activeSubmitters.wait(submitters, std::memory_order_acquire);
                submitters = this->activeSubmitters.load(std::memory_order_acquire);
            }

            const auto sequence = this->claimNext();
            Event& event = this->ring->get(sequence);
            event.command.reset();
            event.responseSlot = nullptr;
            event.type = EventType::Stop;
            this->ring->publish(sequence);

            this->worker.join();
            this->lifecycle.store(Lifecycle::Stopped, std::memory_order_release);
        }
    };
}

#endif // ADVMATCH_SINGLE_THREAD_CONSUMER_H
