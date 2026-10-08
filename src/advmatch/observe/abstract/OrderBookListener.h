#ifndef ADVMATCH_OBSERVE_ORDER_BOOK_LISTENER_H
#define ADVMATCH_OBSERVE_ORDER_BOOK_LISTENER_H

namespace advmatch
{
    struct OrderNode;

    namespace observe
    {
        // Optional owner-thread index maintenance. Allocation failure must be
        // handled by the listener and must never interrupt matching.
        class OrderBookListener
        {
        public:
            virtual ~OrderBookListener() = default;
            virtual void onRested(const OrderNode& order) noexcept = 0;
            virtual void onRemoved(const OrderNode& order) noexcept = 0;
        };
    }
}

#endif // ADVMATCH_OBSERVE_ORDER_BOOK_LISTENER_H
