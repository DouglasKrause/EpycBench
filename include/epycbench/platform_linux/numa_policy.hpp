#pragma once

#include "epycbench/core/error.hpp"
#include "epycbench/core/id_set.hpp"
#include "epycbench/platform_linux/memory_mapping.hpp"

#include <cstddef>
#include <expected>
#include <vector>

namespace epycbench::platform_linux {

namespace detail { class NumaAccess; }

class NativeNumaMask {
  public:
    [[nodiscard]] static auto from_node_set(const core::LinuxNumaNodeSet& nodes)
        -> std::expected<NativeNumaMask, core::Error>;

    [[nodiscard]] auto data() const noexcept -> const unsigned long*;
    [[nodiscard]] auto word_count() const noexcept -> std::size_t;
    [[nodiscard]] auto maxnode() const noexcept -> unsigned long;
    [[nodiscard]] auto contains(core::LinuxNumaNodeId node) const noexcept -> bool;

  private:
    NativeNumaMask(std::vector<unsigned long> words, unsigned long maxnode,
                   unsigned long native_bind_maxnode) noexcept;

    std::vector<unsigned long> words_;
    unsigned long maxnode_;
    unsigned long native_bind_maxnode_;

    friend class detail::NumaAccess;
};

[[nodiscard]] auto bind_memory(const AnonymousMapping& mapping, const NativeNumaMask& nodes)
    -> std::expected<void, core::Error>;

} // namespace epycbench::platform_linux
