#ifndef ADVMATCH_SDK_CONTRACT_MANAGER_H
#define ADVMATCH_SDK_CONTRACT_MANAGER_H

#include "Manager.h"

namespace advmatch::sdk
{
    class ContractManager final : public Manager
    {
        template <typename Operation>
        CompletableFuture<CommandReceipt> update(const Symbol& symbol, Operation operation, std::uint64_t requestId) const
        {
            return detail::Results::attempt<CommandReceipt>([&]
            {
                symbol.requireOwner(this->api);
                return detail::Results::checked(this->api, std::move(operation), requestId);
            });
        }

    public:
        using Manager::Manager;

        CompletableFuture<MarketView> market(const Symbol& symbol) const
        {
            return detail::Results::attempt<MarketView>([&]
            {
                symbol.requireOwner(this->api);
                return symbol.market();
            });
        }

        CompletableFuture<ReadResponse<MarketView>> markets(ObservationQuery query = {}) const
        {
            return this->read<MarketView>(ReadKind::Markets, std::move(query));
        }

        CompletableFuture<ReadResponse<PositionView>> positions(const Symbol& symbol, ObservationQuery query = {}) const
        {
            return detail::Results::attempt<ReadResponse<PositionView>>([&]
            {
                symbol.requireOwner(this->api);
                query.symbolId = symbol.id();
                return this->read<PositionView>(ReadKind::MarketPositions, std::move(query));
            });
        }

        CompletableFuture<ReadResponse<ContractEventView>> events(ObservationQuery query = {}) const
        {
            return this->read<ContractEventView>(ReadKind::ContractEvents, std::move(query));
        }

        CompletableFuture<CommandReceipt> setAutomaticLiquidationBudget(std::uint64_t maxCandidates, std::uint64_t requestId = 0) const
        {
            return detail::Results::checked(this->api, commands::SetAutomaticLiquidationBudget{maxCandidates}, requestId);
        }

        CompletableFuture<CommandReceipt> updateMark(const Symbol& symbol, Price price, Timestamp timestamp, std::uint64_t requestId = 0) const
        {
            return this->update(symbol, commands::UpdateMarkPrice{symbol.id(), price, timestamp}, requestId);
        }

        CompletableFuture<CommandReceipt> settleFunding(const Symbol& symbol, std::int32_t rate, Timestamp timestamp, std::uint64_t requestId = 0) const
        {
            return this->update(symbol, commands::SettleFunding{symbol.id(), rate, timestamp}, requestId);
        }

        CompletableFuture<CommandReceipt> configureReference(const Symbol& symbol, ReferencePriceConfig config, std::uint64_t requestId = 0) const
        {
            return this->update(symbol, commands::ConfigureReferencePrice{symbol.id(), std::move(config)}, requestId);
        }

        CompletableFuture<CommandReceipt> bindSpot(const Symbol& symbol, const Symbol& spot, std::uint64_t requestId = 0) const
        {
            return detail::Results::attempt<CommandReceipt>([&]
            {
                spot.requireOwner(this->api);
                ReferencePriceConfig config;
                config.sources.push_back(PriceSource{PriceSourceType::Spot, spot.id()});
                return this->configureReference(symbol, std::move(config), requestId);
            });
        }

        CompletableFuture<CommandReceipt> configureSchedule(const Symbol& symbol, ContractSchedule schedule, Timestamp timestamp, std::uint64_t requestId = 0) const
        {
            return this->update(symbol, commands::ConfigureContractSchedule{symbol.id(), schedule, timestamp}, requestId);
        }

        CompletableFuture<CommandReceipt> publishPrice(const Symbol& symbol, std::uint64_t sourceId, Price price, Timestamp timestamp, std::uint64_t requestId = 0) const
        {
            return this->update(symbol, commands::PublishExternalPrice{symbol.id(), ExternalPrice{sourceId, price, timestamp}}, requestId);
        }

        // Caller supplies timestamps; refresh computes prices and handles due
        // funding/delivery on the owner thread. No background wall-clock work.
        CompletableFuture<CommandReceipt> refresh(const Symbol& symbol, Timestamp timestamp, std::uint64_t requestId = 0) const
        {
            return this->update(symbol, commands::RefreshReferencePrice{symbol.id(), timestamp}, requestId);
        }

        CompletableFuture<CommandReceipt> updateReference(const Symbol& symbol, ReferencePrices prices, Timestamp timestamp, std::uint64_t requestId = 0) const
        {
            return this->update(symbol, commands::UpdateReferencePrice{symbol.id(), prices, timestamp}, requestId);
        }

        CompletableFuture<CommandReceipt> expire(const Symbol& symbol, Timestamp timestamp, std::uint64_t requestId = 0) const
        {
            return this->update(symbol, commands::Expire{symbol.id(), timestamp}, requestId);
        }

        CompletableFuture<CommandReceipt> deliver(const Symbol& symbol, Price price, Timestamp timestamp, std::uint64_t requestId = 0) const
        {
            return this->update(symbol, commands::Deliver{symbol.id(), price, timestamp}, requestId);
        }

        CompletableFuture<CommandReceipt> processLiquidations(const Symbol& symbol, Timestamp timestamp, bool force = true, std::uint64_t maxCandidates = std::numeric_limits<std::uint64_t>::max(), std::uint64_t requestId = 0) const
        {
            return this->update(symbol, commands::ProcessLiquidations{symbol.id(), timestamp, force, maxCandidates}, requestId);
        }

        CompletableFuture<CommandReceipt> fundInsurance(const Symbol& symbol, const Account& source, PositiveBalanceChange amount, std::uint64_t requestId = 0) const
        {
            return detail::Results::attempt<CommandReceipt>([&]
            {
                source.requireOwner(this->api);
                return this->update(symbol, commands::FundInsurance{symbol.id(), source.id(), amount}, requestId);
            });
        }
    };
}

#endif // ADVMATCH_SDK_CONTRACT_MANAGER_H
