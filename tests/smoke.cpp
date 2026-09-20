#include "advmatch/advmatch.h"
#include "advmatch/core.hpp"

#include <disruptor/BusySpinWaitStrategy.h>
#include <disruptor/EventFactory.h>
#include <disruptor/RingBuffer.h>

#include <memory>

struct Command {
    int value = 0;
};

class CommandFactory final : public disruptor::EventFactory<Command> {
public:
    Command newInstance() override { return {}; }
};

int main() {
    if (advmatch_abi_version() != 1 ||
        advmatch::build_description() != "AdvMatch " ADVMATCH_EXPECTED_VERSION) return 1;

    disruptor::BusySpinWaitStrategy wait_strategy;
    auto ring = disruptor::RingBuffer<Command,
        disruptor::SingleProducerSequencer<disruptor::BusySpinWaitStrategy>>
        ::createSingleProducer(std::make_shared<CommandFactory>(), 1024, wait_strategy);
    const auto sequence = ring->next();
    ring->get(sequence).value = 42;
    ring->publish(sequence);
    if (ring->getCursor() != sequence || ring->get(sequence).value != 42) return 2;
}
