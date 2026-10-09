#ifndef ADVMATCH_CONFIGURABLE_WAIT_STRATEGY_H
#define ADVMATCH_CONFIGURABLE_WAIT_STRATEGY_H

#include <disruptor/BlockingWaitStrategy.h>
#include <disruptor/BusySpinWaitStrategy.h>

namespace advmatch::consuming
{
    // Set once before start(); never mutate while producers/worker are alive.
    // Busy spin reserves a CPU core, including while the exchange is idle.
    class ConfigurableWaitStrategy
    {
        disruptor::BlockingWaitStrategy blocking;
        disruptor::BusySpinWaitStrategy spinning;

    public:
        static constexpr bool kIsBlockingStrategy = true;
        bool busySpin = false;

        template <typename Barrier>
        std::int64_t waitFor(std::int64_t sequence, const disruptor::Sequence& cursor, const disruptor::Sequence& dependent, Barrier& barrier)
        {
            return this->busySpin ? this->spinning.waitFor(sequence, cursor, dependent, barrier)
                : this->blocking.waitFor(sequence, cursor, dependent, barrier);
        }

        void signalAllWhenBlocking()
        {
            if (!this->busySpin)
            {
                this->blocking.signalAllWhenBlocking();
            }
        }
    };
}

#endif // ADVMATCH_CONFIGURABLE_WAIT_STRATEGY_H
