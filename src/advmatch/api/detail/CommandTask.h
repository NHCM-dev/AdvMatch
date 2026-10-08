#ifndef ADVMATCH_COMMAND_TASK_H
#define ADVMATCH_COMMAND_TASK_H

#include "../command/Command.h"
#include "../async/CompletionSource.h"

namespace advmatch::api::detail
{
    class CommandTask
    {
    public:
        virtual ~CommandTask() = default;
        virtual void run(command::CommandContext& context) noexcept = 0;
        virtual void reject(std::exception_ptr error) noexcept = 0;
    };

    template <typename Response>
    class TypedCommandTask final : public CommandTask
    {
        std::shared_ptr<const command::Command<Response>> command;
        async::CompletionSource<Response> source;

    public:
        TypedCommandTask(std::shared_ptr<const command::Command<Response>> command, async::CompletionSource<Response> source)
            : command(std::move(command)), source(std::move(source))
        {
        }

        void run(command::CommandContext& context) noexcept override
        {
            if (!this->source.tryStart())
            {
                return;
            }

            try
            {
                async::detail::NonBlockingScope scope;
                if constexpr (std::is_void_v<Response>)
                {
                    this->command->execute(context);
                    this->source.complete();
                }
                else
                {
                    this->source.complete(this->command->execute(context));
                }
            }
            catch (...)
            {
                this->source.fail(std::current_exception());
            }
        }

        void reject(std::exception_ptr error) noexcept override
        {
            this->source.fail(std::move(error));
        }
    };
}

#endif // ADVMATCH_COMMAND_TASK_H
