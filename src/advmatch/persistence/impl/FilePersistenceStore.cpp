#include "FilePersistenceStore.h"
#include "../codec/BinaryCodec.h"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <limits>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <io.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace advmatch
{
    namespace
    {
        constexpr std::uint32_t JournalMagic = 0x574D4441;
        constexpr std::uint32_t SnapshotMagic = 0x534D4441;
        constexpr std::uint16_t FormatVersion = 1;
        constexpr std::size_t JournalHeaderSize = 24;
        constexpr std::size_t SnapshotHeaderSize = 28;

        FILE* openFile(const std::filesystem::path& path, bool create)
        {
#ifdef _WIN32
            return _wfopen(path.c_str(), create ? L"wb+" : L"rb+");
#else
            return std::fopen(path.c_str(), create ? "wb+" : "rb+");
#endif
        }

        void flushFile(FILE* file)
        {
            persistenceCheck(std::fflush(file) == 0, "file flush failed", PersistenceFailure::Io);
#ifdef _WIN32
            persistenceCheck(_commit(_fileno(file)) == 0, "file sync failed", PersistenceFailure::Io);
#else
            persistenceCheck(::fsync(fileno(file)) == 0, "file sync failed", PersistenceFailure::Io);
#endif
        }

        void seekFile(FILE* file, std::uint64_t position)
        {
            persistenceCheck(position <= std::uint64_t(std::numeric_limits<std::int64_t>::max()), "file offset overflow");
#ifdef _WIN32
            const auto result = _fseeki64(file, static_cast<std::int64_t>(position), SEEK_SET);
#else
            const auto result = fseeko(file, static_cast<off_t>(position), SEEK_SET);
#endif
            persistenceCheck(result == 0, "file seek failed", PersistenceFailure::Io);
        }

        void truncateFile(FILE* file, std::uint64_t position)
        {
            persistenceCheck(std::fflush(file) == 0, "file flush failed", PersistenceFailure::Io);
#ifdef _WIN32
            const auto result = _chsize_s(_fileno(file), position);
#else
            const auto result = ftruncate(fileno(file), static_cast<off_t>(position));
#endif
            persistenceCheck(result == 0, "file truncate failed", PersistenceFailure::Io);
            std::clearerr(file);
            seekFile(file, position);
            flushFile(file);
        }

        void writeFile(FILE* file, std::span<const std::byte> bytes)
        {
            persistenceCheck(std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size(), "file write failed", PersistenceFailure::Io);
        }

        void publishFile(const std::filesystem::path& temporary, const std::filesystem::path& destination)
        {
#ifdef _WIN32
            persistenceCheck(MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0, "snapshot publication failed", PersistenceFailure::Io);
#else
            persistenceCheck(::rename(temporary.c_str(), destination.c_str()) == 0, "snapshot publication failed", PersistenceFailure::Io);
            const auto descriptor = ::open(destination.parent_path().c_str(), O_RDONLY | O_DIRECTORY);
            persistenceCheck(descriptor >= 0, "directory open failed", PersistenceFailure::Io);
            const auto result = ::fsync(descriptor);
            ::close(descriptor);
            persistenceCheck(result == 0, "directory sync failed", PersistenceFailure::Io);
#endif
        }
    }

    struct FilePersistenceStore::Impl
    {
        PersistenceConfig config;
        FILE* journal = nullptr;
        std::uint64_t journalBytes = 0;
        CommandSequence highestRecord = 0;
        CommandSequence publishedThrough = 0;
#ifdef _WIN32
        HANDLE lock = INVALID_HANDLE_VALUE;
#else
        int lock = -1;
#endif

        explicit Impl(PersistenceConfig config)
            : config(std::move(config))
        {
        }

        ~Impl()
        {
            if (this->journal != nullptr)
            {
                std::fclose(this->journal);
            }

#ifdef _WIN32
            if (this->lock != INVALID_HANDLE_VALUE)
            {
                CloseHandle(this->lock);
            }
#else
            if (this->lock >= 0)
            {
                ::close(this->lock);
            }
#endif
        }

        std::vector<std::pair<CommandSequence, std::filesystem::path>> snapshots() const
        {
            std::vector<std::pair<CommandSequence, std::filesystem::path>> entries;
            for (const auto& entry : std::filesystem::directory_iterator(this->config.directory))
            {
                const std::string name = entry.path().filename().string();
                if (!name.starts_with("snapshot-") || !name.ends_with(".bin"))
                {
                    continue;
                }

                const auto digits = std::string_view(name).substr(9, name.size() - 13);
                CommandSequence sequence = 0;
                const auto result = std::from_chars(digits.data(), digits.data() + digits.size(), sequence);
                persistenceCheck(result.ec == std::errc{} && result.ptr == digits.data() + digits.size(), "invalid snapshot filename");
                entries.emplace_back(sequence, entry.path());
            }

            std::sort(entries.begin(), entries.end());
            return entries;
        }
    };

    FilePersistenceStore::FilePersistenceStore(const PersistenceConfig& config)
        : impl(std::make_unique<Impl>(config))
    {
        persistenceCheck(!config.directory.empty() && config.retainedCheckpoints > 0 && config.maxRecordBytes > 0 && config.maxRecordBytes <= std::numeric_limits<std::uint32_t>::max(), "invalid file persistence configuration", PersistenceFailure::InvalidConfiguration);
        std::filesystem::create_directories(config.directory);
        const auto lockPath = config.directory / "writer.lock";
#ifdef _WIN32
        this->impl->lock = CreateFileW(lockPath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        const auto lockError = GetLastError();
        persistenceCheck(this->impl->lock != INVALID_HANDLE_VALUE, "cannot acquire persistence writer lock", lockError == ERROR_SHARING_VIOLATION ? PersistenceFailure::Locked : PersistenceFailure::Io);
#else
        this->impl->lock = ::open(lockPath.c_str(), O_CREAT | O_RDWR, 0600);
        persistenceCheck(this->impl->lock >= 0, "cannot open persistence writer lock", PersistenceFailure::Io);
        persistenceCheck(flock(this->impl->lock, LOCK_EX | LOCK_NB) == 0, "cannot acquire persistence writer lock", PersistenceFailure::Locked);
#endif
        const auto journalPath = config.directory / "journal.wal";
        const bool exists = std::filesystem::exists(journalPath);
        this->impl->journal = openFile(journalPath, !exists);
        persistenceCheck(this->impl->journal != nullptr, "cannot open journal", PersistenceFailure::Io);
        this->impl->journalBytes = std::filesystem::file_size(journalPath);
        seekFile(this->impl->journal, this->impl->journalBytes);
        // Make creation durable before accepting any command.
        if (!exists)
        {
            flushFile(this->impl->journal);
#ifndef _WIN32
            const auto descriptor = ::open(config.directory.c_str(), O_RDONLY | O_DIRECTORY);
            persistenceCheck(descriptor >= 0, "directory open failed", PersistenceFailure::Io);
            const auto result = ::fsync(descriptor);
            ::close(descriptor);
            persistenceCheck(result == 0, "directory sync failed", PersistenceFailure::Io);
#endif
        }
    }

    FilePersistenceStore::~FilePersistenceStore() = default;

    std::optional<StoredSnapshot> FilePersistenceStore::loadSnapshot()
    {
        const auto entries = this->impl->snapshots();
        const auto headPath = this->impl->config.directory / "checkpoint.head";
        CommandSequence required = 0;
        if (std::filesystem::exists(headPath))
        {
            persistenceCheck(std::filesystem::file_size(headPath) == 20, "invalid checkpoint manifest size");
            std::unique_ptr<FILE, decltype(&std::fclose)> file(openFile(headPath, false), &std::fclose);
            persistenceCheck(file != nullptr, "cannot read checkpoint manifest", PersistenceFailure::Io);
            std::array<std::byte, 20> header{};
            persistenceCheck(std::fread(header.data(), 1, header.size(), file.get()) == header.size(), "truncated checkpoint manifest");
            BinaryReader reader(header);
            persistenceCheck(reader.number<std::uint32_t>() == SnapshotMagic, "invalid checkpoint manifest magic");
            persistenceCheck(reader.number<std::uint16_t>() == FormatVersion, "unsupported checkpoint manifest version", PersistenceFailure::UnsupportedVersion);
            persistenceCheck(reader.number<std::uint16_t>() == 0, "invalid checkpoint manifest flags");
            required = reader.number<CommandSequence>();
            persistenceCheck(reader.number<std::uint32_t>() == crc32c(std::span(header).first(16)), "checkpoint manifest checksum mismatch");
        }

        if (entries.empty())
        {
            persistenceCheck(required == 0, "required checkpoint is missing");
            return std::nullopt;
        }

        const auto& [sequence, path] = entries.back();
        persistenceCheck(sequence >= required, "latest required checkpoint is missing");
        const auto size = std::filesystem::file_size(path);
        persistenceCheck(size >= SnapshotHeaderSize && size - SnapshotHeaderSize <= this->impl->config.maxSnapshotBytes && size <= std::numeric_limits<std::size_t>::max(), "invalid snapshot size");
        std::unique_ptr<FILE, decltype(&std::fclose)> file(openFile(path, false), &std::fclose);
        persistenceCheck(file != nullptr, "cannot read snapshot", PersistenceFailure::Io);
        std::array<std::byte, SnapshotHeaderSize> header{};
        persistenceCheck(std::fread(header.data(), 1, header.size(), file.get()) == header.size(), "truncated snapshot header");
        BinaryReader reader(header);
        persistenceCheck(reader.number<std::uint32_t>() == SnapshotMagic, "invalid snapshot magic");
        persistenceCheck(reader.number<std::uint16_t>() == FormatVersion, "unsupported snapshot version", PersistenceFailure::UnsupportedVersion);
        persistenceCheck(reader.number<std::uint16_t>() == 0, "invalid snapshot flags");
        persistenceCheck(reader.number<CommandSequence>() == sequence, "snapshot sequence mismatch");
        persistenceCheck(reader.number<std::uint64_t>() == size - SnapshotHeaderSize, "snapshot size mismatch");
        const auto checksum = reader.number<std::uint32_t>();
        StoredSnapshot snapshot{sequence, std::vector<std::byte>(static_cast<std::size_t>(size - SnapshotHeaderSize))};
        persistenceCheck(std::fread(snapshot.payload.data(), 1, snapshot.payload.size(), file.get()) == snapshot.payload.size(), "truncated snapshot");
        persistenceCheck(crc32c(std::span(header).first(SnapshotHeaderSize - 4), snapshot.payload) == checksum, "snapshot checksum mismatch");
        this->impl->publishedThrough = sequence;
        return snapshot;
    }

    void FilePersistenceStore::replay(const std::function<void(const JournalRecord&)>& visitor)
    {
        seekFile(this->impl->journal, 0);
        std::uint64_t offset = 0;
        for (;;)
        {
            std::array<std::byte, JournalHeaderSize> header{};
            const auto read = std::fread(header.data(), 1, header.size(), this->impl->journal);
            persistenceCheck(std::ferror(this->impl->journal) == 0, "journal read failed", PersistenceFailure::Io);
            if (read == 0)
            {
                break;
            }

            auto incomplete = [&]
            {
                persistenceCheck(this->impl->config.tailRecovery == TailRecovery::TruncateIncomplete, "incomplete journal tail");
                truncateFile(this->impl->journal, offset);
            };
            if (read != header.size())
            {
                incomplete();
                break;
            }

            BinaryReader reader(header);
            persistenceCheck(reader.number<std::uint32_t>() == JournalMagic, "invalid journal magic");
            persistenceCheck(reader.number<std::uint16_t>() == FormatVersion, "unsupported journal version", PersistenceFailure::UnsupportedVersion);
            const auto type = reader.enumeration(JournalRecordType::Commit);
            persistenceCheck(type != static_cast<JournalRecordType>(0) && reader.number<std::uint8_t>() == 0, "invalid journal record flags/type");
            const auto sequence = reader.number<CommandSequence>();
            const auto length = reader.number<std::uint32_t>();
            const auto checksum = reader.number<std::uint32_t>();
            persistenceCheck(sequence != 0 && length <= this->impl->config.maxRecordBytes, "invalid journal record size/sequence");
            JournalRecord record{type, sequence, std::vector<std::byte>(length)};
            const auto payloadRead = std::fread(record.payload.data(), 1, length, this->impl->journal);
            persistenceCheck(std::ferror(this->impl->journal) == 0, "journal read failed", PersistenceFailure::Io);
            if (payloadRead != length)
            {
                incomplete();
                break;
            }

            persistenceCheck(crc32c(std::span(header).first(JournalHeaderSize - 4), record.payload) == checksum, "journal checksum mismatch");
            offset += header.size() + length;
            this->impl->highestRecord = std::max(this->impl->highestRecord, sequence);
            visitor(record);
        }

        std::clearerr(this->impl->journal);
        this->impl->journalBytes = offset;
        seekFile(this->impl->journal, offset);
    }

    void FilePersistenceStore::append(const JournalRecord& record)
    {
        persistenceCheck(record.sequence != 0 && record.payload.size() <= this->impl->config.maxRecordBytes && (record.type == JournalRecordType::Intent || record.type == JournalRecordType::Commit), "invalid journal record or configured limit exceeded", PersistenceFailure::InvalidConfiguration);
        BinaryWriter header;
        header.number(JournalMagic);
        header.number(FormatVersion);
        header.enumeration(record.type);
        header.number<std::uint8_t>(0);
        header.number(record.sequence);
        header.number<std::uint32_t>(static_cast<std::uint32_t>(record.payload.size()));
        header.number(crc32c(header.view(), record.payload));
        writeFile(this->impl->journal, header.view());
        writeFile(this->impl->journal, record.payload);
        this->impl->journalBytes += header.view().size() + record.payload.size();
        this->impl->highestRecord = std::max(this->impl->highestRecord, record.sequence);
    }

    void FilePersistenceStore::sync()
    {
        flushFile(this->impl->journal);
    }

    void FilePersistenceStore::publishSnapshot(CommandSequence sequence, std::span<const std::byte> payload)
    {
        persistenceCheck(sequence >= this->impl->publishedThrough && payload.size() <= this->impl->config.maxSnapshotBytes, "snapshot boundary/size violates configured limit", PersistenceFailure::InvalidConfiguration);
        const auto destination = this->impl->config.directory / ("snapshot-" + std::to_string(sequence) + ".bin");
        auto temporary = destination;
        temporary += ".tmp";
        BinaryWriter header;
        header.number(SnapshotMagic);
        header.number(FormatVersion);
        header.number<std::uint16_t>(0);
        header.number(sequence);
        header.number<std::uint64_t>(payload.size());
        header.number(crc32c(header.view(), payload));
        {
            std::unique_ptr<FILE, decltype(&std::fclose)> file(openFile(temporary, true), &std::fclose);
            persistenceCheck(file != nullptr, "cannot create snapshot", PersistenceFailure::Io);
            writeFile(file.get(), header.view());
            writeFile(file.get(), payload);
            flushFile(file.get());
        }

        publishFile(temporary, destination);
        // This small manifest prevents silent rollback when a newest checkpoint
        // is missing after its covered WAL has already been pruned.
        const auto headPath = this->impl->config.directory / "checkpoint.head";
        const auto temporaryHead = this->impl->config.directory / "checkpoint.head.tmp";
        BinaryWriter head;
        head.number(SnapshotMagic);
        head.number(FormatVersion);
        head.number<std::uint16_t>(0);
        head.number(sequence);
        head.number(crc32c(head.view()));
        {
            std::unique_ptr<FILE, decltype(&std::fclose)> file(openFile(temporaryHead, true), &std::fclose);
            persistenceCheck(file != nullptr, "cannot create checkpoint manifest", PersistenceFailure::Io);
            writeFile(file.get(), head.view());
            flushFile(file.get());
        }

        publishFile(temporaryHead, headPath);
        this->impl->publishedThrough = sequence;
        const auto entries = this->impl->snapshots();
        for (std::size_t index = this->impl->config.retainedCheckpoints; index < entries.size(); ++index)
        {
            std::filesystem::remove(entries[index - this->impl->config.retainedCheckpoints].second);
        }
    }

    void FilePersistenceStore::resetJournalThrough(CommandSequence sequence)
    {
        persistenceCheck(sequence <= this->impl->publishedThrough && this->impl->highestRecord <= sequence, "cannot discard uncovered journal records", PersistenceFailure::InvalidConfiguration);
        truncateFile(this->impl->journal, 0);
        this->impl->journalBytes = 0;
        this->impl->highestRecord = 0;
    }
}
