#ifndef ADVMATCH_COMPLETABLE_FUTURE_H
#define ADVMATCH_COMPLETABLE_FUTURE_H

#include "detail/FutureState.h"

#include <span>
#include <utility>

namespace advmatch::api::async
{
    template <typename T>
    class CompletableFuture;

    // Producer capability. Consumers normally receive only CompletableFuture.
    template <typename T>
    class CompletionSource
    {
        template <typename>
        friend class CompletableFuture;
        std::shared_ptr<detail::FutureState<T>> state;

    public:
        explicit CompletionSource(std::shared_ptr<Executor> executor = nullptr)
            : state(std::make_shared<detail::FutureState<T>>(std::move(executor)))
        {
        }

        CompletableFuture<T> future() const noexcept;

        bool tryStart() const noexcept
        {
            return this->state->tryStart();
        }

        bool completeShared(std::shared_ptr<const FutureValue<T>> value) const noexcept
        {
            if (value == nullptr)
            {
                return false;
            }

            const auto state = this->state;
            return state->finish(detail::CompletionState::Success, {std::move(value), nullptr, false});
        }

        template <typename U = T>
        bool complete(U value) const requires (!std::is_void_v<U>)
        {
            return this->completeShared(std::make_shared<const FutureValue<T>>(std::move(value)));
        }

        bool complete() const requires std::is_void_v<T>
        {
            static const auto unit = std::make_shared<const std::monostate>();
            return this->completeShared(unit);
        }

        bool fail(std::exception_ptr error) const noexcept
        {
            if (error == nullptr)
            {
                return false;
            }

            const auto state = this->state;
            return state->finish(detail::CompletionState::Failed, {nullptr, std::move(error), false});
        }
    };

    template <typename T>
    class CompletableFuture
    {
        template <typename>
        friend class CompletionSource;
        template <typename>
        friend class CompletableFuture;
        std::shared_ptr<detail::FutureState<T>> state;

        explicit CompletableFuture(std::shared_ptr<detail::FutureState<T>> state) noexcept
            : state(std::move(state))
        {
        }

        template <typename U, typename Function>
        CompletableFuture<U> pipe(Function function, std::shared_ptr<Executor> executor) const
        {
            auto selected = executor != nullptr ? std::move(executor) : this->state->executor;
            CompletionSource<U> next(selected);
            try
            {
                auto action = std::make_shared<Function>(std::move(function));
                this->state->onComplete([next, selected, action](const FutureOutcome<T>& outcome) noexcept
                {
                    try
                    {
                        selected->execute([next, action, outcome]
                        {
                            if (!next.tryStart())
                            {
                                return;
                            }

                            try
                            {
                                std::invoke(*action, outcome, next);
                            }
                            catch (...)
                            {
                                next.fail(std::current_exception());
                            }
                        });
                    }
                    catch (...)
                    {
                        next.fail(std::current_exception());
                    }
                });
            }
            catch (...)
            {
                next.fail(std::current_exception());
            }

            return next.future();
        }

        void forwardTo(CompletionSource<T> destination) const
        {
            this->state->onComplete([destination](const FutureOutcome<T>& outcome) noexcept
            {
                if (outcome.error != nullptr)
                {
                    destination.fail(outcome.error);
                }
                else
                {
                    destination.completeShared(outcome.value);
                }
            });
        }

        static void checkError(const FutureOutcome<T>& outcome)
        {
            if (outcome.error != nullptr)
            {
                std::rethrow_exception(outcome.error);
            }
        }

    public:
        using ValueType = T;

        CompletableFuture()
            : state(std::make_shared<detail::FutureState<T>>(nullptr))
        {
        }

        CompletableFuture(const CompletableFuture&) = default;
        CompletableFuture& operator=(const CompletableFuture&) = default;

        // A future is a shareable completion handle. Moving keeps both handles
        // usable, matching reference-like semantics rather than unique future.
        CompletableFuture(CompletableFuture&& other) noexcept
            : state(other.state)
        {
        }

        CompletableFuture& operator=(CompletableFuture&& other) noexcept
        {
            this->state = other.state;
            return *this;
        }

        bool isDone() const noexcept
        {
            return this->state->isDone();
        }

        bool isCancelled() const noexcept
        {
            return this->state->isCancelled();
        }

