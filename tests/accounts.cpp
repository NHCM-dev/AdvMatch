#include "advmatch/management/UserManager.h"
#include "advmatch/matching/MatchingEngine.h"
#include "advmatch/ledging/ledgers/impl/ContractAccountLedger.h"
#include "advmatch/ledging/ledgers/impl/SpotAccountLedger.h"

#include <cstdlib>

#define CHECK(condition) \
    do \
    { \
        if (!(condition)) \
        { \
            std::abort(); \
        } \
    } while (false)

int main()
{
    using namespace advmatch;

    UserManager users;
    users.reserve(2);

    const UserId firstUserId = users.createUser();
    const UserId secondUserId = users.createUser();
    User* firstUser = users.getUser(firstUserId);
    User* secondUser = users.getUser(secondUserId);
    CHECK(firstUser && secondUser);

    SpotAccount& spotAccount = firstUser->getSpotAccount();
    ContractAccount& contractAccount = firstUser->getContractAccount();
    AccountManager& accounts = users.getAccountManager();
    SymbolManager symbols;
    MatchingEngine matchingEngine(users, symbols);
    CHECK(spotAccount.getAccountId() != contractAccount.getAccountId());
    CHECK(spotAccount.getAccountType() == AccountType::Spot);
    CHECK(contractAccount.getAccountType() == AccountType::Contract);
    CHECK(spotAccount.getUserId() == firstUserId && contractAccount.getUserId() == firstUserId);
    CHECK(accounts.getAccount(spotAccount.getAccountId()) == &spotAccount);
    CHECK(accounts.getAccount(contractAccount.getAccountId()) == &contractAccount);
    CHECK(accounts.getSpotAccount(firstUserId) == &spotAccount);
    CHECK(accounts.getContractAccount(firstUserId) == &contractAccount);
    CHECK(accounts.getAccountCount() == 4);

    SpotAccountLedger spotLedger;
    ContractAccountLedger contractLedger;
    CHECK(&spotLedger.getAccount(*firstUser) == &spotAccount);
    CHECK(&contractLedger.getAccount(*firstUser) == &contractAccount);

    CHECK(spotAccount.updateBalance(1, 1000));
    CHECK(!contractAccount.queryActiveBalance(1));
    CHECK(matchingEngine.transferToContract(firstUserId, 1, 400));
    CHECK(spotAccount.queryActiveBalance(1) == 600);
    CHECK(contractAccount.queryActiveBalance(1) == 400);

    CHECK(contractAccount.reserveBalance(1, 100));
    CHECK(!matchingEngine.transferToSpot(firstUserId, 1, 350));
    CHECK(contractAccount.queryActiveBalance(1) == 300);
    CHECK(contractAccount.queryReservedBalance(1) == 100);
    CHECK(matchingEngine.transferToSpot(firstUserId, 1, 250));
    CHECK(spotAccount.queryActiveBalance(1) == 850);
    CHECK(contractAccount.queryActiveBalance(1) == 50);
    CHECK(contractAccount.queryReservedBalance(1) == 100);

    CHECK(contractAccount.releaseReservedBalance(1, 100));
    CHECK(matchingEngine.transferToSpot(firstUserId, 1, 150));
    CHECK(spotAccount.queryActiveBalance(1) == 1000);
    CHECK(!contractAccount.queryActiveBalance(1));

    CHECK(secondUser->getSpotAccount().updateBalance(1, 10));
    CHECK(!accounts.transfer(spotAccount.getAccountId(), secondUser->getContractAccount().getAccountId(), 1, 1));
    CHECK(spotAccount.queryActiveBalance(1) == 1000);
    CHECK(!secondUser->getContractAccount().queryActiveBalance(1));

    const AccountId spotAccountId = spotAccount.getAccountId();
    const AccountId contractAccountId = contractAccount.getAccountId();
    CHECK(users.removeUser(firstUserId));
    CHECK(!matchingEngine.transferToContract(firstUserId, 1, 1));
    CHECK(!accounts.getAccount(spotAccountId));
    CHECK(!accounts.getAccount(contractAccountId));
    CHECK(accounts.getAccountCount() == 2);
}
