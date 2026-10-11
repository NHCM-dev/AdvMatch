#include "advmatch/advmatch.h"
#include "advmatch/Exchange.h"
#include "binding/WireModels.h"
#include "advmatch/persistence/codec/CommandCodec.h"

#include <atomic>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <condition_variable>
#include <mutex>
#include <unordered_map>

namespace advmatch::binding
{
    namespace
    {
        thread_local std::array<char, 2048> lastError{};
        std::atomic<std::uint64_t> nextHandle{1};

        std::uint64_t allocateHandle()
        {
            auto value = nextHandle.load(std::memory_order_relaxed);
            for (;;)
            {
                if (value == std::numeric_limits<std::uint64_t>::max())
                {
                    throw std::overflow_error("binding handle space exhausted");
                }

                if (nextHandle.compare_exchange_weak(value, value + 1, std::memory_order_relaxed))
                {
                    return value;
                }
            }
        }

        struct Slots
        {
            std::mutex mutex;
            std::unordered_map<std::uint64_t, std::shared_ptr<observe::ObservationSlot>> values;

            std::uint64_t add(std::shared_ptr<observe::ObservationSlot> slot)
            {
                const auto id = allocateHandle();
                std::lock_guard lock(this->mutex);
                this->values.emplace(id, std::move(slot));
                return id;
            }

            std::shared_ptr<observe::ObservationSlot> get(std::uint64_t id)
            {
                std::lock_guard lock(this->mutex);
                const auto it = this->values.find(id);
                if (it == this->values.end())
                {
                    throw std::invalid_argument("invalid observation slot handle");
                }

                return it->second;
            }
        };

        class BindingCommand final : public api::command::Command<Response>
        {
            std::uint8_t operation;
            std::optional<CommandEnvelope> trading;
            observe::ObservationQuery query;
            std::vector<observe::ObservationQuery> queries;
            std::vector<observe::ReadStamp> known;
            persistence::CommittedQuery history;
            std::uint64_t slotId = 0;
            std::shared_ptr<Slots> slots;

        public:
            BindingCommand(std::span<const std::byte> bytes, std::shared_ptr<Slots> slots, std::size_t maxBatchQueries)
                : slots(std::move(slots))
            {
                BinaryReader reader(bytes);
                persistenceCheck(reader.number<std::uint16_t>() == 1, "unsupported binding packet version");
                this->operation = reader.number<std::uint8_t>();
                switch (this->operation)
                {
                    case 0:
                        this->trading = decodeCommand(reader.bytes(reader.remaining()));
                        break;
                    case 1:
                    case 6:
                        this->query = readObservationQuery(reader);
                        break;
                    case 2:
                    case 11:
                    {
                        const auto count = reader.count();
                        persistenceCheck(count <= maxBatchQueries, "too many batch queries");
                        this->queries.reserve(count);
                        for (std::size_t index = 0; index < count; ++index)
                        {
                            this->queries.push_back(readObservationQuery(reader));
                            if (this->operation == 11)
                            {
                                this->known.push_back(readReadStamp(reader));
                            }
                        }
                        break;
                    }
                    case 3:
                    case 4:
                    case 5:
                    case 8:
                    case 9:
                        break;
                    case 7:
                        this->slotId = reader.number<std::uint64_t>();
                        break;
                    case 10:
                        this->history = readCommittedQuery(reader);
                        break;
                    default:
                        throw std::invalid_argument("unknown binding operation");
                }

                reader.finish();
            }

            Response execute(api::command::CommandContext& context) const override
            {
                switch (this->operation)
                {
                    case 0:
                        return context.trading().execute(this->trading->command, this->trading->requestId);
                    case 1:
                        return context.observation().read(this->query);
                    case 2:
                        return context.observation().readBatch(this->queries);
                    case 11:
                        return context.observation().readChanged(this->queries, this->known);
                    case 3:
                        return context.persistence().flush();
                    case 4:
                        return context.persistence().tick();
                    case 5:
                        return context.persistence().checkpoint();
                    case 6:
                        return this->slots->add(context.observation().createSlot(this->query));
                    case 7:
                        return *context.observation().refresh(this->slots->get(this->slotId));
                    case 8:
                        context.observation().rebuildOpenOrderIndex();
                        return std::monostate{};
                    case 9:
                        return context.persistence().status();
                    case 10:
                        return context.persistence().readCommitted(this->history);
                    default:
                        throw std::logic_error("invalid decoded binding command");
                }
            }
        };