        // Cancellation only wins before command/callback execution starts.
        // It never interrupts matching, rolls back assets or undoes a WAL commit.
        bool cancel() const
        {
            return this->state->finish(detail::CompletionState::Cancelled, {nullptr, std::make_exception_ptr(FutureCancelled()), true});
        }

        T get() const
        {
            const auto outcome = this->state->await();
            checkError(outcome);
            if constexpr (!std::is_void_v<T>)
            {
                return *outcome.value;
            }
        }

        T join() const
        {
            return this->get();
        }

        std::shared_ptr<const FutureValue<T>> getShared() const
        {
            const auto outcome = this->state->await();
            checkError(outcome);
            return outcome.value;
        }

        template <typename Rep, typename Period>
        T getFor(std::chrono::duration<Rep, Period> timeout) const
        {
            const auto outcome = this->state->awaitFor(timeout);
            checkError(outcome);
            if constexpr (!std::is_void_v<T>)
            {
                return *outcome.value;
            }
        }

        template <typename Function>
        auto thenApply(Function function, std::shared_ptr<Executor> executor = nullptr) const
        {
            using U = std::remove_cvref_t<decltype(detail::invokeValue<T>(function, std::declval<const FutureOutcome<T>&>()))>;
            return this->template pipe<U>([function = std::move(function)](const FutureOutcome<T>& outcome, const CompletionSource<U>& next) mutable
            {
                if (outcome.error != nullptr)
                {
                    next.fail(outcome.error);
                    return;
                }

                if constexpr (std::is_void_v<U>)
                {
                    detail::invokeValue<T>(function, outcome);
                    next.complete();
                }
                else
                {
                    next.complete(detail::invokeValue<T>(function, outcome));
                }
            }, std::move(executor));
        }

        template <typename Function>
        CompletableFuture<void> thenAccept(Function function, std::shared_ptr<Executor> executor = nullptr) const
        {
            return this->thenApply([function = std::move(function)](auto&&... arguments) mutable
            {
                std::invoke(function, std::forward<decltype(arguments)>(arguments)...);
            }, std::move(executor));
        }

        template <typename Function>
        CompletableFuture<void> thenRun(Function function, std::shared_ptr<Executor> executor = nullptr) const
        {
            return this->thenAccept([function = std::move(function)](auto&&...) mutable
            {
                std::invoke(function);
            }, std::move(executor));
        }

        template <typename Function>
        auto thenCompose(Function function, std::shared_ptr<Executor> executor = nullptr) const
        {
            using Inner = std::remove_cvref_t<decltype(detail::invokeValue<T>(function, std::declval<const FutureOutcome<T>&>()))>;
            using U = typename Inner::ValueType;
            return this->template pipe<U>([function = std::move(function)](const FutureOutcome<T>& outcome, const CompletionSource<U>& next) mutable
            {
                if (outcome.error != nullptr)
                {
                    next.fail(outcome.error);
                    return;
                }

                auto inner = detail::invokeValue<T>(function, outcome);
                inner.forwardTo(next);
            }, std::move(executor));
        }

        template <typename Function>
        CompletableFuture<T> exceptionally(Function function, std::shared_ptr<Executor> executor = nullptr) const
        {
            return this->template pipe<T>([function = std::move(function)](const FutureOutcome<T>& outcome, const CompletionSource<T>& next) mutable
            {
                if (outcome.error == nullptr)
                {
                    next.completeShared(outcome.value);
                }
                else if constexpr (std::is_void_v<T>)
                {
                    std::invoke(function, outcome.error);
                    next.complete();
                }
                else
                {
                    next.complete(std::invoke(function, outcome.error));
                }
            }, std::move(executor));
        }

        template <typename Function>
        auto handle(Function function, std::shared_ptr<Executor> executor = nullptr) const
        {
            using U = std::remove_cvref_t<std::invoke_result_t<Function&, const FutureOutcome<T>&>>;
            return this->template pipe<U>([function = std::move(function)](const FutureOutcome<T>& outcome, const CompletionSource<U>& next) mutable
            {
                if constexpr (std::is_void_v<U>)
                {
                    std::invoke(function, outcome);
                    next.complete();
                }
                else
                {
                    next.complete(std::invoke(function, outcome));
                }
            }, std::move(executor));
        }

