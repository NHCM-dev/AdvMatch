#ifndef ADVMATCH_COMMAND_CODEC_H
#define ADVMATCH_COMMAND_CODEC_H

#include "BinaryCodec.h"
#include "advmatch/api/command/model/TradingCommand.h"

namespace advmatch
{
    struct CommandEnvelope
    {
        std::uint64_t requestId;
        TradingCommand command;
    };

    std::vector<std::byte> encodeCommand(const TradingCommand& command, std::uint64_t requestId);
    CommandEnvelope decodeCommand(std::span<const std::byte> bytes);
    std::vector<std::byte> encodeResult(const CommandResult& result);
    CommandResult decodeResult(std::span<const std::byte> bytes);
}

#endif // ADVMATCH_COMMAND_CODEC_H
