#ifndef ADVMATCH_EXCHANGE_API_H
#define ADVMATCH_EXCHANGE_API_H

#include "ApiError.h"
#include "detail/CommandTask.h"

#include <concepts>

namespace advmatch::api
{
    class ExchangeApi
    {
    protected:
        virtual std::shared_ptr<async::Executor> completionExecutor() const noexcept = 0;
        virtual void dispatch(std::unique_ptr<detail::CommandTask> task) noexcept = 0;

    public:
        virtual ~ExchangeApi() = default;
        virtual async::CompletableFuture<void> close() = 0;

        // Transport adapters own their response storage and cancellation state.
        // Tasks still use the same admission, owner thread and shutdown drain.
        void submitTask(std::unique_ptr<detail::CommandTask> task) noexcept
        {
            if (task)
            {
                this->dispatch(std::move(task));
            }
        }

        template <typename Response>
        async::CompletableFuture<Response> submit(std::shared_ptr<const command::Command<Response>> command)
        {
            async::CompletionSource<Response> source(this->completionExecutor());
            try
            {
                if (command == nullptr)
                {
                    throw ApiError(ApiFailure::InvalidCommand, "command is null");
                }

                this->dispatch(std::make_unique<detail::TypedCommandTask<Response>>(std::move(command), source));
            }
            catch (...)
            {
                source.fail(std::current_exception());
            }

            return source.future();
        }

        template <typename Concrete>
        requires std::derived_from<Concrete, command::Command<typename Concrete::ResponseType>>
        async::CompletableFuture<typename Concrete::ResponseType> submit(Concrete command)
        {
            using Response = typename Concrete::ResponseType;
            async::CompletionSource<Response> source(this->completionExecutor());
            try
            {
                // A value command lives inside its queue task: no extra
                // shared allocation or command reference counting is needed.
                this->dispatch(std::make_unique<detail::TypedCommandTask<Response, Concrete>>(std::move(command), source));
            }
            catch (...)
            {
                source.fail(std::current_exception());
            }

            return source.future();
        }

        template <typename Concrete>
        requires std::derived_from<std::remove_cv_t<Concrete>, command::Command<typename Concrete::ResponseType>>
        async::CompletableFuture<typename Concrete::ResponseType> submit(std::shared_ptr<Concrete> command)
        {
            using Response = typename Concrete::ResponseType;
            std::shared_ptr<const command::Command<Response>> abstract = std::move(command);
            return this->submit<Response>(std::move(abstract));
        }
    };
}

#endif // ADVMATCH_EXCHANGE_API_H
