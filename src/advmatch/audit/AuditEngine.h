#ifndef ADVMATCH_AUDIT_ENGINE_H
#define ADVMATCH_AUDIT_ENGINE_H

#include "model/AuditReport.h"
#include "advmatch/util/Types.h"

namespace advmatch
{
    class MatchingEngine;
    class AssetManager;
}

namespace advmatch::audit
{
    class AuditEngine
    {
        const MatchingEngine& matching;
        DenseMap<AssetId, WideBalance> externalFlows;

    public:
        explicit AuditEngine(const MatchingEngine& matching)
            : matching(matching)
        {
        }

        void recordExternalFlow(AssetId assetId, BalanceChange change)
        {
            this->externalFlows[assetId] += change;
        }

        // Full owner-thread validation scans and sorts retained state, kept
        // out of trade execution. maxIssues bounds diagnostic output, not work.
        AuditReport report(const AssetManager& assets, std::size_t maxIssues = 64) const;
    };
}

#endif // ADVMATCH_AUDIT_ENGINE_H
