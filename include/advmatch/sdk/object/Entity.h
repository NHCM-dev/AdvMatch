#ifndef ADVMATCH_SDK_ENTITY_H
#define ADVMATCH_SDK_ENTITY_H

#include "../detail/Results.h"

namespace advmatch::sdk
{
    template <typename View, typename Id, ReadKind Kind>
    class Entity
    {
        Id entityId;

    protected:
        std::shared_ptr<ExchangeApi> api;

        ObservationQuery selection(ObservationQuery query = {}) const
        {
            query.kind = Kind;
            if constexpr (Kind == ReadKind::User)
            {
                query.userId = this->entityId;
            }
            else if constexpr (Kind == ReadKind::Asset)
            {
                query.assetId = this->entityId;
            }
            else if constexpr (Kind == ReadKind::Account)
            {
                query.accountId = this->entityId;
            }
            else if constexpr (Kind == ReadKind::Order || Kind == ReadKind::ConditionalOrder)
            {
                query.orderId = this->entityId;
            }
            else if constexpr (Kind == ReadKind::Position)
            {
                query.positionId = this->entityId;
            }
            else
            {
                query.symbolId = this->entityId;
            }

            return query;
        }

    public:
        using IdType = Id;
        using ViewType = View;

        Entity(std::shared_ptr<ExchangeApi> api, Id id)
            : entityId(id), api(std::move(api))
        {
            if (this->api == nullptr || id == 0)
            {
                throw std::invalid_argument("entity requires a session and a nonzero ID");
            }
        }

        virtual ~Entity() = default;

        Id id() const noexcept
        {
            return this->entityId;
        }

        Id handle() const noexcept
        {
            return this->entityId;
        }

        void requireOwner(const std::shared_ptr<ExchangeApi>& owner) const
        {
            if (this->api != owner)
            {
                throw std::invalid_argument("entity belongs to another exchange");
            }
        }

        virtual CompletableFuture<View> snapshot() const
        {
            return this->api->template readRows<View>(this->selection()).thenApply(detail::Results::one<View>);
        }

        bool operator==(const Entity& other) const noexcept
        {
            return this->api == other.api && this->entityId == other.entityId;
        }
    };
}

#endif // ADVMATCH_SDK_ENTITY_H
