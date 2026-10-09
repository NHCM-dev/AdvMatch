#ifndef ADVMATCH_SDK_MANAGER_H
#define ADVMATCH_SDK_MANAGER_H

#include "../object/Symbol.h"

namespace advmatch::sdk
{
    class Manager
    {
    protected:
        std::shared_ptr<ExchangeApi> api;

        template <typename Row>
        CompletableFuture<ReadResponse<Row>> read(ReadKind kind, ObservationQuery query) const
        {
            query.kind = kind;
            return this->api->template readRows<Row>(std::move(query));
        }

    public:
        explicit Manager(std::shared_ptr<ExchangeApi> api)
            : api(std::move(api))
        {
            if (this->api == nullptr)
            {
                throw std::invalid_argument("manager requires an exchange session");
            }
        }

        virtual ~Manager() = default;
    };

    template <typename Handle, ReadKind Kind>
    class EntityManager : public Manager
    {
    public:
        using Manager::Manager;

        // get constructs a cheap ID reference; find validates it asynchronously.
        Handle get(typename Handle::IdType id) const
        {
            return Handle(this->api, id);
        }

        CompletableFuture<Handle> find(typename Handle::IdType id) const
        {
            return detail::Results::attempt<Handle>([&]
            {
                auto handle = this->get(id);
                return handle.snapshot().thenApply([handle](const typename Handle::ViewType&)
                {
                    return handle;
                });
            });
        }

        CompletableFuture<ReadResponse<typename Handle::ViewType>> list(ObservationQuery query = {}) const
        {
            return this->template read<typename Handle::ViewType>(Kind, std::move(query));
        }
    };
}

#endif // ADVMATCH_SDK_MANAGER_H
