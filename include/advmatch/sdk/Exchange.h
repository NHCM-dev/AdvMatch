#ifndef ADVMATCH_SDK_EXCHANGE_H
#define ADVMATCH_SDK_EXCHANGE_H

#include "ExchangeApi.h"

namespace advmatch::sdk
{
    // The facade closes admission and drains on destruction. Entity handles
    // retain the closed API object, so use after scope exit fails safely.
    class Exchange
    {
    protected:
        std::shared_ptr<ExchangeApi> session;

    public:
        explicit Exchange(ExchangeConfig config = {})
            : session(makeExchangeApi(std::move(config)))
        {
        }

        explicit Exchange(std::shared_ptr<ExchangeApi> api)
            : session(std::move(api))
        {
            if (this->session == nullptr)
            {
                throw std::invalid_argument("exchange API is null");
            }
        }

        virtual ~Exchange()
        {
            try
            {
                this->session->close().get();
            }
            catch (...)
            {
                // Explicit closeAsync().get() lets the caller observe errors.
            }
        }

        Exchange(const Exchange&) = delete;
        Exchange& operator=(const Exchange&) = delete;

        std::shared_ptr<ExchangeApi> api() const noexcept
        {
            return this->session;
        }

        template <typename Concrete>
        CompletableFuture<typename Concrete::ResponseType> submit(const Concrete& command) const
        {
            return this->session->submit(command);
        }

        template <typename Concrete>
        CompletableFuture<typename Concrete::ResponseType> submit(const std::shared_ptr<Concrete>& command) const
        {
            return this->session->submit(command);
        }

        CompletableFuture<CommandReceipt> submit(TradingCommand operation, std::uint64_t requestId = 0) const
        {
            return this->session->execute(std::move(operation), requestId);
        }

        CompletableFuture<void> closeAsync() const
        {
            return this->session->close();
        }

        void close() const
        {
            this->closeAsync().get();
        }
    };
}

#endif // ADVMATCH_SDK_EXCHANGE_H
