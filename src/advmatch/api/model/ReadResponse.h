#ifndef ADVMATCH_READ_RESPONSE_H
#define ADVMATCH_READ_RESPONSE_H

#include "advmatch/observe/model/ObservationTypes.h"

namespace advmatch::api
{
    template <typename Row>
    struct ReadResponse
    {
        observe::ObservationQuery query;
        observe::ReadPage page;
        std::vector<Row> values;

        std::span<const Row> rows() const noexcept
        {
            return this->values;
        }
    };

    struct OverviewResponse
    {
        observe::ObservationQuery query;
        observe::ReadPage page;
        std::optional<observe::OverviewView> value;
    };
}

#endif // ADVMATCH_READ_RESPONSE_H
