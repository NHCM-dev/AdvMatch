#ifndef ADVMATCH_PERSISTENCE_TYPES_H
#define ADVMATCH_PERSISTENCE_TYPES_H

#include "advmatch/util/Types.h"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace advmatch
{
    enum class PersistenceFailure : std::uint8_t
    {
        InvalidConfiguration,
        Io,
        Locked,
        CorruptData,
        UnsupportedVersion,
        ReplayMismatch,
        Faulted,
        RequestConflict,
        RequestExpired
    };

    class PersistenceError : public std::runtime_error
    {
        PersistenceFailure failure;

    public:
        PersistenceError(PersistenceFailure failure, const std::string& message)
            : std::runtime_error(message), failure(failure)
        {
        }

        PersistenceFailure reason() const noexcept
        {
            return this->failure;
        }
    };

    enum class JournalRecordType : std::uint8_t
    {
        Intent = 1,
        Commit = 2
    };

    struct JournalRecord
    {
        JournalRecordType type;
        CommandSequence sequence;
        std::vector<std::byte> payload;
    };

    struct StoredSnapshot
    {
        CommandSequence sequence;
        std::vector<std::byte> payload;
    };

}

#endif // ADVMATCH_PERSISTENCE_TYPES_H
