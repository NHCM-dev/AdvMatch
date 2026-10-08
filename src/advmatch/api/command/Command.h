#ifndef ADVMATCH_API_COMMAND_H
#define ADVMATCH_API_COMMAND_H

#include "CommandContext.h"

namespace advmatch::api::command
{
    template <typename Response>
    class Command
    {
    public:
        using ResponseType = Response;
        virtual ~Command() = default;
        virtual Response execute(CommandContext& context) const = 0;
    };
}

#endif // ADVMATCH_API_COMMAND_H
