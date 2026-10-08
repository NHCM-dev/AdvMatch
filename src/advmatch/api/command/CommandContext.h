#ifndef ADVMATCH_COMMAND_CONTEXT_H
#define ADVMATCH_COMMAND_CONTEXT_H

#include "../service/TradingService.h"
#include "advmatch/observe/abstract/ObservationService.h"
#include "advmatch/persistence/abstract/PersistenceService.h"

namespace advmatch::api::command
{
    // Capability boundary: custom commands can compose sanctioned operations,
    // but cannot borrow managers or mutate state outside journal execution.
    class CommandContext
    {
    public:
        virtual ~CommandContext() = default;
        virtual TradingService& trading() noexcept = 0;
        virtual observe::ObservationService& observation() noexcept = 0;
        virtual persistence::PersistenceService& persistence() noexcept = 0;
    };
}

#endif // ADVMATCH_COMMAND_CONTEXT_H
