#ifndef ADVMATCH_COMMAND_RESULT_H
#define ADVMATCH_COMMAND_RESULT_H

#include "advmatch/trading/OrderTypes.h"

namespace advmatch
{
    enum class CommandResultType : std::uint8_t
    {
        Management,
        Balance,
        Place,
        Modify,
        Cancel,
        Contract,
        Archive,
        Conditional
    };

    enum class ManagementState : std::uint8_t
    {
        Success,
        Rejected
    };

    struct CommandResult
    {
        CommandResultType type = CommandResultType::Management;
        // State belongs to type: ManagementState, BalanceOpResult,
        // PlaceOrderState, ModifyOrderState, CancelOrderState, ContractState,
        // ConditionalOrderState.
        std::uint8_t state = 0;
        bool accepted = false;
        std::uint64_t entityId = 0;
        PositionId positionId = 0;
        OrderStatus orderStatus = OrderStatus::Rejected;
        Quantity filled = 0;
        Quantity unfilled = 0;
        std::uint64_t affected = 0;
        std::uint64_t scannedOrders = 0;
        std::uint64_t scannedPositions = 0;
        std::uint64_t archivedOrders = 0;
        std::uint64_t archivedPositions = 0;

        explicit operator bool() const noexcept
        {
            return this->accepted;
        }

        bool operator==(const CommandResult&) const = default;
    };

    struct CommandReceipt
    {
        CommandSequence sequence;
        CommandSequence durableThrough;
        bool duplicate;
        CommandResult result;

        bool durable() const noexcept
        {
            return this->sequence != 0 && this->sequence <= this->durableThrough;
        }
    };
}

#endif // ADVMATCH_COMMAND_RESULT_H
