#include "advmatch/sdk/AdvMatch.h"
#include "advmatch/Exchange.h"
#include "advmatch/api/command/Commands.h"

#include <iostream>

int main()
{
    try
    {
        advmatch::ExchangeConfig config;
        config.persistence.enabled = false;
        auto native = std::make_shared<advmatch::Exchange>(config);
        const auto first = native->submit(advmatch::api::command::CreateUser({})).get();
        {
            advmatch::sdk::MatchingExchange facade(advmatch::sdk::wrapExchange(native));
            const auto user = facade.users().find(first.result.entityId).get();
            if (user.id() != first.result.entityId)
            {
                throw std::runtime_error("native adapter reads another session");
            }

            facade.users().create().get();
            const auto overview = native->submit(advmatch::api::command::ReadOverview{}).get();
            if (!overview.value || overview.value->activeUsers != 2)
            {
                throw std::runtime_error("native adapter mutation not visible to core");
            }

            advmatch::sdk::ObservationQuery query;
            query.kind = advmatch::sdk::ReadKind::User;
            query.userId = user.id();
            auto slot = facade.observations().createSlot(query).get();
            slot->refresh().get();
            if (slot->load()->rows<advmatch::sdk::UserView>().front().userId != user.id())
            {
                throw std::runtime_error("adapted cache uses another session");
            }
        }

        try
        {
            native->submit(advmatch::api::command::CreateUser({})).get();
            throw std::runtime_error("adapted facade did not close the native session");
        }
        catch (const advmatch::api::ApiError& error)
        {
            if (error.reason() != advmatch::api::ApiFailure::Closed)
            {
                throw;
            }
        }

        std::cout << "C++ SDK native adapter: shared state, cache and shutdown passed\n";
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
