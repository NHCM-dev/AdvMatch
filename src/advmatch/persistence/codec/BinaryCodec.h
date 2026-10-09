#ifndef ADVMATCH_BINARY_CODEC_H
#define ADVMATCH_BINARY_CODEC_H

#include "../model/PersistenceTypes.h"

#include <array>
#include <bit>
#include <cstring>
#include <limits>
#include <type_traits>
#include <utility>

namespace advmatch
{
    inline void persistenceCheck(bool condition, const char* message, PersistenceFailure failure = PersistenceFailure::CorruptData)
    {
        if (!condition)
        {
            throw PersistenceError(failure, message);
        }
    }

    class BinaryWriter
    {
        std::vector<std::byte> data;

    public:
        explicit BinaryWriter(std::size_t capacity = 128)
        {
            this->data.reserve(capacity);
        }

        template <typename Value>
        void number(Value value)
        {
            static_assert(std::is_integral_v<Value> && !std::is_same_v<Value, bool>);
            using Unsigned = std::make_unsigned_t<Value>;
            const auto bits = std::bit_cast<Unsigned>(value);
            const auto offset = this->data.size();
            this->data.resize(offset + sizeof(Value));
            if constexpr (std::endian::native == std::endian::little)
            {
                std::memcpy(this->data.data() + offset, &bits, sizeof(Value));
            }
            else
            {
                for (std::size_t index = 0; index < sizeof(Value); ++index)
                {
                    this->data[offset + index] = static_cast<std::byte>((bits >> (index * 8)) & 255);
                }
            }
        }

        void boolean(bool value)
        {
            this->number<std::uint8_t>(value);
        }

        template <typename Value>
        void enumeration(Value value)
        {
            this->number<std::uint8_t>(static_cast<std::uint8_t>(value));
        }

        void bytes(std::span<const std::byte> value)
        {
            this->data.insert(this->data.end(), value.begin(), value.end());
        }

        template <typename Value>
        void patchNumber(std::size_t offset, Value value)
        {
            static_assert(std::is_integral_v<Value> && !std::is_same_v<Value, bool>);
            persistenceCheck(offset <= this->data.size() && sizeof(Value) <= this->data.size() - offset, "invalid frame offset");
            using Unsigned = std::make_unsigned_t<Value>;
            const auto bits = std::bit_cast<Unsigned>(value);
            for (std::size_t index = 0; index < sizeof(Value); ++index)
            {
                this->data[offset + index] = static_cast<std::byte>((bits >> (index * 8)) & 255);
            }
        }

        void string(const std::string& value)
        {
            persistenceCheck(value.size() <= std::numeric_limits<std::uint32_t>::max(), "string too large", PersistenceFailure::InvalidConfiguration);
            this->number<std::uint32_t>(static_cast<std::uint32_t>(value.size()));
            this->bytes(std::as_bytes(std::span(value.data(), value.size())));
        }

        std::span<const std::byte> view() const noexcept
        {
            return this->data;
        }

        std::vector<std::byte> take() noexcept
        {
            return std::move(this->data);
        }
    };

    class BinaryReader
    {
        std::span<const std::byte> data;
        std::size_t offset = 0;

    public:
        explicit BinaryReader(std::span<const std::byte> data)
            : data(data)
        {
        }

        template <typename Value>
        Value number()
        {
            static_assert(std::is_integral_v<Value> && !std::is_same_v<Value, bool>);
            using Unsigned = std::make_unsigned_t<Value>;
            persistenceCheck(sizeof(Value) <= this->remaining(), "truncated integer");
            Unsigned value = 0;
            if constexpr (std::endian::native == std::endian::little)
            {
                std::memcpy(&value, this->data.data() + this->offset, sizeof(Value));
                this->offset += sizeof(Value);
            }
            else
            {
                for (std::size_t index = 0; index < sizeof(Value); ++index)
                {
                    value |= static_cast<Unsigned>(std::to_integer<unsigned>(this->data[this->offset++])) << (index * 8);
                }
            }

            return std::bit_cast<Value>(value);
        }

        bool boolean()
        {
            const auto value = this->number<std::uint8_t>();
            persistenceCheck(value <= 1, "invalid boolean");
            return value != 0;
        }

        template <typename Value>
        Value enumeration(Value maximum)
        {
            const auto value = this->number<std::uint8_t>();
            persistenceCheck(value <= static_cast<std::uint8_t>(maximum), "invalid enum");
            return static_cast<Value>(value);
        }

        std::span<const std::byte> bytes(std::size_t count)
        {
            persistenceCheck(count <= this->remaining(), "truncated payload");
            const auto result = this->data.subspan(this->offset, count);
            this->offset += count;
            return result;
        }

        std::string string()
        {
            const auto data = this->bytes(this->number<std::uint32_t>());
            return {reinterpret_cast<const char*>(data.data()), data.size()};
        }

        std::size_t count(std::size_t minimumBytes = 1)
        {
            const auto count = this->number<std::uint64_t>();
            persistenceCheck(minimumBytes != 0 && count <= this->remaining() / minimumBytes, "invalid collection size");
            return static_cast<std::size_t>(count);
        }

        std::size_t remaining() const noexcept
        {
            return this->data.size() - this->offset;
        }

        void finish() const
        {
            persistenceCheck(this->remaining() == 0, "unexpected trailing payload");
        }
    };

    inline std::uint32_t crc32c(std::span<const std::byte> first, std::span<const std::byte> second = {}) noexcept
    {
        static constexpr auto table = []
        {
            std::array<std::uint32_t, 256> values{};
            for (std::uint32_t index = 0; index < 256; ++index)
            {
                auto value = index;
                for (int bit = 0; bit < 8; ++bit)
                {
                    value = (value >> 1) ^ ((value & 1) ? 0x82f63b78U : 0);
                }

                values[index] = value;
            }

            return values;
        }();
        std::uint32_t value = ~std::uint32_t{0};
        for (const auto data : {first, second})
        {
            for (std::byte byte : data)
            {
                value = table[(value ^ std::to_integer<unsigned>(byte)) & 255] ^ (value >> 8);
            }
        }

        return ~value;
    }
}

#endif // ADVMATCH_BINARY_CODEC_H
