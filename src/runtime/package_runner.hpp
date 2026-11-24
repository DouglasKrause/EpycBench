#pragma once
namespace epycbench::runtime {
// Private controller-spawned entry point for package admission and observations.
[[nodiscard]] int serve_package_runner();
} // namespace epycbench::runtime