        void writeObservation(BinaryWriter& writer, const observe::ObservationResult& result)
        {
            write(writer, result.query);
            write(writer, result.page);
            writer.number<std::uint8_t>(static_cast<std::uint8_t>(result.data.index()));
            std::visit([&](const auto& data)
            {
                using Data = std::decay_t<decltype(data)>;
                if constexpr (std::is_same_v<Data, observe::OverviewView>)
                {
                    write(writer, data);
                }
                else if constexpr (!std::is_same_v<Data, std::monostate>)
                {
                    writer.number<std::uint64_t>(data.size());
                    for (const auto& row : data)
                    {
                        write(writer, row);
                    }
                }
            }, result.data);
        }

        void writeResponse(BinaryWriter& writer, const Response& response)
        {
            writer.number<std::uint16_t>(1);
            writer.number<std::uint8_t>(static_cast<std::uint8_t>(response.index()));
            std::visit([&](const auto& value)
            {
                using Value = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<Value, observe::ObservationResult>)
                {
                    writeObservation(writer, value);
                }
                else if constexpr (std::is_same_v<Value, observe::ObservationBatch>)
                {
                    writer.enumeration(value.state);
                    write(writer, value.stamp);
                    writer.number<std::uint64_t>(value.results.size());
                    for (const auto& result : value.results)
                    {
                        writeObservation(writer, result);
                    }
                }
                else if constexpr (std::is_same_v<Value, std::uint64_t>)
                {
                    writer.number(value);
                }
                else if constexpr (!std::is_same_v<Value, std::monostate>)
                {
                    write(writer, value);
                }
            }, response);
        }

        std::vector<std::byte> encodeResponse(const Response& response)
        {
            BinaryWriter writer;
            writeResponse(writer, response);
            return writer.take();
        }

        struct Request;

        struct Completions
        {
            std::mutex mutex;
            std::condition_variable changed;
            Request* head = nullptr;
            Request* tail = nullptr;

            void append(Request& request) noexcept;
            void remove(Request& request) noexcept;
        };

        enum class RequestState : std::uint8_t
        {
            Pending,
            Running,
            Success,
            Failed,
            Cancelled
        };

        struct Request
        {
            const std::uint64_t id;
            std::shared_ptr<Completions> completions;
            std::atomic<RequestState> state{RequestState::Pending};
            std::optional<Response> value;
            std::exception_ptr error;
            std::optional<api::async::CompletableFuture<void>> closing;
            Request* previous = nullptr;
            Request* next = nullptr;
            bool linked = false;
            bool retained = true;

            Request(std::uint64_t id, std::shared_ptr<Completions> completions)
                : id(id), completions(std::move(completions))
            {
            }

            Request(std::uint64_t id, api::async::CompletableFuture<void> future)
                : id(id), closing(std::move(future))
            {
            }

            bool isDone() const noexcept
            {
                const auto status = this->state.load(std::memory_order_acquire);
                return this->closing ? this->closing->isDone() : status != RequestState::Pending && status != RequestState::Running;
            }

            bool tryStart() noexcept
            {
                auto expected = RequestState::Pending;
                return this->state.compare_exchange_strong(expected, RequestState::Running, std::memory_order_acq_rel);
            }

            bool cancel()
            {
                if (this->closing)
                {
                    return false;
                }

                std::lock_guard lock(this->completions->mutex);
                auto expected = RequestState::Pending;
                if (!this->state.compare_exchange_strong(expected, RequestState::Cancelled, std::memory_order_acq_rel))
                {
                    return false;
                }

                if (this->retained)
                {
                    this->completions->append(*this);
                    this->completions->changed.notify_one();
                }

                return true;
            }

            void finish(RequestState status, bool deferred = false) noexcept
            {
                if (deferred)
                {
                    this->state.store(status, std::memory_order_release);
                    return;
                }

                std::lock_guard lock(this->completions->mutex);
                this->state.store(status, std::memory_order_release);
                if (this->retained)
                {
                    this->completions->append(*this);
                    this->completions->changed.notify_one();
                }
            }

            void reject(std::exception_ptr error) noexcept
            {
                if (this->tryStart())
                {
                    this->error = std::move(error);
                    this->finish(RequestState::Failed);
                }
            }

