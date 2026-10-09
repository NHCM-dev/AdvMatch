#ifndef ADVMATCH_REFERENCE_PRICE_CODEC_H
#define ADVMATCH_REFERENCE_PRICE_CODEC_H

#include "BinaryCodec.h"
#include "advmatch/trading/futures/ReferencePrice.h"

namespace advmatch
{
    class ReferencePriceCodec
    {
    public:
        static void write(BinaryWriter& writer, const PriceSource& value)
        {
            writer.enumeration(value.type);
            writer.number(value.sourceId);
            writer.number(value.weight);
            writer.number(value.numerator);
            writer.number(value.denominator);
        }

        static PriceSource readPriceSource(BinaryReader& reader)
        {
            PriceSource value;
            value.type = reader.enumeration(PriceSourceType::External);
            value.sourceId = reader.number<std::uint64_t>();
            value.weight = reader.number<std::uint32_t>();
            value.numerator = reader.number<std::uint32_t>();
            value.denominator = reader.number<std::uint32_t>();
            return value;
        }

        static void write(BinaryWriter& writer, const ExternalPrice& value)
        {
            writer.number(value.sourceId);
            writer.number(value.price);
            writer.number(value.timestamp);
        }

        static ExternalPrice readExternalPrice(BinaryReader& reader)
        {
            ExternalPrice value;
            value.sourceId = reader.number<std::uint64_t>();
            value.price = reader.number<Price>();
            value.timestamp = reader.number<Timestamp>();
            return value;
        }

        static void write(BinaryWriter& writer, const ReferencePrices& value)
        {
            writer.number(value.indexPrice);
            writer.number(value.markPrice);
            writer.number(value.premiumRate);
            writer.number(value.fundingRate);
            writer.number(value.deliveryPrice);
            writer.number(value.sourceCount);
        }

        static ReferencePrices readReferencePrices(BinaryReader& reader)
        {
            ReferencePrices value;
            value.indexPrice = reader.number<Price>();
            value.markPrice = reader.number<Price>();
            value.premiumRate = reader.number<std::int32_t>();
            value.fundingRate = reader.number<std::int32_t>();
            value.deliveryPrice = reader.number<Price>();
            value.sourceCount = reader.number<std::uint32_t>();
            return value;
        }

        static void write(BinaryWriter& writer, const ContractSchedule& value)
        {
            writer.number(value.fundingInterval);
            writer.number(value.nextFundingTimestamp);
            writer.number(value.maxFundingRate);
            writer.number(value.expiryTimestamp);
        }

        static ContractSchedule readContractSchedule(BinaryReader& reader)
        {
            ContractSchedule value;
            value.fundingInterval = reader.number<Timestamp>();
            value.nextFundingTimestamp = reader.number<Timestamp>();
            value.maxFundingRate = reader.number<MMRRate>();
            value.expiryTimestamp = reader.number<Timestamp>();
            return value;
        }

        static void write(BinaryWriter& writer, const ReferencePriceConfig& value)
        {
            writer.number<std::uint64_t>(value.sources.size());
            for (const auto& source : value.sources)
            {
                write(writer, source);
            }

            writer.number(value.minimumSources);
            writer.number(value.maxAge);
            writer.number(value.maxDeviationRate);
            writer.number(value.markClampRate);
            writer.number(value.interestRate);
            writer.number(value.fundingClampRate);
            writer.boolean(value.automaticFunding);
            writer.boolean(value.automaticDelivery);
            writer.number(value.maxCatchUpPeriods);
        }

        static ReferencePriceConfig readReferencePriceConfig(BinaryReader& reader)
        {
            ReferencePriceConfig value;
            const auto count = reader.count(21);
            persistenceCheck(count <= 32, "too many price sources");
            value.sources.reserve(count);
            for (std::size_t index = 0; index < count; ++index)
            {
                value.sources.push_back(readPriceSource(reader));
            }

            value.minimumSources = reader.number<std::uint32_t>();
            value.maxAge = reader.number<Timestamp>();
            value.maxDeviationRate = reader.number<MMRRate>();
            value.markClampRate = reader.number<MMRRate>();
            value.interestRate = reader.number<std::int32_t>();
            value.fundingClampRate = reader.number<MMRRate>();
            value.automaticFunding = reader.boolean();
            value.automaticDelivery = reader.boolean();
            value.maxCatchUpPeriods = reader.number<std::uint32_t>();
            return value;
        }

    };
}

#endif // ADVMATCH_REFERENCE_PRICE_CODEC_H

