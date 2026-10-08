#include "advmatch/consuming/SingleThreadConsumer.h"
#include "advmatch/persistence/PersistentTradingEngine.h"

#include <cstdio>
#include <stdexcept>

namespace
{
    using namespace advmatch;

    enum class WorkType
    {
        Execute,
        Status,
        Checkpoint,
        Close
    };

    struct Work
    {
        WorkType type = WorkType::Status;
        TradingCommand command = commands::CreateUser{};
        std::uint64_t requestId = 0;
    };

    struct Response
    {
        CommandReceipt receipt{};
        CommandSequence sequence = 0;
        CommandSequence durable = 0;
        std::size_t users = 0;
    };

    class TradingConsumer final : public consuming::SingleThreadConsumer<Work, Response>
    {
        PersistentTradingEngine trading;

    protected:
        Response consume(Work&& work) override
        {
            Response response;
            switch (work.type)
            {
                case WorkType::Execute:
                    response.receipt = this->trading.execute(work.command, work.requestId);
                    break;
                case WorkType::Status:
                    this->trading.tick();
                    break;
                case WorkType::Checkpoint:
                    this->trading.checkpoint();
                    break;
                case WorkType::Close:
                    this->trading.close();
                    break;
            }

            response.sequence = this->trading.lastSequence();
            response.durable = this->trading.durableThrough();
            response.users = this->trading.getUserManager().getUserCount();
            return response;
        }

    public:
        explicit TradingConsumer(PersistenceConfig config)
            : trading(std::move(config))
        {
        }

        ~TradingConsumer() override
        {
            this->stopBeforeDerivedDestruction();
        }

        Response submit(Work work)
        {
            consuming::ResponseSlot<Response> slot;
            return this->post(std::move(work), slot).get();
        }
    };
}

int main(int argc, char** argv)
{
    try
    {
        PersistenceConfig config;
        config.directory = argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::path("data/example");
        const std::string policy = argc > 2 ? argv[2] : "every";
        if (policy == "batch")
        {
            config.flushPolicy = FlushPolicy::Batch;
        }
        else if (policy == "manual")
        {
            config.flushPolicy = FlushPolicy::Manual;
        }
        else if (policy != "every")
        {
            throw std::invalid_argument("flush policy must be every, batch or manual");
        }

        TradingConsumer consumer(config);
        consumer.start(1024);
        const auto restored = consumer.submit({WorkType::Status});
        std::printf("Recovered: users=%zu sequence=%llu durable=%llu\n", restored.users, static_cast<unsigned long long>(restored.sequence), static_cast<unsigned long long>(restored.durable));

        const auto created = consumer.submit({WorkType::Execute, commands::CreateUser{}, restored.sequence + 1});
        std::printf("CreateUser: id=%llu durable=%s\n", static_cast<unsigned long long>(created.receipt.result.entityId), created.receipt.durable() ? "yes" : "no");

        const auto checkpoint = consumer.submit({WorkType::Checkpoint});
        std::printf("Checkpoint: sequence=%llu durable=%llu\n", static_cast<unsigned long long>(checkpoint.sequence), static_cast<unsigned long long>(checkpoint.durable));
        consumer.submit({WorkType::Close});
        consumer.stop();
        std::puts("Run again with the same directory to recover the previous state.");
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "Persistence example failed: %s\n", error.what());
        return 1;
    }
}
