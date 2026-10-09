#ifndef ADVMATCH_SDK_COMMAND_H
#define ADVMATCH_SDK_COMMAND_H

#include "../ExchangeApi.h"

namespace advmatch::sdk::command
{
    template <typename Response>
    class Command
    {
    public:
        using ResponseType = Response;

        virtual ~Command() = default;
        virtual CompletableFuture<Response> execute(ExchangeApi& api) const = 0;
    };
}

#endif // ADVMATCH_SDK_COMMAND_H
