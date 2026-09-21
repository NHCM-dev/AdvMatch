#ifndef ADVMATCH_REQUESTTICKET_H
#define ADVMATCH_REQUESTTICKET_H

#include <atomic>
#include <cstdint>
#include <exception>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace advmatch::consuming
{
    template <typename Command, typename Response>
    class SingleThreadConsumer;

    template <typename Response>
    class RequestTicket;

    enum class ResponseState : std::uint8_t
    {
        Available,
        Pending,
        Completed,
        Failed
    };

    template <typename Response>
    class alignas(64) ResponseSlot
    {
        static_assert(std::is_nothrow_move_constructible_v<Response>, "fast responses must be nothrow move constructible");

        template <typename CommandType, typename ResponseType>
        friend class SingleThreadConsumer;

        friend class RequestTicket<Response>;

        std::atomic<ResponseState> state{ResponseState::Available};
        std::optional<Response> response;
        std::exception_ptr error;

        void prepare()
        {
            ResponseState expected = ResponseState::Available;
            if (!this->state.compare_exchange_strong(expected, ResponseState::Pending, std::memory_order_acq_rel))
            {
                throw std::logic_error("response slot is already in use");
            }
        }

        void complete(Response response)
        {
            this->response.emplace(std::move(response));
            this->state.store(ResponseState::Completed, std::memory_order_release);
            this->state.notify_one();
        }

        void fail(std::exception_ptr error) noexcept
        {
            this->error = std::move(error);
            this->state.store(ResponseState::Failed, std::memory_order_release);
            this->state.notify_one();
        }

        void release() noexcept
        {
            this->response.reset();
            this->error = nullptr;
            this->state.store(ResponseState::Available, std::memory_order_release);
        }

    public:
        // The producer owns this storage. It must remain alive until the related ticket is consumed.
        ResponseSlot() = default;
        ResponseSlot(const ResponseSlot&) = delete;
        ResponseSlot& operator=(const ResponseSlot&) = delete;
    };

    template <>
    class alignas(64) ResponseSlot<void>
    {
        template <typename CommandType, typename ResponseType>
        friend class SingleThreadConsumer;

        friend class RequestTicket<void>;

        std::atomic<ResponseState> state{ResponseState::Available};
        std::exception_ptr error;

        void prepare()
        {
            ResponseState expected = ResponseState::Available;
            if (!this->state.compare_exchange_strong(expected, ResponseState::Pending, std::memory_order_acq_rel))
            {
                throw std::logic_error("response slot is already in use");
            }
        }

        void complete() noexcept
        {
            this->state.store(ResponseState::Completed, std::memory_order_release);
            this->state.notify_one();
        }

        void fail(std::exception_ptr error) noexcept
        {
            this->error = std::move(error);
            this->state.store(ResponseState::Failed, std::memory_order_release);
            this->state.notify_one();
        }

        void release() noexcept
        {
            this->error = nullptr;
            this->state.store(ResponseState::Available, std::memory_order_release);
        }

    public:
        // The producer owns this storage. It must remain alive until the related ticket is consumed.
        ResponseSlot() = default;
        ResponseSlot(const ResponseSlot&) = delete;
        ResponseSlot& operator=(const ResponseSlot&) = delete;
    };

    template <typename Response>
    class RequestTicket
    {
        template <typename CommandType, typename ResponseType>
        friend class SingleThreadConsumer;

        ResponseSlot<Response>* slot = nullptr;

        explicit RequestTicket(ResponseSlot<Response>* slot) noexcept
            : slot(slot)
        {}

    public:
        RequestTicket() = default;
        RequestTicket(const RequestTicket&) = delete;
        RequestTicket& operator=(const RequestTicket&) = delete;

        RequestTicket(RequestTicket&& other) noexcept
            : slot(std::exchange(other.slot, nullptr))
        {}

        RequestTicket& operator=(RequestTicket&&) = delete;

        [[nodiscard]]
        bool isReady() const noexcept
        {
            if (!this->slot)
            {
                return false;
            }

            const ResponseState state = this->slot->state.load(std::memory_order_acquire);
            return state == ResponseState::Completed || state == ResponseState::Failed;
        }

        Response get()
        {
            if (!this->slot)
            {
                throw std::logic_error("request ticket has no response");
            }

            ResponseState state = this->slot->state.load(std::memory_order_acquire);
            while (state == ResponseState::Pending)
            {
                this->slot->state.wait(ResponseState::Pending, std::memory_order_acquire);
                state = this->slot->state.load(std::memory_order_acquire);
            }

            ResponseSlot<Response>* slot = std::exchange(this->slot, nullptr);
            if (state == ResponseState::Failed)
            {
                const std::exception_ptr error = slot->error;
                slot->release();
                std::rethrow_exception(error);
            }

            if (state != ResponseState::Completed)
            {
                slot->release();
                throw std::logic_error("request ticket has an invalid state");
            }

            Response response = std::move(*slot->response);
            slot->release();
            return response;
        }
    };

    template <>
    class RequestTicket<void>
    {
        template <typename CommandType, typename ResponseType>
        friend class SingleThreadConsumer;

        ResponseSlot<void>* slot = nullptr;

        explicit RequestTicket(ResponseSlot<void>* slot) noexcept
            : slot(slot)
        {}

    public:
        RequestTicket() = default;
        RequestTicket(const RequestTicket&) = delete;
        RequestTicket& operator=(const RequestTicket&) = delete;

        RequestTicket(RequestTicket&& other) noexcept
            : slot(std::exchange(other.slot, nullptr))
        {}

        RequestTicket& operator=(RequestTicket&&) = delete;

        [[nodiscard]]
        bool isReady() const noexcept
        {
            if (!this->slot)
            {
                return false;
            }

            const ResponseState state = this->slot->state.load(std::memory_order_acquire);
            return state == ResponseState::Completed || state == ResponseState::Failed;
        }

        void get()
        {
            if (!this->slot)
            {
                throw std::logic_error("request ticket has no response");
            }

            ResponseState state = this->slot->state.load(std::memory_order_acquire);
            while (state == ResponseState::Pending)
            {
                this->slot->state.wait(ResponseState::Pending, std::memory_order_acquire);
                state = this->slot->state.load(std::memory_order_acquire);
            }

            ResponseSlot<void>* slot = std::exchange(this->slot, nullptr);
            if (state == ResponseState::Failed)
            {
                const std::exception_ptr error = slot->error;
                slot->release();
                std::rethrow_exception(error);
            }

            if (state != ResponseState::Completed)
            {
                slot->release();
                throw std::logic_error("request ticket has an invalid state");
            }

            slot->release();
        }
    };
}

#endif // ADVMATCH_REQUESTTICKET_H
