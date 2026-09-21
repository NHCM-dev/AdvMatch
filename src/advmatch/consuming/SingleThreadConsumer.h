#ifndef ADVMATCH_SINGLE_THREAD_CONSUMER_H
#define ADVMATCH_SINGLE_THREAD_CONSUMER_H

#include <disruptor/BusySpinWaitStrategy.h>
#include <disruptor/RingBuffer.h>

#include <atomic>
#include <cstdint>
#include <exception>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>

#include "advmatch/consuming/RequestTicket.h"
#include "disruptor/BlockingWaitStrategy.h"

namespace advmatch::consuming
{
    // Derive from this class and implement consume(). All calls to consume()
    // run on one worker thread, in ring sequence order. Call stop() from the
    // derived destructor before derived state is destroyed.
    template <typename Command, typename Response>
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
            Future,
            Ticket,
            Stop
        };

        struct Request
        {
            explicit Request(Command command)
                : command(std::move(command))
            {}

            Command command;
            std::promise<Response> promise;
        };

        struct Event
        {
            std::optional<Command> command;
            std::shared_ptr<Request> request;
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

        using Ring = disruptor::MultiProducerRingBuffer<Event, disruptor::BlockingWaitStrategy>;
        using Barrier = decltype(std::declval<Ring&>().newBarrier());

        disruptor::BlockingWaitStrategy waitStrategy;
        std::shared_ptr<Ring> ring;
        Barrier barrier;
        disruptor::Sequence consumed;
        std::thread worker;
        std::thread::id workerId;
        std::mutex lifecycleMutex;
        std::atomic<Lifecycle> lifecycle{Lifecycle::Created};
        std::atomic<std::uint32_t> activeSubmitters{0};

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

        void run() noexcept
        {
            std::int64_t next = this->consumed.get() + 1;

            for (;;)
            {
                const auto available = this->barrier->waitFor(next);
                for (; next <= available; ++next)
                {
                    Event& event = this->ring->get(next);
                    if (event.type == EventType::Stop)
                    {
                        this->consumed.set(next);
                        return;
                    }

                    if (event.type == EventType::Future)
                    {
                        std::shared_ptr<Request> request = std::move(event.request);
                        try
                        {
                            if constexpr (std::is_void_v<Response>)
                            {
                                consume(std::move(request->command));
                                request->promise.set_value();
                            }
                            else
                            {
                                request->promise.set_value(consume(std::move(request->command)));
                            }
                        }
                        catch (...)
                        {
                            request->promise.set_exception(std::current_exception());
                        }
                    }
                    else if (event.type == EventType::Ticket)
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
                    this->consumed.set(next);
                }
            }
        }

    protected:
        virtual Response consume(Command&& command) = 0;

    public:
        SingleThreadConsumer() = default;

        virtual ~SingleThreadConsumer()
        {
            stop();
        }

        SingleThreadConsumer(const SingleThreadConsumer&) = delete;
        SingleThreadConsumer& operator=(const SingleThreadConsumer&) = delete;

        void start(int ringSize = 32)
        {
            std::lock_guard lock(this->lifecycleMutex);
            if (this->lifecycle.load(std::memory_order_relaxed) != Lifecycle::Created)
            {
                throw std::logic_error("consumer can only be started once");
            }

            this->ring = Ring::createMultiProducer(std::make_shared<EventFactory>(), ringSize, this->waitStrategy);
            this->ring->addGatingSequences(this->consumed);
            this->barrier = this->ring->newBarrier();
            this->worker = std::thread(&SingleThreadConsumer::run, this);
            this->workerId = this->worker.get_id();
            this->lifecycle.store(Lifecycle::Running, std::memory_order_release);
        }

        RequestTicket<Response> post(Command command, ResponseSlot<Response>& responseSlot)
        {
            static_assert(std::is_nothrow_move_constructible_v<Command>, "fast commands must be nothrow move constructible");

            this->beginSubmit();
            try
            {
                responseSlot.prepare();

                const auto sequence = this->ring->next();
                Event& event = this->ring->get(sequence);
                event.request.reset();
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

            const auto sequence = this->ring->next();
            Event& event = this->ring->get(sequence);
            event.command.reset();
            event.request.reset();
            event.responseSlot = nullptr;
            event.type = EventType::Stop;
            this->ring->publish(sequence);

            this->worker.join();
            this->lifecycle.store(Lifecycle::Stopped, std::memory_order_release);
        }
    };
}

#endif // ADVMATCH_SINGLE_THREAD_CONSUMER_H
