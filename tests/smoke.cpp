#include "advmatch/advmatch.h"
#include "advmatch/core.hpp"

#include <disruptor/BlockingWaitStrategy.h>
#include <disruptor/EventFactory.h>
#include <disruptor/RingBuffer.h>

#include <memory>

struct Command
{
    int value = 0;
};

class CommandFactory final : public disruptor::EventFactory<Command>
{
public:
    Command newInstance() override
    {
        return {};
    }
};

int main()
{
    if (advmatch_abi_version() != 4 || advmatch::buildDescription() != "AdvMatch " ADVMATCH_EXPECTED_VERSION)
    {
        return 1;
    }

    disruptor::BlockingWaitStrategy waitStrategy;
    auto ring = disruptor::RingBuffer<Command, disruptor::SingleProducerSequencer<disruptor::BlockingWaitStrategy>>::createSingleProducer(
        std::make_shared<CommandFactory>(), 32, waitStrategy);
    const auto sequence = ring->next();
    ring->get(sequence).value = 42;
    ring->publish(sequence);
    if (ring->getCursor() != sequence || ring->get(sequence).value != 42)
    {
        return 2;
    }
}
