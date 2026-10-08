#ifndef ADVMATCH_PERSISTENCE_CONFIG_H
#define ADVMATCH_PERSISTENCE_CONFIG_H

#include "advmatch/util/Types.h"
#include <filesystem>

namespace advmatch
{
    enum class FlushPolicy
    {
        EveryCommand,
        Batch,
        Manual
    };

    enum class TailRecovery
    {
        Reject,
        TruncateIncomplete
    };

    struct PersistenceConfig
    {
        bool enabled = true;
        std::filesystem::path directory = "data/advmatch";
        FlushPolicy flushPolicy = FlushPolicy::EveryCommand;
        std::size_t flushEveryCommands = 256;
        std::uint64_t flushIntervalMilliseconds = 10;
        std::size_t checkpointEveryCommands = 0;
        std::size_t retainedCheckpoints = 2;
        bool pruneJournalAfterCheckpoint = true;
        TailRecovery tailRecovery = TailRecovery::TruncateIncomplete;
        std::size_t maxRecordBytes = 1024 * 1024;
        std::uint64_t maxSnapshotBytes = std::uint64_t{4} * 1024 * 1024 * 1024;
        std::size_t retainedRequests = 10'000;
    };

}

#endif // ADVMATCH_PERSISTENCE_CONFIG_H
