#pragma once

#include "epycbench/experiment/coordinator.hpp"

namespace epycbench::platform_linux::experiment_operations {
class Directory;
}

namespace epycbench::experiment {

class LedgerStore {
  public:
    // The exclusive directory owner must remain alive until this store is
    // retired. The caller's empty ledger changes only after successful replay.
    [[nodiscard]] static auto open(const platform_linux::experiment_operations::Directory&,
                                   std::string plan_digest, Ledger&)
        -> std::expected<LedgerStore, core::Error>;

    LedgerStore(LedgerStore&&) noexcept;
    auto operator=(LedgerStore&&) noexcept -> LedgerStore&;
    LedgerStore(const LedgerStore&) = delete;
    auto operator=(const LedgerStore&) -> LedgerStore& = delete;

    // The coordinator validates the transition before handing it to storage.
    // Open and replay again before persisting after a publication failure.
    [[nodiscard]] auto persist(const Entry&) -> std::expected<void, core::Error>;
    [[nodiscard]] auto head() const noexcept -> std::optional<core::Sha256Digest> { return previous_; }

  private:
    LedgerStore(const platform_linux::experiment_operations::Directory&, std::string,
                std::size_t maximum_entries) noexcept;

    const platform_linux::experiment_operations::Directory* directory_;
    std::string plan_digest_;
    std::size_t maximum_entries_;
    std::uint64_t next_sequence_{1};
    std::optional<core::Sha256Digest> previous_;
};

} // namespace epycbench::experiment
