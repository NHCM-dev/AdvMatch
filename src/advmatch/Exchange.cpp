//
// Created by NHCM on 2026/10/7.
//

#include "Exchange.h"
#include "consuming/SingleThreadConsumer.h"
#include "observe/impl/ObserverService.h"
#include "observe/impl/ObservationEventSink.h"
#include "persistence/impl/EnginePersistenceService.h"

#include <mutex>
#include <thread>

namespace advmatch
{
    struct Exchange::Impl final : api::command::CommandContext, api::TradingService
    {
        struct Runtime
        {
            PersistentTradingEngine engine;
            observe::TradingObserver observer;
            observe::ObservationEventSink events;
            observe::ObserverService observation;
            persistence::EnginePersistenceService persistence;
            bool initialized = false;

            Runtime(const ExchangeConfig& config, std::unique_ptr<PersistenceStore> store)
                : engine(config.persistence, std::move(store)), observer(this->engine, config.observation), events(this->observer),
                  observation(this->observer, config.observationCacheSlots), persistence(this->engine, config.persistence.enabled, config.maxHistoryRows)
            {
            }

            void initialize()
            {
                if (!this->initialized)
                {
                    this->engine.setEventSink(&this->events);
                    this->initialized = true;
                }
            }
        };

        class Worker final : public consuming::SingleThreadConsumer<std::unique_ptr<api::detail::CommandTask>, void>
        {
            Impl& owner;

            void consume(std::unique_ptr<api::detail::CommandTask>&& task) override
            {
                try
                {
                    this->owner.runtime->initialize();
                    task->run(this->owner);
                }
                catch (...)
                {
                    task->reject(std::current_exception());
                }
            }

            void onStopped() noexcept override
            {
                try
                {
                    this->owner.runtime->initialize();
                    this->owner.runtime->engine.setEventSink(nullptr);
                    this->owner.runtime->engine.close();
                }
                catch (...)
                {
                    this->owner.closeError = std::current_exception();
                }

                // Includes faulted stores: close always releases directory locks.
                // Services/observer die before the managers they reference.
                this->owner.runtime.reset();
            }

        public:
            explicit Worker(Impl& owner)
                : owner(owner)
            {
            }

            ~Worker() override
            {
                this->stopBeforeDerivedDestruction();
            }

            void stopSafely() noexcept
            {
                this->stopBeforeDerivedDestruction();
            }
        };

        enum class Lifecycle
        {
            Running,
            Closing,
            Closed
        };

        std::shared_ptr<api::async::Executor> executor;
        std::unique_ptr<Runtime> runtime;
        std::exception_ptr closeError;
        Worker worker{*this};
        std::mutex admission;
        Lifecycle lifecycle = Lifecycle::Running;
        api::async::CompletionSource<void> closeSource;
        std::thread shutdownThread;

        Impl(ExchangeConfig config, std::unique_ptr<PersistenceStore> store)
            : executor(config.callbackExecutor != nullptr ? config.callbackExecutor : api::async::defaultExecutor()), closeSource(this->executor)
        {
            if (config.ringBufferSize <= 0 || (config.ringBufferSize & (config.ringBufferSize - 1)) != 0 || config.progressBatchSize > static_cast<std::size_t>(config.ringBufferSize))
            {
                throw std::invalid_argument("exchange ring size must be a power of two; progress batch must fit");
            }

            this->runtime = std::make_unique<Runtime>(config, std::move(store));
            this->worker.start(config.ringBufferSize, config.progressBatchSize);
        }

        ~Impl() override
        {
            try
            {
                this->close().get();
            }
            catch (...)
            {
                // Explicit close exposes failures. Destruction still drains and
                // releases the session; it cannot report an I/O exception.
                this->worker.stopSafely();
            }

            if (this->shutdownThread.joinable())
            {
                this->shutdownThread.join();
            }
        }

        api::TradingService& trading() noexcept override
        {
            return *this;
        }

        observe::ObservationService& observation() noexcept override
        {
            return this->runtime->observation;
        }

        persistence::PersistenceService& persistence() noexcept override
        {
            return this->runtime->persistence;
        }

        CommandReceipt execute(const TradingCommand& command, std::uint64_t requestId) override
        {
            this->runtime->observer.notifyCommand(command);
            try
            {
                auto receipt = this->runtime->engine.execute(command, requestId);
                this->runtime->observer.endCommand();
                return receipt;
            }
            catch (...)
            {
                this->runtime->observer.endCommand();
                throw;
            }
        }

        void dispatch(std::unique_ptr<api::detail::CommandTask> task) noexcept
        {
            try
            {
                std::lock_guard lock(this->admission);
                if (this->lifecycle != Lifecycle::Running)
                {
                    throw api::ApiError(api::ApiFailure::Closed, "exchange is closing or closed");
                }

                if (!this->worker.tryPostDetached(task))
                {
                    throw api::ApiError(api::ApiFailure::QueueFull, "exchange command queue is full");
                }
            }
            catch (...)
            {
                task->reject(std::current_exception());
            }
        }

        api::async::CompletableFuture<void> close()
        {
            std::lock_guard lock(this->admission);
            if (this->lifecycle == Lifecycle::Running)
            {
                this->lifecycle = Lifecycle::Closing;
                this->closeSource.tryStart();
                try
                {
                    this->shutdownThread = std::thread([this]
                    {
                        try
                        {
                            this->worker.stop();
                            {
                                std::lock_guard admissionLock(this->admission);
                                this->lifecycle = Lifecycle::Closed;
                            }

                            if (this->closeError != nullptr)
                            {
                                this->closeSource.fail(this->closeError);
                            }
                            else
                            {
                                this->closeSource.complete();
                            }
                        }
                        catch (...)
                        {
                            this->closeSource.fail(std::current_exception());
                        }
                    });
                }
                catch (...)
                {
                    this->lifecycle = Lifecycle::Running;
                    auto failed = this->closeSource.future();
                    this->closeSource.fail(std::current_exception());
                    this->closeSource = api::async::CompletionSource<void>(this->executor);
                    return failed;
                }
            }

            return this->closeSource.future();
        }
    };

    Exchange::Exchange(ExchangeConfig config, std::unique_ptr<PersistenceStore> store)
        : impl(std::make_unique<Impl>(std::move(config), std::move(store)))
    {
    }

    Exchange::~Exchange() = default;

    std::shared_ptr<api::async::Executor> Exchange::completionExecutor() const noexcept
    {
        return this->impl->executor;
    }

    void Exchange::dispatch(std::unique_ptr<api::detail::CommandTask> task) noexcept
    {
        this->impl->dispatch(std::move(task));
    }

    api::async::CompletableFuture<void> Exchange::close()
    {
        return this->impl->close();
    }
} // advmatch
