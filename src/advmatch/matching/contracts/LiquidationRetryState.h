#ifndef ADVMATCH_LIQUIDATION_RETRY_STATE_H
#define ADVMATCH_LIQUIDATION_RETRY_STATE_H

#include "advmatch/util/Types.h"

#include <array>

namespace advmatch
{
    // Exact values, not a hash: no collision can suppress a valid fill. Kept
    // only for a blocked liquidation. No account/balance/maker pointer escapes
    // in this record, so archival and pool reuse cannot leave a dangling key.
    struct LiquidationRetryState
    {
        std::array<std::uint64_t, 72> dependencies{};
    };
}

#endif // ADVMATCH_LIQUIDATION_RETRY_STATE_H