            void checkResult() const
            {
                switch (this->state.load(std::memory_order_acquire))
                {
                    case RequestState::Success:
                        return;
                    case RequestState::Failed:
                        std::rethrow_exception(this->error);
                    case RequestState::Cancelled:
                        throw api::async::FutureCancelled();
                    default:
                        throw std::logic_error("request is not complete");
                }
            }

            std::vector<std::byte> result() const
            {
                if (this->closing)
                {
                    this->closing->get();
                    return encodeResponse(std::monostate{});
                }

                switch (this->state.load(std::memory_order_acquire))
                {
                    case RequestState::Success:
                        return encodeResponse(*this->value);
                    case RequestState::Failed:
                        std::rethrow_exception(this->error);
                    case RequestState::Cancelled:
                        throw api::async::FutureCancelled();
                    default:
                        throw std::logic_error("request is not complete");
                }
            }
        };

        void Completions::append(Request& request) noexcept
        {
            request.previous = this->tail;
            request.next = nullptr;
            request.linked = true;
            if (this->tail)
            {
                this->tail->next = &request;
            }
            else
            {
                this->head = &request;
            }

            this->tail = &request;
        }

        void Completions::remove(Request& request) noexcept
        {
            if (!request.linked)
            {
                return;
            }

            if (request.previous)
            {
                request.previous->next = request.next;
            }
            else
            {
                this->head = request.next;
            }

            if (request.next)
            {
                request.next->previous = request.previous;
            }
            else
            {
                this->tail = request.previous;
            }

            request.previous = nullptr;
            request.next = nullptr;
            request.linked = false;
        }

        struct BindingWork
        {
            BindingCommand command;
            std::shared_ptr<Request> request;

            void run(api::command::CommandContext& context, bool deferred = false) noexcept
            {
                if (!this->request->tryStart())
                {
                    return;
                }

                try
                {
                    api::async::detail::NonBlockingScope scope;
                    this->request->value.emplace(this->command.execute(context));
                    this->request->finish(RequestState::Success, deferred);
                }
                catch (...)
                {
                    this->request->error = std::current_exception();
                    this->request->finish(RequestState::Failed, deferred);
                }
            }
        };

        class BindingTask final : public api::detail::CommandTask
        {
            BindingWork work;

        public:
            explicit BindingTask(BindingWork work) : work(std::move(work))
            {
            }

            void run(api::command::CommandContext& context) noexcept override
            {
                this->work.run(context);
            }

            void reject(std::exception_ptr error) noexcept override
            {
                this->work.request->reject(std::move(error));
            }
        };

        class BindingBatchTask final : public api::detail::CommandTask
        {
            std::vector<BindingWork> work;

        public:
            explicit BindingBatchTask(std::vector<BindingWork> work) : work(std::move(work))
            {
            }

            void run(api::command::CommandContext& context) noexcept override
            {
                for (auto& item : this->work)
                {
                    item.run(context, true);
                }

                // Publish a completed batch in one critical section. The
                // response thread cannot contend once per command with fills.
                std::lock_guard lock(this->work.front().request->completions->mutex);
                for (auto& item : this->work)
                {
                    if (item.request->retained && !item.request->linked)
                    {
                        item.request->completions->append(*item.request);
                    }
                }

                this->work.front().request->completions->changed.notify_one();
            }

            void reject(std::exception_ptr error) noexcept override
            {
                for (auto& item : this->work)
                {
                    item.request->reject(error);
                }
            }
        };

        struct Session
        {
            BindingConfig config;
            Exchange exchange;
            std::shared_ptr<Slots> slots = std::make_shared<Slots>();
            std::mutex mutex;
            std::shared_ptr<Completions> completions = std::make_shared<Completions>();
            DenseMap<std::uint64_t, std::shared_ptr<Request>> requests;
            bool closing = false;
            std::uint64_t closeId = 0;

            explicit Session(BindingConfig config)
                : config(std::move(config)), exchange(this->config.exchange())
            {
                this->requests.reserve(this->config.maxPendingRequests);
            }

