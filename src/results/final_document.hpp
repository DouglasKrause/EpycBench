#pragma once
#include "epycbench/core/canonical_json.hpp"
#include "results/filesystem_journal.hpp"
#include "results/incremental_event_pair.hpp"
namespace epycbench::results::final_document {
enum class Error {
    state,
    limits,
    capacity,
    overflow,
    schema,
    canonical,
    digest,
    reference,
    association,
    calculation,
    missing_genesis,
    allocation,
    io
};
struct Failure {
    Error kind{};
    std::optional<event_profile::Error> event;
    std::optional<FinalCalculationFailure> calculation;
    std::optional<platform_linux::journal_file::Failure> file;
};
struct Limits {
    std::size_t input_bytes{}, output_bytes{}, nodes{}, depth{}, text_bytes{}, records{},
        experiments{}, pairs{}, blocks{}, observations{}, samples{}, references{}, work{},
        retained_bytes{}, simultaneous_bytes{};
    event_profile::Limits events;
    core::JcsLimits analysis;
};
struct Usage {
    std::size_t input_bytes{}, output_bytes{}, nodes{}, depth{}, text_bytes{}, records{},
        experiments{}, pairs{}, blocks{}, observations{}, samples{}, references{}, work{},
        retained_bytes{}, simultaneous_bytes{};
    auto operator==(const Usage&) const -> bool = default;
};
struct FinalizationFacts {
    journal::RunId id;
    journal::Timestamp timestamp;
};
struct Publication;
struct PublicationAccess;
class Validated {
  public:
    struct Data;
    ~Validated();
    Validated(Validated&&) noexcept;
    auto operator=(Validated&&) noexcept -> Validated&;
    Validated(const Validated&) = delete;
    auto operator=(const Validated&) -> Validated& = delete;
    [[nodiscard]] auto canonical() const noexcept -> std::string_view;
    [[nodiscard]] auto digest() const noexcept -> const core::Sha256Digest*;
    [[nodiscard]] auto usage() const noexcept -> Usage;
    [[nodiscard]] auto final_leaf() const noexcept -> std::string_view;

  private:
    std::unique_ptr<Data> data_;
    friend struct PublicationAccess;
    explicit Validated(std::unique_ptr<Data>) noexcept;
    friend auto validate(std::string_view, Limits) -> std::expected<Validated, Failure>;
    friend auto assemble(const filesystem_journal::Writer&, const FinalizationFacts&, Limits)
        -> std::expected<Validated, Failure>;
    friend auto assemble(const filesystem_journal::Recovery&, const FinalizationFacts&, Limits)
        -> std::expected<Validated, Failure>;
    friend auto publish(Validated&&, std::string_view, std::string_view,
                        platform_linux::journal_file::Limits, filesystem_journal::Durability)
        -> std::expected<Publication, Failure>;
};
[[nodiscard]] auto assemble(const filesystem_journal::Writer&, const FinalizationFacts&, Limits)
    -> std::expected<Validated, Failure>;
[[nodiscard]] auto assemble(const filesystem_journal::Recovery&, const FinalizationFacts&, Limits)
    -> std::expected<Validated, Failure>;
[[nodiscard]] auto validate(std::string_view, Limits) -> std::expected<Validated, Failure>;
enum class Residual { none, temporary, final, uncertain };
struct PublicationReceipt {
    bool temporary_created{}, complete_written{}, file_synced{}, rename_observed{},
        directory_synced{};
    Residual residual{Residual::none};
    std::optional<Failure> failure;
    platform_linux::journal_file::CloseReport cleanup;
};
struct Publication {
    Validated document;
    PublicationReceipt receipt;
};
// Private operation injection; it cannot construct a Validated value.
struct PublicationAccess {
    [[nodiscard]] static auto
    publish(Validated&&, std::string_view, std::string_view, platform_linux::journal_file::Limits,
            filesystem_journal::Durability, platform_linux::journal_file::detail::Operations)
        -> std::expected<Publication, Failure>;
};
[[nodiscard]] auto publish(Validated&&, std::string_view root, std::string_view final_leaf,
                           platform_linux::journal_file::Limits, filesystem_journal::Durability)
    -> std::expected<Publication, Failure>;
} // namespace epycbench::results::final_document
