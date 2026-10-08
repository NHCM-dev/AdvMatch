#include "ThreadPoolExecutor.h"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace advmatch::api::async
{
    struct ThreadPoolExecutor::Impl
    {
        struct Queue
        {
            std::mutex mutex;
            std::condition_variable changed;
            std::deque<std::function<void()>> work;
            bool closing = false;
        };

        std::shared_ptr<Queue> queue = std::make_shared<Queue>();
        std::mutex lifecycle;
        std::vector<std::thread> workers;

        explicit Impl(std::size_t threads)
        {
            if (threads == 0)
            {
                throw std::invalid_argument("callback executor needs at least one thread");
            }

            this->workers.reserve(threads);
            try
            {
                for (std::size_t index = 0; index < threads; ++index)
                {
                    this->workers.emplace_back([queue = this->queue]
                    {
                        for (;;)
                        {
                            std::function<void()> work;
                            {
                                std::unique_lock lock(queue->mutex);
                                queue->changed.wait(lock, [&]
                                {
                                    return queue->closing || !queue->work.empty();
                                });
                                if (queue->work.empty())
                                {
                                    return;
                                }

                                work = std::move(queue->work.front());
                                queue->work.pop_front();
                            }

                            // Future continuations capture their exceptions.
                            // Independent executor jobs cannot kill a worker.
                            try
                            {
                                work();
                            }
                            catch (...)
                            {
                            }
                        }
                    });
                }
            }
            catch (...)
            {
                this->shutdown();
                throw;
            }
        }

        void shutdown() noexcept
        {
            std::vector<std::thread> workers;
            bool fromWorker = false;
            {
                std::lock_guard lifecycleLock(this->lifecycle);
                {
                    std::lock_guard queueLock(this->queue->mutex);
                    this->queue->closing = true;
                }

                for (const auto& worker : this->workers)
                {
                    fromWorker = fromWorker || worker.get_id() == std::this_thread::get_id();
                }

                workers.swap(this->workers);
            }

            this->queue->changed.notify_all();
            for (auto& worker : workers)
            {
                if (fromWorker)
                {
                    // A future may release the last executor reference inside
                    // its callback. Workers retain Queue, not this wrapper.
                    worker.detach();
                }
                else
                {
                    worker.join();
                }
            }
        }
    };

    ThreadPoolExecutor::ThreadPoolExecutor(std::size_t threads)
        : impl(std::make_unique<Impl>(threads))
    {
    }

    ThreadPoolExecutor::~ThreadPoolExecutor()
    {
        this->shutdown();
    }

    void ThreadPoolExecutor::execute(std::function<void()> work)
    {
        if (!work)
        {
            throw std::invalid_argument("empty executor job");
        }

        {
            std::lock_guard lock(this->impl->queue->mutex);
            if (this->impl->queue->closing)
            {
                throw std::runtime_error("callback executor is shut down");
            }

            this->impl->queue->work.push_back(std::move(work));
        }

        this->impl->queue->changed.notify_one();
    }

    void ThreadPoolExecutor::shutdown() noexcept
    {
        this->impl->shutdown();
    }

    std::shared_ptr<Executor> defaultExecutor()
    {
        static const auto executor = std::make_shared<ThreadPoolExecutor>();
        return executor;
    }
}