            std::uint64_t submit(std::span<const std::byte> bytes)
            {
                auto command = BindingCommand(bytes, this->slots, this->config.observation.maxBatchQueries);
                std::lock_guard lock(this->mutex);
                if (this->closing)
                {
                    throw api::ApiError(api::ApiFailure::Closed, "exchange is closing");
                }

                if (this->requests.size() >= this->config.maxPendingRequests)
                {
                    throw api::ApiError(api::ApiFailure::QueueFull, "binding pending request limit reached");
                }

                const auto id = allocateHandle();
                auto request = std::make_shared<Request>(id, this->completions);
                auto task = std::make_unique<BindingTask>(BindingWork{std::move(command), request});
                this->requests.emplace(id, std::move(request));
                this->exchange.submitTask(std::move(task));
                return id;
            }

            void submitBatch(std::span<const std::byte> bytes, std::span<std::uint64_t> ids)
            {
                BinaryReader reader(bytes);
                persistenceCheck(reader.number<std::uint32_t>() == ids.size(), "batch count mismatch");
                std::vector<BindingWork> work;
                work.reserve(ids.size());
                for (auto& id : ids)
                {
                    const auto length = reader.number<std::uint32_t>();
                    persistenceCheck(length > 0 && length <= this->config.maxRequestBytes, "invalid batch command size");
                    auto command = BindingCommand(reader.bytes(length), this->slots, this->config.observation.maxBatchQueries);
                    id = allocateHandle();
                    work.push_back({std::move(command), std::make_shared<Request>(id, this->completions)});
                }

                reader.finish();
                std::lock_guard lock(this->mutex);
                if (this->closing)
                {
                    throw api::ApiError(api::ApiFailure::Closed, "exchange is closing");
                }

                if (ids.size() > this->config.maxPendingRequests - std::min(this->requests.size(), this->config.maxPendingRequests))
                {
                    throw api::ApiError(api::ApiFailure::QueueFull, "binding pending request limit reached");
                }

                // All allocations and map entries precede dispatch. A malformed
                // packet or allocation failure can never execute a partial batch.
                try
                {
                    for (auto& item : work)
                    {
                        this->requests.emplace(item.request->id, item.request);
                    }

                    auto task = std::make_unique<BindingBatchTask>(std::move(work));
                    this->exchange.submitTask(std::move(task));
                }
                catch (...)
                {
                    for (const auto id : ids)
                    {
                        this->requests.erase(id);
                    }

                    throw;
                }
            }

            std::uint64_t close()
            {
                std::lock_guard lock(this->mutex);
                if (this->closeId != 0 && this->requests.contains(this->closeId))
                {
                    return this->closeId;
                }

                const auto id = allocateHandle();
                auto request = std::make_shared<Request>(id, this->exchange.close());
                this->requests.emplace(id, std::move(request));
                this->closing = true;
                this->closeId = id;
                return id;
            }

            std::shared_ptr<Request> request(std::uint64_t id)
            {
                std::lock_guard lock(this->mutex);
                const auto it = this->requests.find(id);
                if (it == this->requests.end())
                {
                    throw std::invalid_argument("invalid request handle");
                }

                return it->second;
            }
        };

        std::mutex sessionsMutex;
        std::unordered_map<std::uint64_t, std::shared_ptr<Session>> sessions;

        std::shared_ptr<Session> session(std::uint64_t id)
        {
            std::lock_guard lock(sessionsMutex);
            const auto it = sessions.find(id);
            if (it == sessions.end())
            {
                throw std::invalid_argument("invalid exchange handle");
            }

            return it->second;
        }

        template <typename Function>
        int guarded(Function&& function) noexcept
        {
            try
            {
                lastError[0] = '\0';
                return function();
            }
            catch (const api::async::FutureCancelled& error)
            {
                std::snprintf(lastError.data(), lastError.size(), "%s", error.what());
                return -2;
            }
            catch (const api::ApiError& error)
            {
                std::snprintf(lastError.data(), lastError.size(), "%s", error.what());
                return error.reason() == api::ApiFailure::QueueFull ? -3 : -1;
            }
            catch (const std::exception& error)
            {
                std::snprintf(lastError.data(), lastError.size(), "%s", error.what());
                return -1;
            }
            catch (...)
            {
                std::snprintf(lastError.data(), lastError.size(), "%s", "unknown native exception");
                return -1;
            }
        }

        void require(bool condition, const char* message)
        {
            if (!condition)
            {
                throw std::invalid_argument(message);
            }
        }

