#ifndef ADVMATCH_API_ERROR_H
#define ADVMATCH_API_ERROR_H

#include <stdexcept>

namespace advmatch::api
{
    enum class ApiFailure
    {
        Closed,
        QueueFull,
        InvalidCommand
    };

    class ApiError : public std::runtime_error
    {
        ApiFailure failure;

    public:
        ApiError(ApiFailure failure, const char* message)
            : std::runtime_error(message), failure(failure)
        {
        }

        ApiFailure reason() const noexcept
        {
            return this->failure;
        }
    };
}

#endif // ADVMATCH_API_ERROR_H
