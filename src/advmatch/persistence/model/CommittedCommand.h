#ifndef ADVMATCH_COMMITTED_COMMAND_H
#define ADVMATCH_COMMITTED_COMMAND_H

#include "advmatch/api/command/model/TradingCommand.h"

namespace advmatch
{
    struct CommittedCommand
    {
        CommandSequence sequence;
        std::uint64_t requestId;
        CommandResult result;
        std::vector<Trade> trades;
        std::vector<ContractEvent> contractEvents;
    };
}

#endif // ADVMATCH_COMMITTED_COMMAND_H