        void output(std::span<const std::byte> bytes, std::uint8_t** data, std::size_t* size)
        {
            auto* buffer = static_cast<std::uint8_t*>(std::malloc(bytes.size()));
            if (buffer == nullptr)
            {
                throw std::bad_alloc();
            }

            std::memcpy(buffer, bytes.data(), bytes.size());
            *data = buffer;
            *size = bytes.size();
        }
    }
}

using namespace advmatch::binding;

extern "C" ADVMATCH_API int advmatch_abi_version(void)
{
    return 4;
}

extern "C" ADVMATCH_API const char* advmatch_last_error(void)
{
    return lastError.data();
}

extern "C" ADVMATCH_API int advmatch_exchange_create(const std::uint8_t* config, std::size_t size, std::uint64_t* exchange)
{
    return guarded([&]
    {
        require(exchange != nullptr, "exchange output is null");
        *exchange = 0;
        BindingConfig options;
        if (size != 0)
        {
            require(config != nullptr && size <= 65536, "invalid config packet");
            advmatch::BinaryReader reader(std::as_bytes(std::span(config, size)));
            require(reader.number<std::uint16_t>() == 1, "unsupported config version");
            options = readExchangeConfig(reader);
            reader.finish();
        }

        require(options.maxPendingRequests > 0 && options.maxPendingRequests <= 1048576 && options.maxRequestBytes > 0 && options.maxRequestBytes <= 67108864, "invalid binding limits");
        auto value = std::make_shared<Session>(std::move(options));
        const auto id = allocateHandle();
        {
            std::lock_guard lock(sessionsMutex);
            sessions.emplace(id, std::move(value));
        }

        *exchange = id;
        return 0;
    });
}

extern "C" ADVMATCH_API int advmatch_exchange_submit(std::uint64_t exchange, const std::uint8_t* command, std::size_t size, std::uint64_t* request)
{
    return guarded([&]
    {
        require(request != nullptr, "request output is null");
        *request = 0;
        const auto value = session(exchange);
        require(command != nullptr && size > 0 && size <= value->config.maxRequestBytes, "invalid command packet size");
        *request = value->submit(std::as_bytes(std::span(command, size)));
        return 0;
    });
}

extern "C" ADVMATCH_API int advmatch_exchange_submit_batch(std::uint64_t exchange, const std::uint8_t* commands, std::size_t size, std::uint64_t* requests, std::size_t count)
{
    return guarded([&]
    {
        require(requests != nullptr && count > 0 && count <= 1024, "invalid batch count");
        std::fill_n(requests, count, 0);
        require(commands != nullptr && size > 0 && size <= 67108864, "invalid batch packet size");
        const auto value = session(exchange);
        try
        {
            value->submitBatch(std::as_bytes(std::span(commands, size)), std::span(requests, count));
        }
        catch (...)
        {
            std::fill_n(requests, count, 0);
            throw;
        }

        return 0;
    });
}

extern "C" ADVMATCH_API int advmatch_exchange_await(std::uint64_t exchange, unsigned int timeoutMillis)
{
    return guarded([&]
    {
        require(timeoutMillis <= 100, "invalid completion wait timeout");
        const auto value = session(exchange);
        auto& queue = *value->completions;
        std::unique_lock lock(queue.mutex);
        return queue.changed.wait_for(lock, std::chrono::milliseconds(timeoutMillis), [&] { return queue.head != nullptr; }) ? 0 : 1;
    });
}

