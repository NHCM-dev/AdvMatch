#ifndef ADVMATCH_AUDIT_ENGINE_H
#define ADVMATCH_AUDIT_ENGINE_H

#include "model/AuditReport.h"
#include "advmatch/util/Types.h"
#include "advmatch/trading/Asset.h"
#include <vector>

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

        auto prepareExternalFlows(const std::vector<BalanceDelta>& changes) const
        {
            auto next = this->externalFlows;
            for (const auto& leg : changes) next[leg.assetId] += leg.change;
            return next;
        }

        void commitExternalFlows(DenseMap<AssetId, WideBalance>& prepared) noexcept
        {
            this->externalFlows.swap(prepared);
        }

        // Full owner-thread validation scans and sorts retained state, kept
        // out of trade execution. maxIssues bounds diagnostic output, not work.
        AuditReport report(const AssetManager& assets, std::size_t maxIssues = 64) const;
    };
}

#endif // ADVMATCH_AUDIT_ENGINE_H
