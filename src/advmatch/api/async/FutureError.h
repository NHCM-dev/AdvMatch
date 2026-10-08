#ifndef ADVMATCH_FUTURE_ERROR_H
#define ADVMATCH_FUTURE_ERROR_H

#include <stdexcept>

namespace advmatch::api::async
{
    class FutureCancelled : public std::runtime_error
    {
    public:
        FutureCancelled()
            : std::runtime_error("future cancelled before execution")
        {
        }
    };

    class FutureTimeout : public std::runtime_error
    {
    public:
        FutureTimeout()
            : std::runtime_error("future wait timed out")
        {
        }
    };
}

#endif // ADVMATCH_FUTURE_ERROR_H
