#ifndef ADVMATCH_AUDIT_REPORT_H
#define ADVMATCH_AUDIT_REPORT_H

#include "advmatch/util/ValueTypes.h"
#include <string>
#include <vector>

namespace advmatch::audit
{
    enum class AuditIssueType : std::uint8_t
    {
        Conservation,
        NegativeBalance,
        Reservation,
        Book,
        Risk
    };

    struct AssetAudit
    {
        AssetId assetId = 0;
        // Decimal integers preserve totals exceeding a single 64-bit account.
        std::string externalFlow;
        std::string active;
        std::string reserved;
        std::string spotFees;
        std::string contractFees;
        std::string insurance;
        std::string clearing;
        std::string fundingRounding;
        std::string actual;
        std::string difference;
        bool conserved = true;
    };

    struct AuditIssue
    {
        AuditIssueType type;
        std::uint64_t entityId;
        AssetId assetId;
        std::string message;
    };

    struct AuditReport
    {
        bool conserved = true;
        bool consistent = true;
        std::uint64_t stateHash = 0;
        std::uint64_t checkedAccounts = 0;
        std::uint64_t checkedOrders = 0;
        std::uint64_t checkedPositions = 0;
        std::uint64_t issueCount = 0;
        std::vector<AssetAudit> assets;
        std::vector<AuditIssue> issues;
    };
}

#endif // ADVMATCH_AUDIT_REPORT_H
