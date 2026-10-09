#ifndef ADVMATCH_ASYNC_EXECUTOR_H
#define ADVMATCH_ASYNC_EXECUTOR_H

#include "advmatch/sdk/Export.h"

#include <functional>
#include <memory>

namespace advmatch::api::async
{
    // Implementations must enqueue work, never execute user callbacks inline
    // on the trading owner thread. Rejected work must throw.
    class Executor
    {
    public:
        virtual ~Executor() = default;
        virtual void execute(std::function<void()> work) = 0;
    };

    ADVMATCH_SDK_API std::shared_ptr<Executor> defaultExecutor();
}

#endif // ADVMATCH_ASYNC_EXECUTOR_H
