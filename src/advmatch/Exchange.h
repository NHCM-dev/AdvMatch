//
// Created by NHCM on 2026/10/7.
//

#ifndef ADVMATCH_EXCHANGE_H
#define ADVMATCH_EXCHANGE_H

#include "api/ExchangeApi.h"
#include "config/ExchangeConfig.h"
#include "persistence/abstract/PersistenceStore.h"

namespace advmatch
{
    // Owns one trading session and consumer. Producers only receive API
    // futures/value snapshots; no mutable managers escape this boundary.
    class Exchange final : public api::ExchangeApi
    {
        struct Impl;
        std::unique_ptr<Impl> impl;

        std::shared_ptr<api::async::Executor> completionExecutor() const noexcept override;
        void dispatch(std::unique_ptr<api::detail::CommandTask> task) noexcept override;

    public:
        explicit Exchange(ExchangeConfig config = {}, std::unique_ptr<PersistenceStore> store = nullptr);
        ~Exchange() override;
        Exchange(const Exchange&) = delete;
        Exchange& operator=(const Exchange&) = delete;

        // Immediately closes admission, then drains accepted commands, flushes
        // and releases the session on its owner thread. Repeated close shares
        // the outcome. The destructor waits; asynchronous close does not.
        api::async::CompletableFuture<void> close() override;
    };
} // advmatch

#endif //ADVMATCH_EXCHANGE_H
