#ifndef ADVMATCH_SINGLE_THREAD_CONSUMER_H
#define ADVMATCH_SINGLE_THREAD_CONSUMER_H

#include <disruptor/BusySpinWaitStrategy.h>
#include <disruptor/RingBuffer.h>

#include <cstdint>
#include <exception>
#include <future>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>

namespace advmatch::consuming
{
    // Derive from this class and implement consume(). All calls to consume()
    // run on one worker thread, in ring sequence order. Call stop() from the
    // derived destructor before derived state is destroyed.
    template <typename Command, typename Response>
    class SingleThreadConsumer
    {
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
            std::shared_ptr<Request> request;
            bool stop = false;
        };

        struct EventFactory : disruptor::EventFactory<Event>
        {
            Event newInstance() override
            {
                return {};
            }
        };

        using Ring = disruptor::MultiProducerRingBuffer<Event, disruptor::BusySpinWaitStrategy>;
        using Barrier = decltype(std::declval<Ring&>().newBarrier());

        disruptor::BusySpinWaitStrategy waitStrategy;
        std::shared_ptr<Ring> ring;
        Barrier barrier;
        disruptor::Sequence consumed;
        std::thread worker;
        std::thread::id workerId;
        mutable std::shared_mutex submitMutex;
        bool started = false;
        bool stopping = false;
        bool stopped = false;

        void run() noexcept
        {
            std::int64_t next = this->consumed.get() + 1;

            for (;;)
            {
                const auto available = this->barrier->waitFor(next);
                for (; next <= available; ++next)
                {
                    Event& event = this->ring->get(next);
                    if (event.stop)
                    {
                        this->consumed.set(next);
                        return;
                    }

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

        void start(int ringSize = 1024)
        {
            std::unique_lock lock(this->submitMutex);
            if (this->started || this->stopped)
            {
                throw std::logic_error("consumer can only be started once");
            }

            this->ring = Ring::createMultiProducer(std::make_shared<EventFactory>(), ringSize, this->waitStrategy);
            this->ring->addGatingSequences(this->consumed);
            this->barrier = this->ring->newBarrier();
            this->worker = std::thread(&SingleThreadConsumer::run, this);
            this->workerId = this->worker.get_id();
            this->started = true;
        }

        std::future<Response> post(Command command)
        {
            auto request = std::make_shared<Request>(std::move(command));
            std::future<Response> response = request->promise.get_future();

            std::shared_lock lock(this->submitMutex);
            if (!this->started || this->stopping)
            {
                throw std::logic_error("consumer is not accepting commands");
            }

            if (this->workerId == std::this_thread::get_id())
            {
                throw std::logic_error("consumer cannot post from its worker thread");
            }

            const auto sequence = this->ring->next();
            Event& event = this->ring->get(sequence);
            event.request = std::move(request);
            event.stop = false;
            this->ring->publish(sequence);

            return response;
        }

        void stop()
        {
            std::unique_lock lock(this->submitMutex);
            if (this->workerId == std::this_thread::get_id())
            {
                throw std::logic_error("consumer cannot stop itself");
            }

            if (!this->started || this->stopping)
            {
                return;
            }

            this->stopping = true;
            const auto sequence = this->ring->next();
            Event& event = this->ring->get(sequence);
            event.request.reset();
            event.stop = true;
            this->ring->publish(sequence);

            lock.unlock();
            this->worker.join();
            lock.lock();

            this->started = false;
            this->stopped = true;
        }
    };
}

#endif // ADVMATCH_SINGLE_THREAD_CONSUMER_H
