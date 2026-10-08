#ifndef ADVMATCH_TRADING_SERVICE_H
#define ADVMATCH_TRADING_SERVICE_H

#include "../command/model/TradingCommand.h"

namespace advmatch::api
{
    class TradingService
    {
    public:
        virtual ~TradingService() = default;
        virtual CommandReceipt execute(const TradingCommand& command, std::uint64_t requestId) = 0;
    };
}

#endif // ADVMATCH_TRADING_SERVICE_H
