#ifndef ADVMATCH_CONTRACT_MATH_H
#define ADVMATCH_CONTRACT_MATH_H

#include "advmatch/trading/futures/Contract.h"
#include "BalanceMath.h"

#include <limits>
#include <optional>

namespace advmatch
{
    // All business arithmetic is checked here. Intermediate calculations use
    // 128 bits; externally visible balances remain signed 64-bit atomic units.
    class ContractMath
    {
    public:
        static std::optional<Balance> balance(WideBalance value) noexcept
        {
            if (value < std::numeric_limits<Balance>::min() || value > std::numeric_limits<Balance>::max())
            {
                return std::nullopt;
            }

            return static_cast<Balance>(value);
        }

        static std::optional<Balance> notional(Price price, Quantity quantity, Quantity multiplier = 1) noexcept
        {
            if (quantity == 0)
            {
                return Balance(0);
            }

            if (price == 0 || multiplier == 0)
            {
                return std::nullopt;
            }

            const auto amount = BalanceMath::multiply(price, quantity);
            return amount && multiplier != 1 ? BalanceMath::multiply(static_cast<std::uint64_t>(*amount), multiplier) : amount;
        }

        // Most contract quantities fit in 64 bits. Avoid a software 128-bit
        // division there; preserve exact wide arithmetic for large positions.
        static WideBalance dividePositive(WideBalance numerator, WideBalance denominator) noexcept
        {
            const auto maximum = std::numeric_limits<std::uint64_t>::max();
            if (numerator >= 0 && numerator <= maximum && denominator > 0 && denominator <= maximum)
            {
                return static_cast<std::uint64_t>(numerator) / static_cast<std::uint64_t>(denominator);
            }

            return numerator / denominator;
        }

        static Balance rate(Balance amount, MMRRate rate, bool roundUp = false) noexcept
        {
            if (amount >= 0)
            {
                const auto product = BalanceMath::multiply(static_cast<std::uint64_t>(amount), rate);
                if (product)
                {
                    // A nonnegative checked product plus the bounded rounding
                    // increment fits uint64. Larger/signed cases stay wide.
                    const auto numerator = static_cast<std::uint64_t>(*product) + (roundUp && amount > 0 ? RATE_SCALE - 1 : 0);
                    return static_cast<Balance>(numerator / RATE_SCALE);
                }
            }

            const WideBalance numerator = static_cast<WideBalance>(amount) * rate;
            return static_cast<Balance>((numerator + (roundUp && amount > 0 ? RATE_SCALE - 1 : 0)) / RATE_SCALE);
        }

        static Balance margin(Balance notional, LeverageMultiplier leverage) noexcept
        {
            return notional / leverage + (notional % leverage != 0 ? 1 : 0);
        }

        static Balance proportional(Balance amount, Quantity part, Quantity total) noexcept
        {
            return part == total ? amount : static_cast<Balance>(dividePositive(static_cast<WideBalance>(amount) * part, total));
        }

        static void updatePriceReferences(Position& position, const ContractSpec& spec) noexcept
        {
            position.predictLiquidatePrice = 0;
            position.predictBankruptPrice = 0;
            if (position.positionSize == 0 || position.marginMode == MarginMode::Cross)
            {
                return;
            }

            const WideBalance quantity = static_cast<WideBalance>(position.positionSize) * spec.contractMultiplier;
            const bool longSide = position.positionSide == PositionSide::Long;
            const WideBalance numerator = longSide ? static_cast<WideBalance>(position.entryNotional) - position.initialMargin
                : static_cast<WideBalance>(position.entryNotional) + position.initialMargin;
            const WideBalance denominator = quantity * (longSide ? RATE_SCALE - spec.maintenanceMarginRate - spec.liquidationFeeRate
                : RATE_SCALE + spec.maintenanceMarginRate + spec.liquidationFeeRate);
            if (numerator <= 0 || denominator <= 0)
            {
                return;
            }

            const auto liquidation = balance(dividePositive(numerator * RATE_SCALE + (longSide ? denominator - 1 : 0), denominator));
            const auto bankruptcy = balance(dividePositive(numerator, quantity));
            if (liquidation && bankruptcy)
            {
                position.predictLiquidatePrice = static_cast<Price>(*liquidation);
                position.predictBankruptPrice = static_cast<Price>(*bankruptcy);
            }
        }

        static bool validSpec(const ContractSpec& spec, SymbolType type) noexcept
        {
            const bool product = type == SymbolType::Future || type == SymbolType::Perpetual;
            const bool schedule = type == SymbolType::Future ? spec.expiryTimestamp != 0
                : spec.expiryTimestamp == 0 && spec.fundingInterval != 0 && spec.firstFundingTimestamp != 0;
            return product && schedule && spec.contractMultiplier > 0 && spec.defaultLeverage > 0 &&
                spec.defaultLeverage <= spec.maxLeverage && spec.maintenanceMarginRate > 0 &&
                static_cast<std::uint64_t>(spec.maintenanceMarginRate) + spec.liquidationFeeRate < RATE_SCALE / spec.maxLeverage && spec.maxPositionNotional > 0 &&
                spec.makerFeeRate <= RATE_SCALE && spec.takerFeeRate <= RATE_SCALE &&
                spec.liquidationFeeRate <= RATE_SCALE && spec.priceBandRate < RATE_SCALE &&
                spec.maxFundingRate <= RATE_SCALE;
        }
    };
}

#endif // ADVMATCH_CONTRACT_MATH_H
