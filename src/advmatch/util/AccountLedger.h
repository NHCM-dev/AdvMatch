#ifndef ADVMATCH_ACCOUNT_LEDGER_H
#define ADVMATCH_ACCOUNT_LEDGER_H

#include "../matching/Matching.h"

namespace advmatch::util
{
    // The ledger is the only matching component that mutates account balances.
    // Risk engines calculate requirements; settlement engines ask the ledger to
    // apply the corresponding reservation, consumption, release, and credit.
    class AccountLedger
    {
    public:
        AccountLedger() = delete;

        [[nodiscard]]
        static bool reserve(OrderNode& node, Balance requiredBalance);

        static void releaseTo(OrderNode& node, Balance requiredBalance);
        static void releaseAll(OrderNode& node);
        static void consume(OrderNode& node, PositiveBalanceChange amount);
        static void credit(User& user, AssetId assetId, PositiveBalanceChange amount);
    };
} // advmatch::util

#endif // ADVMATCH_ACCOUNT_LEDGER_H
