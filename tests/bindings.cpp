#include "advmatch/advmatch.h"
#include "advmatch/api/binding/WireModels.h"
#include "advmatch/persistence/codec/CommandCodec.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

#define CHECK(condition) do { if (!(condition)) { std::fprintf(stderr, "Binding check failed at %d: %s; native=%s\n", __LINE__, #condition, advmatch_last_error()); std::abort(); } } while (false)

namespace
{
    using namespace advmatch;
    using namespace advmatch::binding;

    std::vector<std::byte> wait(std::uint64_t exchange, std::uint64_t request)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        for (;;)
        {
            std::uint8_t* data = nullptr;
            std::size_t size = 0;
            const auto status = advmatch_exchange_poll(exchange, request, &data, &size);
            if (status == 0)
            {
                const auto span = std::as_bytes(std::span(data, size));
                std::vector<std::byte> bytes(span.begin(), span.end());
                advmatch_buffer_free(data);
                CHECK(advmatch_request_release(exchange, request) == 0);
                return bytes;
            }

            CHECK(status == 1 && data == nullptr && size == 0);
            CHECK(std::chrono::steady_clock::now() < deadline);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    std::uint64_t submit(std::uint64_t exchange, const std::vector<std::byte>& packet)
    {
        std::uint64_t request = 0;
        CHECK(advmatch_exchange_submit(exchange, reinterpret_cast<const std::uint8_t*>(packet.data()), packet.size(), &request) == 0);
        CHECK(request != 0);
        return request;
    }

    template <typename T, typename Read>
    void roundTrip(const T& value, Read read)
    {
        BinaryWriter first;
        write(first, value);
        const auto bytes = first.take();
        BinaryReader reader(bytes);
        const auto recovered = read(reader);
        reader.finish();
        BinaryWriter second;
        write(second, recovered);
        CHECK(second.take() == bytes);
    }
}

int main()
{
    CHECK(advmatch_abi_version() == 4);
    BindingConfig config;
    config.persistence.enabled = false;
    config.maxPendingRequests = 1;
    config.observation.indexUserOpenOrders = true;
    roundTrip(config, readExchangeConfig);
    roundTrip(ContractSpec{}, readContractSpec);
    roundTrip(observe::ObservationQuery{}, readObservationQuery);
    roundTrip(ContractOrderOptions{}, readContractOrderOptions);
    BinaryWriter writer;
    writer.number<std::uint16_t>(1);
    write(writer, config);
    const auto packet = writer.take();
    std::uint64_t exchange = 0;
    CHECK(advmatch_exchange_create(reinterpret_cast<const std::uint8_t*>(packet.data()), packet.size(), &exchange) == 0);

    BinaryWriter trading;
    trading.number<std::uint16_t>(1);
    trading.number<std::uint8_t>(0);
    trading.bytes(encodeCommand(commands::CreateUser{}, 1));
    const auto command = trading.take();
    auto request = submit(exchange, command);
    std::uint64_t rejected = 0;
    CHECK(advmatch_exchange_submit(exchange, reinterpret_cast<const std::uint8_t*>(command.data()), command.size(), &rejected) == -3 && rejected == 0);
    const auto bytes = wait(exchange, request);
    BinaryReader receiptReader(bytes);
    CHECK(receiptReader.number<std::uint16_t>() == 1 && receiptReader.number<std::uint8_t>() == 1);
    const auto receipt = readCommandReceipt(receiptReader);
    CHECK(receipt.result.accepted && receipt.result.entityId == 1);
    receiptReader.finish();
    int cancelled = 0;
    CHECK(advmatch_exchange_cancel(exchange, request, &cancelled) == -1);

    const std::uint8_t bad[] = {1, 0, 255};
    CHECK(advmatch_exchange_submit(exchange, bad, sizeof(bad), &rejected) == -1);
    CHECK(advmatch_exchange_submit(exchange, nullptr, 4, &rejected) == -1);
    CHECK(advmatch_exchange_poll(exchange, 999999, nullptr, nullptr) == -1);
    request = submit(exchange, command);
    wait(exchange, request);
    std::uint64_t closeRequest = 0;
    CHECK(advmatch_exchange_close(exchange, &closeRequest) == 0);
    CHECK(advmatch_exchange_cancel(exchange, closeRequest, &cancelled) == 0 && cancelled == 0);
    CHECK(advmatch_exchange_submit(exchange, reinterpret_cast<const std::uint8_t*>(command.data()), command.size(), &rejected) == -1);
    wait(exchange, closeRequest);
    // Repeated close after releasing its old handle must still be pollable.
    CHECK(advmatch_exchange_close(exchange, &closeRequest) == 0);
    wait(exchange, closeRequest);
    CHECK(advmatch_exchange_destroy(exchange) == 0);
    CHECK(advmatch_exchange_destroy(exchange) == -1);
    CHECK(advmatch_exchange_submit(exchange, reinterpret_cast<const std::uint8_t*>(command.data()), command.size(), &rejected) == -1);
    config.maxPendingRequests = 64;
    config.ringBufferSize = 1;
    BinaryWriter batchConfig;
    batchConfig.number<std::uint16_t>(1);
    write(batchConfig, config);
    CHECK(advmatch_exchange_create(reinterpret_cast<const std::uint8_t*>(batchConfig.view().data()), batchConfig.view().size(), &exchange) == 0);
    BinaryWriter batch;
    batch.number<std::uint32_t>(32);
    for (int index = 0; index < 32; ++index)
    {
        batch.number<std::uint32_t>(static_cast<std::uint32_t>(command.size()));
        batch.bytes(command);
    }

    std::array<std::uint64_t, 32> ids{};
    CHECK(advmatch_exchange_await(exchange, 0) == 1);
    CHECK(advmatch_exchange_await(exchange, 101) == -1);
    CHECK(advmatch_exchange_submit_batch(exchange, reinterpret_cast<const std::uint8_t*>(batch.view().data()), batch.view().size(), ids.data(), ids.size()) == 0);
    CHECK(advmatch_exchange_await(exchange, 100) == 0);
    std::size_t completed = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (completed < ids.size())
    {
        std::uint8_t* data = nullptr;
        std::size_t size = 0;
        const auto status = advmatch_exchange_drain(exchange, 7, &data, &size);
        CHECK(std::chrono::steady_clock::now() < deadline);
        if (status == 1)
        {
            std::this_thread::yield();
            continue;
        }

        CHECK(status == 0);
        BinaryReader reader(std::as_bytes(std::span(data, size)));
        const auto count = reader.number<std::uint32_t>();
        CHECK(count <= 7);
        for (std::uint32_t index = 0; index < count; ++index)
        {
            CHECK(reader.number<std::uint64_t>() == ids[completed++]);
            CHECK(reader.number<std::int32_t>() == 0);
            const auto payload = reader.bytes(reader.number<std::uint32_t>());
            BinaryReader response(payload);
            CHECK(response.number<std::uint16_t>() == 1 && response.number<std::uint8_t>() == 1);
            CHECK(readCommandReceipt(response).result.accepted);
            response.finish();
        }

        reader.finish();
        advmatch_buffer_free(data);
    }

    CHECK(advmatch_request_release(exchange, ids[0]) == -1);
    const auto malformedBatch = batch.take();
    CHECK(advmatch_exchange_submit_batch(exchange, reinterpret_cast<const std::uint8_t*>(malformedBatch.data()), malformedBatch.size() - 1, ids.data(), ids.size()) == -1);
    CHECK(std::all_of(ids.begin(), ids.end(), [](auto id) { return id == 0; }));
    CHECK(advmatch_exchange_destroy(exchange) == 0);
    // Race cancellation/drain against deferred batch publication. A released
    // cancelled request must never be published a second time by its task.
    config.maxPendingRequests = 2048;
    BinaryWriter raceConfig;
    raceConfig.number<std::uint16_t>(1);
    write(raceConfig, config);
    CHECK(advmatch_exchange_create(reinterpret_cast<const std::uint8_t*>(raceConfig.view().data()), raceConfig.view().size(), &exchange) == 0);
    BinaryWriter fresh;
    fresh.number<std::uint16_t>(1);
    fresh.number<std::uint8_t>(0);
    fresh.bytes(encodeCommand(commands::CreateUser{}, 0));
    BinaryWriter raceBatch;
    raceBatch.number<std::uint32_t>(1024);
    for (int index = 0; index < 1024; ++index)
    {
        raceBatch.number<std::uint32_t>(static_cast<std::uint32_t>(fresh.view().size()));
        raceBatch.bytes(fresh.view());
    }

    for (int round = 0; round < 8; ++round)
    {
        std::array<std::uint64_t, 1024> tickets{};
        CHECK(advmatch_exchange_submit_batch(exchange, reinterpret_cast<const std::uint8_t*>(raceBatch.view().data()), raceBatch.view().size(), tickets.data(), tickets.size()) == 0);
        CHECK(advmatch_exchange_cancel(exchange, tickets.back(), &cancelled) == 0);
        CHECK(advmatch_request_release(exchange, tickets.front()) == 0);
        std::array<bool, 1024> seen{};
        std::size_t received = 0;
        const auto raceDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (received < 1023)
        {
            std::uint8_t* data = nullptr;
            std::size_t size = 0;
            const auto status = advmatch_exchange_drain(exchange, 1024, &data, &size);
            CHECK(std::chrono::steady_clock::now() < raceDeadline);
            if (status == 1)
            {
                CHECK(advmatch_exchange_await(exchange, 100) == 0);
                continue;
            }

            CHECK(status == 0);
            BinaryReader reader(std::as_bytes(std::span(data, size)));
            const auto count = reader.number<std::uint32_t>();
            for (std::uint32_t index = 0; index < count; ++index)
            {
                const auto id = reader.number<std::uint64_t>();
                const auto found = std::find(tickets.begin() + 1, tickets.end(), id);
                CHECK(found != tickets.end());
                const auto offset = static_cast<std::size_t>(found - tickets.begin());
                CHECK(!seen[offset]);
                seen[offset] = true;
                const auto responseStatus = reader.number<std::int32_t>();
                CHECK(responseStatus == 0 || (offset == 1023 && cancelled && responseStatus == -2));
                static_cast<void>(reader.bytes(reader.number<std::uint32_t>()));
                ++received;
            }

            reader.finish();
            advmatch_buffer_free(data);
        }
    }

    CHECK(advmatch_exchange_close(exchange, &closeRequest) == 0);
    wait(exchange, closeRequest);
    std::uint8_t* empty = nullptr;
    std::size_t emptySize = 0;
    CHECK(advmatch_exchange_drain(exchange, 1024, &empty, &emptySize) == 1);
    CHECK(advmatch_exchange_destroy(exchange) == 0);
    advmatch_buffer_free(nullptr);
    std::puts("C ABI binding tests passed");
}
