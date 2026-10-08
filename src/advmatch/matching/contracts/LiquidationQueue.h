#ifndef ADVMATCH_LIQUIDATION_QUEUE_H
#define ADVMATCH_LIQUIDATION_QUEUE_H

#include <cstddef>

namespace advmatch
{
    namespace observe
    {
        class TradingObserver;
    }
    struct ContractParticipant;

    struct LiquidationLink
    {
        LiquidationLink* previous = nullptr;
        LiquidationLink* next = nullptr;
        ContractParticipant* participant = nullptr;
        bool linked = false;
    };

    // Single-owner-thread intrusive work queue. Enqueue, unlink and pop have
    // fixed pointer work and allocate nothing. A pass fence defers requeued
    // work until the next pass without copying or invalidating an iterator.
    class LiquidationQueue
    {
        friend class observe::TradingObserver;
        LiquidationLink* head = nullptr;
        LiquidationLink* tail = nullptr;
        std::size_t count = 0;

        void append(LiquidationLink& link) noexcept
        {
            if (link.linked)
            {
                return;
            }

            link.previous = this->tail;
            link.next = nullptr;
            link.linked = true;
            if (this->tail)
            {
                this->tail->next = &link;
            }
            else
            {
                this->head = &link;
            }

            this->tail = &link;
            this->count += link.participant != nullptr;
        }

    public:
        LiquidationQueue() = default;
        LiquidationQueue(const LiquidationQueue&) = delete;
        LiquidationQueue& operator=(const LiquidationQueue&) = delete;

        void enqueue(ContractParticipant& participant, LiquidationLink& link) noexcept
        {
            link.participant = &participant;
            this->append(link);
        }

        void remove(LiquidationLink& link) noexcept
        {
            if (!link.linked)
            {
                return;
            }

            if (link.previous)
            {
                link.previous->next = link.next;
            }
            else
            {
                this->head = link.next;
            }

            if (link.next)
            {
                link.next->previous = link.previous;
            }
            else
            {
                this->tail = link.previous;
            }

            this->count -= link.participant != nullptr;
            link.previous = nullptr;
            link.next = nullptr;
            link.linked = false;
        }

        class Pass
        {
            LiquidationQueue& queue;
            LiquidationLink fence;

        public:
            explicit Pass(LiquidationQueue& queue) noexcept
                : queue(queue)
            {
                this->queue.append(this->fence);
            }

            ~Pass()
            {
                this->queue.remove(this->fence);
            }

            Pass(const Pass&) = delete;
            Pass& operator=(const Pass&) = delete;

            ContractParticipant* next() noexcept
            {
                LiquidationLink* link = this->queue.head;
                if (!link || link == &this->fence)
                {
                    return nullptr;
                }

                this->queue.remove(*link);
                return link->participant;
            }

            bool exhausted() const noexcept
            {
                return !this->queue.head || this->queue.head == &this->fence;
            }
        };

        void clear() noexcept
        {
            while (this->head)
            {
                this->remove(*this->head);
            }
        }

        std::size_t size() const noexcept
        {
            return this->count;
        }

        bool empty() const noexcept
        {
            return this->count == 0;
        }
    };
}

#endif // ADVMATCH_LIQUIDATION_QUEUE_H
