#ifndef ADVMATCH_FUTURE_STATE_H
#define ADVMATCH_FUTURE_STATE_H

#include "../Executor.h"
#include "../FutureError.h"

#include <chrono>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <type_traits>
#include <variant>
#include <vector>

namespace advmatch::api::async
{
    template <typename T>
    using FutureValue = std::conditional_t<std::is_void_v<T>, std::monostate, T>;

    template <typename T>
    struct FutureOutcome
    {
        std::shared_ptr<const FutureValue<T>> value;
        std::exception_ptr error;
        bool cancelled = false;
    };

    namespace detail
    {
        inline thread_local bool blockingForbidden = false;

        class NonBlockingScope
        {
            bool previous = blockingForbidden;

        public:
            NonBlockingScope()
            {
                blockingForbidden = true;
            }

            ~NonBlockingScope()
            {
                blockingForbidden = this->previous;
            }
        };

        enum class CompletionState
        {
            Pending,
            Running,
            Success,
            Failed,
            Cancelled
        };

        template <typename T>
        class FutureState
        {
            mutable std::mutex mutex;
            mutable std::condition_variable changed;
            CompletionState status = CompletionState::Pending;
            FutureOutcome<T> result;
            std::vector<std::function<void(const FutureOutcome<T>&)>> listeners;

            bool done() const noexcept
            {
                return this->status != CompletionState::Pending && this->status != CompletionState::Running;
            }

        public:
            const std::shared_ptr<Executor> executor;

            explicit FutureState(std::shared_ptr<Executor> executor)
                : executor(executor != nullptr ? std::move(executor) : defaultExecutor())
            {
            }

            bool isDone() const noexcept
            {
                std::lock_guard lock(this->mutex);
                return this->done();
            }

            bool isCancelled() const noexcept
            {
                std::lock_guard lock(this->mutex);
                return this->status == CompletionState::Cancelled;
            }

            bool tryStart() noexcept
            {
                std::lock_guard lock(this->mutex);
                if (this->status != CompletionState::Pending)
                {
                    return false;
                }

                this->status = CompletionState::Running;
                return true;
            }

            bool finish(CompletionState status, FutureOutcome<T> result) noexcept
            {
                std::vector<std::function<void(const FutureOutcome<T>&)>> listeners;
                {
                    std::lock_guard lock(this->mutex);
                    if (this->done() || (status == CompletionState::Cancelled && this->status != CompletionState::Pending))
                    {
                        return false;
                    }

                    this->result = std::move(result);
                    this->status = status;
                    listeners.swap(this->listeners);
                }

                this->changed.notify_all();
                for (const auto& listener : listeners)
                {
                    // Only internal schedulers/forwarders are registered here.
                    listener(this->result);
                }

                return true;
            }

            void onComplete(std::function<void(const FutureOutcome<T>&)> listener)
            {
                {
                    std::lock_guard lock(this->mutex);
                    if (!this->done())
                    {
                        this->listeners.push_back(std::move(listener));
                        return;
                    }
                }

                listener(this->result);
            }

            FutureOutcome<T> await() const
            {
                std::unique_lock lock(this->mutex);
                if (!this->done() && blockingForbidden)
                {
                    throw std::logic_error("cannot wait for an unfinished future on the exchange owner thread");
                }

                this->changed.wait(lock, [&]
                {
                    return this->done();
                });
                return this->result;
            }

            template <typename Rep, typename Period>
            FutureOutcome<T> awaitFor(std::chrono::duration<Rep, Period> timeout) const
            {
                std::unique_lock lock(this->mutex);
                if (!this->done() && blockingForbidden)
                {
                    throw std::logic_error("cannot wait for an unfinished future on the exchange owner thread");
                }

                if (!this->changed.wait_for(lock, timeout, [&]
                {
                    return this->done();
                }))
                {
                    throw FutureTimeout();
                }

                return this->result;
            }
        };

        template <typename T, typename Function>
        decltype(auto) invokeValue(Function& function, const FutureOutcome<T>& outcome)
        {
            if constexpr (std::is_void_v<T>)
            {
                return std::invoke(function);
            }
            else
            {
                return std::invoke(function, *outcome.value);
            }
        }
    }
}

#endif // ADVMATCH_FUTURE_STATE_H
