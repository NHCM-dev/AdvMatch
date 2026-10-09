#ifndef ADVMATCH_SDK_ERRORS_H
#define ADVMATCH_SDK_ERRORS_H

#include "Models.h"
#include "advmatch/api/ApiError.h"

namespace advmatch::sdk
{
    using advmatch::api::ApiError;
    using advmatch::api::ApiFailure;

    class BusinessError : public std::runtime_error
    {
        CommandReceipt result;

    public:
        explicit BusinessError(CommandReceipt receipt)
            : std::runtime_error("exchange operation returned a non-success result"), result(std::move(receipt))
        {
        }

        const CommandReceipt& receipt() const noexcept
        {
            return this->result;
        }
    };

    class ReadError : public std::runtime_error
    {
        ReadPage result;

    public:
        explicit ReadError(ReadPage page)
            : std::runtime_error("exchange entity read failed"), result(std::move(page))
        {
        }

        const ReadPage& page() const noexcept
        {
            return this->result;
        }
    };
}

#endif // ADVMATCH_SDK_ERRORS_H
