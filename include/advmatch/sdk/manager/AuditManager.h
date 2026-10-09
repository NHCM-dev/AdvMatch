#ifndef ADVMATCH_SDK_AUDIT_MANAGER_H
#define ADVMATCH_SDK_AUDIT_MANAGER_H

#include "Manager.h"

namespace advmatch::sdk
{
    class AuditManager final : public Manager
    {
    public:
        using Manager::Manager;

        CompletableFuture<AuditReport> report(std::size_t maxIssues = 64) const
        {
            ObservationQuery query;
            query.limit = maxIssues;
            return this->read<AuditReport>(ReadKind::Audit, query).thenApply(detail::Results::one<AuditReport>);
        }
    };
}

#endif // ADVMATCH_SDK_AUDIT_MANAGER_H