extern "C" ADVMATCH_API int advmatch_exchange_drain(std::uint64_t exchange, std::size_t limit, std::uint8_t** data, std::size_t* size)
{
    return guarded([&]
    {
        require(data != nullptr && size != nullptr, "response output is null");
        *data = nullptr;
        *size = 0;
        require(limit > 0 && limit <= 1024, "invalid drain limit");
        const auto value = session(exchange);
        std::scoped_lock lock(value->mutex, value->completions->mutex);
        if (value->completions->head == nullptr)
        {
            return 1;
        }

        advmatch::BinaryWriter writer(limit * 128);
        writer.number<std::uint32_t>(0);
        std::size_t count = 0;
        for (auto* request = value->completions->head; request != nullptr && count < limit; request = request->next)
        {
            const auto status = guarded([&]
            {
                request->checkResult();
                return 0;
            });
            writer.number(request->id);
            writer.number<std::int32_t>(status);
            const auto lengthOffset = writer.view().size();
            writer.number<std::uint32_t>(0);
            const auto payloadOffset = writer.view().size();
            if (status == 0)
            {
                writeResponse(writer, *request->value);
            }
            else
            {
                const auto error = std::string_view(lastError.data());
                writer.bytes(std::as_bytes(std::span(error.data(), error.size())));
            }

            const auto length = writer.view().size() - payloadOffset;
            require(length <= std::numeric_limits<std::uint32_t>::max(), "batch response too large");
            writer.patchNumber<std::uint32_t>(lengthOffset, static_cast<std::uint32_t>(length));
            ++count;
            if (writer.view().size() >= 67108864)
            {
                break;
            }
        }

        writer.patchNumber<std::uint32_t>(0, static_cast<std::uint32_t>(count));
        output(writer.view(), data, size);
        // Allocation/encoding failure above leaves every ticket drainable.
        for (std::size_t index = 0; index < count; ++index)
        {
            Request* request = value->completions->head;
            const auto id = request->id;
            // Cancellation can be drained before its batch publishes other
            // results. Prevent that task from relinking an already freed ID.
            request->retained = false;
            value->completions->remove(*request);
            value->requests.erase(id);
        }

        lastError[0] = '\0';
        return 0;
    });
}

extern "C" ADVMATCH_API int advmatch_exchange_close(std::uint64_t exchange, std::uint64_t* request)
{
    return guarded([&]
    {
        require(request != nullptr, "close output is null");
        *request = session(exchange)->close();
        return 0;
    });
}

extern "C" ADVMATCH_API int advmatch_exchange_poll(std::uint64_t exchange, std::uint64_t request, std::uint8_t** data, std::size_t* size)
{
    return guarded([&]
    {
        require(data != nullptr && size != nullptr, "response output is null");
        *data = nullptr;
        *size = 0;
        const auto value = session(exchange)->request(request);
        if (!value->isDone())
        {
            return 1;
        }

        output(value->result(), data, size);
        return 0;
    });
}

extern "C" ADVMATCH_API int advmatch_exchange_cancel(std::uint64_t exchange, std::uint64_t request, int* cancelled)
{
    return guarded([&]
    {
        require(cancelled != nullptr, "cancel output is null");
        *cancelled = session(exchange)->request(request)->cancel();
        return 0;
    });
}

extern "C" ADVMATCH_API int advmatch_request_release(std::uint64_t exchange, std::uint64_t request)
{
    return guarded([&]
    {
        const auto value = session(exchange);
        std::lock_guard lock(value->mutex);
        const auto iterator = value->requests.find(request);
        require(iterator != value->requests.end(), "invalid request handle");
        {
            std::lock_guard completedLock(value->completions->mutex);
            iterator->second->retained = false;
            value->completions->remove(*iterator->second);
        }

        value->requests.erase(iterator);
        return 0;
    });
}

extern "C" ADVMATCH_API int advmatch_slot_load(std::uint64_t exchange, std::uint64_t slot, std::uint8_t** data, std::size_t* size)
{
    return guarded([&]
    {
        require(data != nullptr && size != nullptr, "slot output is null");
        *data = nullptr;
        *size = 0;
        const auto result = session(exchange)->slots->get(slot)->load();
        if (result == nullptr)
        {
            return 1;
        }

        output(encodeResponse(*result), data, size);
        return 0;
    });
}

extern "C" ADVMATCH_API int advmatch_slot_release(std::uint64_t exchange, std::uint64_t slot)
{
    return guarded([&]
    {
        const auto value = session(exchange)->slots;
        std::lock_guard lock(value->mutex);
        require(value->values.erase(slot) == 1, "invalid slot handle");
        return 0;
    });
}

extern "C" ADVMATCH_API int advmatch_exchange_destroy(std::uint64_t exchange)
{
    return guarded([&]
    {
        std::shared_ptr<Session> value;
        {
            std::lock_guard lock(sessionsMutex);
            const auto it = sessions.find(exchange);
            require(it != sessions.end(), "invalid exchange handle");
            value = std::move(it->second);
            sessions.erase(it);
        }

        value->exchange.close().get();
        return 0;
    });
}

extern "C" ADVMATCH_API void advmatch_buffer_free(void* buffer)
{
    std::free(buffer);
}
