#ifndef ADVMATCH_SDK_MATCHING_EXCHANGE_H
#define ADVMATCH_SDK_MATCHING_EXCHANGE_H

#include "Exchange.h"
#include "manager/AssetManager.h"
#include "manager/UserManager.h"
#include "manager/SymbolManager.h"
#include "manager/AccountManager.h"
#include "manager/OrderManager.h"
#include "manager/PositionManager.h"
#include "manager/OrderBookManager.h"
#include "manager/MatchingEngine.h"
#include "manager/ContractManager.h"
#include "manager/ObservationManager.h"
#include "manager/PersistenceManager.h"

namespace advmatch::sdk
{
    class MatchingExchange final : public Exchange
    {
    public:
        using Exchange::Exchange;

        AssetManager assets() const
        {
            return AssetManager(this->session);
        }

        UserManager users() const
        {
            return UserManager(this->session);
        }

        SymbolManager symbols() const
        {
            return SymbolManager(this->session);
        }

        AccountManager accounts() const
        {
            return AccountManager(this->session);
        }

        OrderManager orders() const
        {
            return OrderManager(this->session);
        }

        PositionManager positions() const
        {
            return PositionManager(this->session);
        }

        OrderBookManager books() const
        {
            return OrderBookManager(this->session);
        }

        MatchingEngine matching() const
        {
            return MatchingEngine(this->session);
        }

        ContractManager contracts() const
        {
            return ContractManager(this->session);
        }

        ObservationManager observations() const
        {
            return ObservationManager(this->session);
        }

        PersistenceManager persistence() const
        {
            return PersistenceManager(this->session);
        }
    };
}

#endif // ADVMATCH_SDK_MATCHING_EXCHANGE_H