        template <typename Function>
        CompletableFuture<T> whenComplete(Function function, std::shared_ptr<Executor> executor = nullptr) const
        {
            return this->template pipe<T>([function = std::move(function)](const FutureOutcome<T>& outcome, const CompletionSource<T>& next) mutable
            {
                try
                {
                    std::invoke(function, outcome);
                }
                catch (...)
                {
                    next.fail(outcome.error != nullptr ? outcome.error : std::current_exception());
                    return;
                }

                if (outcome.error != nullptr)
                {
                    next.fail(outcome.error);
                }
                else
                {
                    next.completeShared(outcome.value);
                }
            }, std::move(executor));
        }

        template <typename U, typename Function>
        auto thenCombine(CompletableFuture<U> other, Function function) const requires (!std::is_void_v<T> && !std::is_void_v<U>)
        {
            using Combined = std::remove_cvref_t<std::invoke_result_t<Function&, const T&, const U&>>;
            return this->template pipe<Combined>([other, function = std::move(function)](const FutureOutcome<T>& outcome, const CompletionSource<Combined>& next) mutable
            {
                if (outcome.error != nullptr)
                {
                    next.fail(outcome.error);
                    return;
                }

                auto combined = other.thenApply([saved = outcome.value, function = std::move(function)](const U& second) mutable
                {
                    return std::invoke(function, *saved, second);
                });
                combined.forwardTo(next);
            }, nullptr);
        }

        static CompletableFuture<T> completedFuture(FutureValue<T> value) requires (!std::is_void_v<T>)
        {
            CompletionSource<T> source;
            source.complete(std::move(value));
            return source.future();
        }

        static CompletableFuture<void> completedFuture() requires std::is_void_v<T>
        {
            CompletionSource<void> source;
            source.complete();
            return source.future();
        }

        static CompletableFuture<T> failedFuture(std::exception_ptr error)
        {
            if (error == nullptr)
            {
                error = std::make_exception_ptr(std::invalid_argument("failed future needs an exception"));
            }

            CompletionSource<T> source;
            source.fail(error);
            return source.future();
        }

        // Homogeneous inputs; allOf completes after all inputs settle, retaining
        // the first failure. Empty allOf succeeds; empty anyOf is invalid.
        static CompletableFuture<void> allOf(std::span<const CompletableFuture<T>> inputs)
        {
            struct Group
            {
                std::mutex mutex;
                std::size_t remaining;
                std::exception_ptr error;
                CompletionSource<void> source;

                explicit Group(std::size_t size)
                    : remaining(size)
                {
                }
            };

            auto group = std::make_shared<Group>(inputs.size());
            if (inputs.empty())
            {
                group->source.complete();
            }

            try
            {
                for (const auto& input : inputs)
                {
                    input.state->onComplete([group](const FutureOutcome<T>& outcome) noexcept
                    {
                        bool finished = false;
                        std::exception_ptr error;
                        {
                            std::lock_guard lock(group->mutex);
                            if (outcome.error != nullptr && group->error == nullptr)
                            {
                                group->error = outcome.error;
                            }

                            finished = --group->remaining == 0;
                            error = group->error;
                        }

                        if (finished)
                        {
                            if (error != nullptr)
                            {
                                group->source.fail(error);
                            }
                            else
                            {
                                try
                                {
                                    group->source.complete();
                                }
                                catch (...)
                                {
                                    group->source.fail(std::current_exception());
                                }
                            }
                        }
                    });
                }
            }
            catch (...)
            {
                group->source.fail(std::current_exception());
            }

            return group->source.future();
        }

        static CompletableFuture<T> anyOf(std::span<const CompletableFuture<T>> inputs)
        {
            CompletionSource<T> source;
            if (inputs.empty())
            {
                source.fail(std::make_exception_ptr(std::invalid_argument("anyOf needs at least one future")));
            }

            try
            {
                for (const auto& input : inputs)
                {
                    input.forwardTo(source);
                }
            }
            catch (...)
            {
                source.fail(std::current_exception());
            }

            return source.future();
        }
    };

    template <typename T>
    CompletableFuture<T> CompletionSource<T>::future() const noexcept
    {
        return CompletableFuture<T>(this->state);
    }
}

#endif // ADVMATCH_COMPLETABLE_FUTURE_H
