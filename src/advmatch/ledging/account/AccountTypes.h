#ifndef ADVMATCH_ACCOUNT_TYPES_H
#define ADVMATCH_ACCOUNT_TYPES_H

#include "advmatch/util/ValueTypes.h"

namespace advmatch
{
    enum class AccountType : std::uint8_t
    {
        Spot,
        Contract
    };

}

#endif // ADVMATCH_ACCOUNT_TYPES_H
