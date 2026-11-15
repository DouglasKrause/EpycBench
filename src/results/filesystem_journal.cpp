#include "results/filesystem_journal.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <new>

namespace epycbench::results::filesystem_journal {
struct Writer::Data {
    event_profile::Transaction transaction;
    std::optional<file::File> file;
    Limits limits;
    Durability durability;
    Receipt receipt;
};
struct Recovery::Data {
    event_profile::Transaction transaction;
    std::optional<file::File> file;
    Receipt receipt;
};
auto detail::Access::transaction(const Writer& value) noexcept -> const event_profile::Transaction* {
    return value.data_ ? &value.data_->transaction : nullptr;
}
auto detail::Access::transaction(const Recovery& value) noexcept -> const event_profile::Transaction* {
    return value.data_ ? &value.data_->transaction : nullptr;
}
namespace {
constexpr std::string_view suffix = ".epycbench.journal.jsonseq";
struct Name {
    std::array<char, 64> bytes{};
    std::size_t size{};
    auto view() const noexcept -> std::string_view { return {bytes.data(), size}; }
};
auto name(const journal::RunId& run) -> Name {
    Name value;
    const auto& id = run.text();
    value.size = id.size() + suffix.size();
    std::copy(id.begin(), id.end(), value.bytes.begin());
    std::copy(suffix.begin(), suffix.end(),
              value.bytes.begin() + static_cast<std::ptrdiff_t>(id.size()));
    return value;
}
auto failure(Error e) -> Failure { return {e, {}, {}}; }
auto failure(file::Failure e) -> Failure { return {Error::io, {}, e}; }
auto failure(event_profile::Error e) -> Failure { return {Error::semantic, e, {}}; }
void failed(Receipt& receipt, Failure value) noexcept {
    if (!receipt.failure)
        receipt.failure = value;
    receipt.state = State::failed;
}
auto calculate(std::size_t fixed, std::size_t chunk, event_profile::Limits semantic, Limits limits,
               std::size_t leaf) -> std::expected<std::size_t, Failure> {
    auto lower = file::File::storage(limits.file, leaf);
    if (!lower)
        return std::unexpected{failure(lower.error())};
    std::size_t total{};
    for (const auto amount : {fixed, sizeof(Name), *lower, chunk, semantic.simultaneous_bytes}) {
        if (amount > SIZE_MAX - total)
            return std::unexpected{failure(Error::overflow)};
        total += amount;
    }
    return total;
}
auto cleanup(auto& d) noexcept -> CloseReport {
    if (d.receipt.cleanup)
        return *d.receipt.cleanup;
    CloseReport out;
    out.primary = d.receipt.failure;
    if (d.file) {
        const auto result = d.file->close();
        out.attempts = result.attempts;
        for (std::size_t i{}; i < result.count; ++i) {
            if (!out.primary)
                out.primary =
                    failure(file::Failure{result.issues[i], {}, d.file->progress().created});
            else
                out.secondary[out.count++] = result.issues[i];
        }
    }
    if (!d.receipt.failure && out.primary)
        d.receipt.failure = out.primary;
    d.receipt.cleanup = out;
    d.receipt.state = State::closed;
    return out;
}
void update_semantic(auto& d, std::size_t bytes) noexcept {
    const auto summary = d.transaction.summary();
    d.receipt.semantic = {summary.records, summary.digest, bytes};
}
auto synchronize(Writer::Data& d) -> std::expected<void, Failure> {
    auto result = d.file->sync_file();
    if (!result) {
        d.receipt.persistence_uncertain = true;
        failed(d.receipt, failure(result.error()));
        return std::unexpected{*d.receipt.failure};
    }
    d.receipt.synchronized = d.receipt.complete;
    return {};
}
auto append_to(Writer::Data& d, const auto& input) -> std::expected<Receipt, Failure> {
    if (d.receipt.state != State::open)
        return std::unexpected{failure(Error::state)};
    try {
        auto frame = d.transaction.append(input);
        if (!frame) {
            failed(d.receipt, failure(frame.error()));
            return std::unexpected{*d.receipt.failure};
        }
        if (frame->size() > SIZE_MAX - d.receipt.semantic.bytes) {
            failed(d.receipt, failure(Error::overflow));
            return std::unexpected{*d.receipt.failure};
        }
        update_semantic(d, d.receipt.semantic.bytes + frame->size());
        if (d.receipt.complete.bytes > d.limits.file.file_bytes ||
            frame->size() > d.limits.file.file_bytes - d.receipt.complete.bytes) {
            failed(d.receipt, failure(Error::capacity));
            return std::unexpected{*d.receipt.failure};
        }
        std::size_t offset{};
        while (offset < frame->size()) {
            const auto rest =
                std::as_bytes(std::span{frame->data() + offset, frame->size() - offset});
            auto sent = d.file->write_some(rest);
            d.receipt.written_bytes = d.file->progress().bytes_written;
            d.receipt.partial_frame_bytes = d.receipt.written_bytes - d.receipt.complete.bytes;
            if (!sent) {
                failed(d.receipt, failure(sent.error()));
                return std::unexpected{*d.receipt.failure};
            }
            offset += *sent;
        }
        d.receipt.complete = d.receipt.semantic;
        d.receipt.partial_frame_bytes = 0U;
        const auto kind = d.transaction.records().back().kind;
        if (d.durability == Durability::persistent_block &&
            (kind == event_profile::Kind::block_closed ||
             kind == event_profile::Kind::run_closed)) {
            auto result = synchronize(d);
            if (!result)
                return std::unexpected{result.error()};
        }
        return d.receipt;
    } catch (...) {
        failed(d.receipt, failure(Error::allocation));
        throw;
    }
}
} // namespace
Writer::Writer(std::unique_ptr<Data> value) noexcept : data_(std::move(value)) {}
Writer::~Writer() = default;
Writer::Writer(Writer&&) noexcept = default;
auto Writer::operator=(Writer&&) noexcept -> Writer& = default;
Recovery::Recovery(std::unique_ptr<Data> value) noexcept : data_(std::move(value)) {}
Recovery::~Recovery() = default;
Recovery::Recovery(Recovery&&) noexcept = default;
auto Recovery::operator=(Recovery&&) noexcept -> Recovery& = default;
auto Writer::storage(event_profile::Limits s, Limits l, std::size_t leaf)
    -> std::expected<std::size_t, Failure> {
    return calculate(sizeof(Data), 0U, s, l, leaf);
}
auto Recovery::storage(event_profile::Limits s, Limits l, std::size_t leaf)
    -> std::expected<std::size_t, Failure> {
    return calculate(sizeof(Data), l.file.transfer_bytes, s, l, leaf);
}
auto detail::Access::create(std::string_view root, const journal::RunId& run,
                            event_profile::Limits semantic, Limits limits, Durability profile,
                            file::detail::Operations operations) -> std::expected<Writer, Failure> {
    if (profile != Durability::process && profile != Durability::persistent_block)
        return std::unexpected{failure(Error::limits)};
    const auto leaf = name(run);
    auto stock = Writer::storage(semantic, limits, leaf.size);
    if (!stock)
        return std::unexpected{stock.error()};
    if (*stock > limits.working_bytes)
        return std::unexpected{failure(Error::capacity)};
    auto transaction = event_profile::Transaction::create(run, semantic);
    if (!transaction)
        return std::unexpected{failure(transaction.error())};
    auto data = std::make_unique<Writer::Data>(
        Writer::Data{std::move(*transaction), {}, limits, profile, {}});
    data->receipt.state = State::open;
    auto opened = file::detail::Access::open(root, leaf.view(), limits.file, true, operations);
    if (!opened)
        return std::unexpected{failure(opened.error())};
    data->file.emplace(std::move(*opened));
    if (profile == Durability::persistent_block) {
        auto result = synchronize(*data);
        if (result) {
            auto directory = data->file->sync_directory();
            if (!directory) {
                data->receipt.persistence_uncertain = true;
                failed(data->receipt, failure(directory.error()));
            } else
                data->receipt.directory_synchronized = true;
        }
        if (data->receipt.failure) {
            auto error = *data->receipt.failure;
            const auto report = cleanup(*data);
            if (error.io) {
                error.io->cleanup.count = report.count;
                error.io->cleanup.attempts = report.attempts;
                error.io->cleanup.issues = report.secondary;
            }
            return std::unexpected{error};
        }
    }
    return Writer{std::move(data)};
}
auto Writer::create(std::string_view root, const journal::RunId& run,
                    event_profile::Limits semantic, Limits limits, Durability profile)
    -> std::expected<Writer, Failure> {
    return detail::Access::create(root, run, semantic, limits, profile,
                                  file::detail::production_operations());
}
auto Writer::append(const event_profile::Input& input) -> std::expected<Receipt, Failure> {
    if (!data_)
        return std::unexpected{failure(Error::state)};
    return append_to(*data_, input);
}
auto Writer::append(const event_profile::SuppliedEvent& input) -> std::expected<Receipt, Failure> {
    if (!data_)
        return std::unexpected{failure(Error::state)};
    return append_to(*data_, input);
}
auto Writer::seal() -> std::expected<Receipt, Failure> {
    if (!data_ || data_->receipt.state != State::open)
        return std::unexpected{failure(Error::state)};
    if (data_->durability == Durability::persistent_block) {
        auto result = synchronize(*data_);
        if (!result)
            return std::unexpected{result.error()};
    }
    data_->receipt.state = State::sealed;
    return data_->receipt;
}
auto Writer::close() noexcept -> CloseReport { return data_ ? cleanup(*data_) : CloseReport{}; }
auto Writer::receipt() const noexcept -> Receipt { return data_ ? data_->receipt : Receipt{}; }
auto Writer::records() const noexcept -> std::span<const event_profile::Record> {
    return data_ ? data_->transaction.records() : std::span<const event_profile::Record>{};
}
auto detail::Access::read(std::string_view root, const journal::RunId& run,
                          event_profile::Limits semantic, Limits limits,
                          file::detail::Operations operations) -> std::expected<Recovery, Failure> {
    const auto leaf = name(run);
    auto stock = Recovery::storage(semantic, limits, leaf.size);
    if (!stock)
        return std::unexpected{stock.error()};
    if (*stock > limits.working_bytes)
        return std::unexpected{failure(Error::capacity)};
    if (limits.file.transfer_bytes == 0U)
        return std::unexpected{failure(Error::limits)};
    auto transaction = event_profile::Transaction::create(run, semantic);
    if (!transaction)
        return std::unexpected{failure(transaction.error())};
    auto data = std::make_unique<Recovery::Data>(Recovery::Data{std::move(*transaction), {}, {}});
    data->receipt.state = State::open;
    auto buffer = std::make_unique<std::byte[]>(limits.file.transfer_bytes);
    auto opened = file::detail::Access::open(root, leaf.view(), limits.file, false, operations);
    if (!opened)
        return std::unexpected{failure(opened.error())};
    data->file.emplace(std::move(*opened));
    auto& d = *data;
    std::size_t read_bytes{}, verified_bytes{};
    try {
        const auto size = d.file->progress().initial_size;
        while (read_bytes < size && !d.receipt.failure) {
            auto received = d.file->read_some(
                {buffer.get(), std::min(limits.file.transfer_bytes, size - read_bytes)});
            if (!received) {
                failed(d.receipt, failure(received.error()));
                break;
            }
            if (*received == 0U) {
                failed(d.receipt, failure(Error::premature_eof));
                break;
            }
            read_bytes += *received;
            std::size_t offset{};
            while (offset < *received) {
                const auto text = std::string_view{
                    reinterpret_cast<const char*>(buffer.get() + offset), *received - offset};
                auto feed = d.transaction.feed(text);
                if (!feed) {
                    failed(d.receipt, failure(feed.error()));
                    break;
                }
                if (feed->consumed == 0U || feed->consumed > text.size()) {
                    failed(d.receipt, failure(Error::semantic));
                    break;
                }
                offset += feed->consumed;
                verified_bytes += feed->consumed;
                if (feed->accepted) {
                    update_semantic(d, verified_bytes);
                    d.receipt.complete = d.receipt.semantic;
                }
            }
        }
        if (!d.receipt.failure) {
            auto probe = d.file->read_some({buffer.get(), 1U});
            if (!probe)
                failed(d.receipt, failure(probe.error()));
            else if (*probe)
                failed(d.receipt, failure(Error::mutation));
            else {
                auto snapshot = d.file->verify_snapshot();
                if (!snapshot)
                    failed(d.receipt, failure(snapshot.error()));
                else {
                    d.receipt.file_eof = true;
                    auto end = d.transaction.finish();
                    if (!end)
                        failed(d.receipt, failure(end.error()));
                    else {
                        d.receipt.recovery = *end;
                        d.receipt.state = State::sealed;
                    }
                }
            }
        }
    } catch (const std::bad_alloc&) {
        failed(d.receipt, failure(Error::allocation));
    }
    d.receipt.recovery = d.transaction.summary();
    return Recovery{std::move(data)};
}
auto Recovery::read(std::string_view root, const journal::RunId& run,
                    event_profile::Limits semantic, Limits limits)
    -> std::expected<Recovery, Failure> {
    return detail::Access::read(root, run, semantic, limits, file::detail::production_operations());
}
auto Recovery::close() noexcept -> CloseReport { return data_ ? cleanup(*data_) : CloseReport{}; }
auto Recovery::receipt() const noexcept -> Receipt { return data_ ? data_->receipt : Receipt{}; }
auto Recovery::records() const noexcept -> std::span<const event_profile::Record> {
    return data_ ? data_->transaction.records() : std::span<const event_profile::Record>{};
}
} // namespace epycbench::results::filesystem_journal
