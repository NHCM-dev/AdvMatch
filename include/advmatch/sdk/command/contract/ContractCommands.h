#ifndef ADVMATCH_SDK_CONTRACTCOMMANDS_H
#define ADVMATCH_SDK_CONTRACTCOMMANDS_H

#include "../trading/TradingOperation.h"

namespace advmatch::sdk::command
{
    using AddContractBook = TradingOperation<advmatch::commands::AddContractBook>;
    using SetLeverage = TradingOperation<advmatch::commands::SetLeverage>;
    using AddMargin = TradingOperation<advmatch::commands::AddMargin>;
    using UpdateMarkPrice = TradingOperation<advmatch::commands::UpdateMarkPrice>;
    using FundInsurance = TradingOperation<advmatch::commands::FundInsurance>;
    using SettleFunding = TradingOperation<advmatch::commands::SettleFunding>;
    using Expire = TradingOperation<advmatch::commands::Expire>;
    using Deliver = TradingOperation<advmatch::commands::Deliver>;
    using Liquidate = TradingOperation<advmatch::commands::Liquidate>;
    using ProcessLiquidations = TradingOperation<advmatch::commands::ProcessLiquidations>;
    using SetAutomaticLiquidationBudget = TradingOperation<advmatch::commands::SetAutomaticLiquidationBudget>;
    using ConfigureReferencePrice = TradingOperation<advmatch::commands::ConfigureReferencePrice>;
    using ConfigureContractSchedule = TradingOperation<advmatch::commands::ConfigureContractSchedule>;
    using PublishExternalPrice = TradingOperation<advmatch::commands::PublishExternalPrice>;
    using RefreshReferencePrice = TradingOperation<advmatch::commands::RefreshReferencePrice>;
    using UpdateReferencePrice = TradingOperation<advmatch::commands::UpdateReferencePrice>;
}

#endif // ADVMATCH_SDK_CONTRACTCOMMANDS_H
