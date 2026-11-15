#include "results/build_manifest_owner.hpp"
#include "core/json_ownership.hpp"
#include "epycbench/core/canonical_json.hpp"
#include <array>
#include <limits>
#include <vector>

namespace epycbench::results::build_manifest {
namespace {
constexpr std::array<std::string_view, 6> feature_names{"linux",      "cxx23", "testing",
                                                        "asan_ubsan", "tsan",  "native_reference"};
using Fields = std::array<std::string_view, 18>;
auto fields(Input v) noexcept -> Fields {
    return {v.project_version,
            v.git.revision,
            v.compiler.id,
            v.compiler.version,
            v.compiler.target,
            v.compiler.cxx_standard,
            v.compiler.flags,
            v.linker.id,
            v.linker.version,
            v.preset,
            v.dependencies.hwloc,
            v.dependencies.libnuma,
            v.dependencies.libfyaml,
            v.dependencies.cli11,
            v.dependencies.nlohmann_json,
            v.dependencies.fmt,
            v.dependencies.catch2,
            v.build_identity};
}
struct Rejected {
    std::string_view code;
};
void add(std::size_t& value, std::size_t amount, std::size_t limit) {
    if (amount > limit - value)
        throw Rejected{"capacity"};
    value += amount;
}
auto escaped_size(std::string_view text, std::size_t limit) -> std::size_t {
    std::size_t result = 0;
    add(result, 2, limit);
    for (std::size_t index = 0; index < text.size();) {
        const auto first = static_cast<unsigned char>(text[index]);
        if (first == 0)
            throw Rejected{"invalid_field"};
        std::size_t length = 1;
        std::uint32_t scalar = first;
        std::uint32_t minimum = 0;
        if (first >= 0xc2 && first <= 0xdf) {
            length = 2;
            scalar = first & 0x1fU;
            minimum = 0x80;
        } else if (first >= 0xe0 && first <= 0xef) {
            length = 3;
            scalar = first & 0x0fU;
            minimum = 0x800;
        } else if (first >= 0xf0 && first <= 0xf4) {
            length = 4;
            scalar = first & 0x07U;
            minimum = 0x10000;
        } else if (first > 0x7f)
            throw Rejected{"invalid_field"};
        if (length > text.size() - index)
            throw Rejected{"invalid_field"};
        for (std::size_t n = 1; n < length; ++n) {
            const auto byte = static_cast<unsigned char>(text[index + n]);
            if ((byte & 0xc0U) != 0x80U)
                throw Rejected{"invalid_field"};
            scalar = (scalar << 6U) | (byte & 0x3fU);
        }
        if (scalar < minimum || scalar > 0x10ffffU || (scalar >= 0xd800U && scalar <= 0xdfffU))
            throw Rejected{"invalid_field"};
        const bool short_escape = scalar == '"' || scalar == '\\' || scalar == '\b' ||
                                  scalar == '\t' || scalar == '\n' || scalar == '\f' ||
                                  scalar == '\r';
        add(result, short_escape ? 2 : (scalar < 0x20U ? 6 : length), limit);
        index += length;
    }
    return result;
}
auto error(std::string_view code) -> core::Error {
    return {.category = core::ErrorCategory::results,
            .code = core::StableErrorCode{"results.build_manifest." + std::string{code}},
            .message = "Build manifest admission rejected",
            .operation = "retain_build_manifest",
            .os_error = std::nullopt,
            .context = nlohmann::json::object()};
}
auto hex_matches(const core::Sha256Digest& digest, std::string_view hex) noexcept -> bool {
    constexpr std::string_view digits = "0123456789abcdef";
    for (std::size_t i = 0; i < 32; ++i) {
        const auto byte = std::to_integer<unsigned>(digest.bytes()[i]);
        if (hex[2 * i] != digits[byte >> 4U] || hex[2 * i + 1] != digits[byte & 15U])
            return false;
    }
    return true;
}
// Fixed keys/punctuation with every supplied string and feature array omitted.
constexpr std::string_view empty_projection =
    R"({"build_identity":"","compiler":{"cxx_standard":"","flags":"","id":"","target":"","version":""},"dependencies":{"CLI11":"","Catch2":"","fmt":"","hwloc":"","libfyaml":"","libnuma":"","nlohmann-json":""},"enabled_features":[],"git":{"dirty":false,"revision":""},"linker":{"id":"","version":""},"preset":"","project_version":""})";
} // namespace
struct Owner::Storage {
    std::array<std::string, 18> fields;
    bool dirty{};
    std::vector<Feature> features;
    std::string canonical;
    ManifestDigest digest;
    Usage usage;
    explicit Storage(ManifestDigest value) : digest(value) {}
};
Owner::Owner(std::unique_ptr<Storage> value) : storage_(std::move(value)) {}
Owner::Owner(const Owner& other)
    : storage_(other.storage_ ? std::make_unique<Storage>(*other.storage_) : nullptr) {}
auto Owner::operator=(const Owner& other) -> Owner& {
    Owner copy{other};
    storage_.swap(copy.storage_);
    return *this;
}
Owner::Owner(Owner&&) noexcept = default;
auto Owner::operator=(Owner&&) noexcept -> Owner& = default;
Owner::~Owner() = default;
auto Owner::input() const noexcept -> std::optional<Input> {
    if (!storage_)
        return std::nullopt;
    const auto& f = storage_->fields;
    return Input{f[0],
                 {f[1], storage_->dirty},
                 {f[2], f[3], f[4], f[5], f[6]},
                 {f[7], f[8]},
                 f[9],
                 storage_->features,
                 {f[10], f[11], f[12], f[13], f[14], f[15], f[16]},
                 f[17]};
}
auto Owner::canonical() const noexcept -> std::string_view {
    return storage_ ? std::string_view{storage_->canonical} : std::string_view{};
}
auto Owner::digest() const noexcept -> const ManifestDigest* {
    return storage_ ? &storage_->digest : nullptr;
}
auto Owner::usage() const noexcept -> Usage { return storage_ ? storage_->usage : Usage{}; }
auto retain(Input in, Limits limits) -> std::expected<Owner, core::Error> {
    try {
        const auto maximum = std::string{}.max_size();
        for (auto n : {limits.features, limits.field_bytes, limits.string_bytes,
                       limits.preimage_bytes, limits.canonical_bytes, limits.total_bytes})
            if (n > maximum)
                throw Rejected{"invalid_limits"};
        if (in.enabled_features.size() > limits.features)
            throw Rejected{"capacity"};
        if (in.enabled_features.size() < 2 || in.enabled_features.size() > 6 ||
            in.enabled_features[0] != Feature::linux_platform ||
            in.enabled_features[1] != Feature::cxx23)
            throw Rejected{"invalid_features"};
        int previous = -1;
        std::array<bool, 4> optional{};
        for (auto feature : in.enabled_features) {
            const int index = static_cast<int>(feature);
            if (index < 0 || index >= 6 || index <= previous)
                throw Rejected{"invalid_features"};
            if (index >= 2)
                optional[static_cast<std::size_t>(index - 2)] = true;
            previous = index;
        }
        const auto f = fields(in);
        Usage usage{.features = in.enabled_features.size()};
        std::size_t canonical = empty_projection.size() - (in.git.dirty ? 1U : 0U);
        if (canonical > limits.canonical_bytes)
            throw Rejected{"capacity"};
        for (std::size_t i = 0; i < f.size(); ++i) {
            if (f[i].empty() && i != 6)
                throw Rejected{"invalid_field"};
            if (f[i].size() > limits.field_bytes)
                throw Rejected{"capacity"};
            add(usage.string_bytes, f[i].size(), limits.string_bytes);
            const auto encoded = escaped_size(f[i], maximum);
            add(canonical, encoded - 2, limits.canonical_bytes);
        }
        if (f[5] != "C++23" || f[17].size() != 64)
            throw Rejected{"invalid_field"};
        for (char ch : f[17])
            if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f')))
                throw Rejected{"invalid_field"};
        for (std::size_t i = 0; i < in.enabled_features.size(); ++i) {
            const auto index = static_cast<std::size_t>(in.enabled_features[i]);
            add(canonical, feature_names[index].size() + 2 + (i != 0 ? 1U : 0U),
                limits.canonical_bytes);
        }
        usage.canonical_bytes = canonical;
        std::array<std::string_view, 21> preimage{f[0],
                                                  f[1],
                                                  in.git.dirty ? "true" : "false",
                                                  f[2],
                                                  f[3],
                                                  f[4],
                                                  f[6],
                                                  f[7],
                                                  f[8],
                                                  f[9],
                                                  optional[0] ? "true" : "false",
                                                  optional[1] ? "true" : "false",
                                                  optional[2] ? "true" : "false",
                                                  optional[3] ? "true" : "false",
                                                  f[10],
                                                  f[11],
                                                  f[12],
                                                  f[13],
                                                  f[14],
                                                  f[15],
                                                  f[16]};
        add(usage.preimage_bytes, 20, limits.preimage_bytes);
        for (auto text : preimage)
            add(usage.preimage_bytes, text.size(), limits.preimage_bytes);
        add(usage.total_bytes, sizeof(Owner), limits.total_bytes);
        add(usage.total_bytes, sizeof(Owner::Storage), limits.total_bytes);
        add(usage.total_bytes, usage.features * sizeof(Feature), limits.total_bytes);
        add(usage.total_bytes, usage.string_bytes, limits.total_bytes);
        add(usage.total_bytes, canonical, limits.total_bytes);
        {
            std::string joined;
            joined.reserve(usage.preimage_bytes);
            for (std::size_t i = 0; i < preimage.size(); ++i) {
                if (i != 0)
                    joined += '|';
                joined += preimage[i];
            }
            if (!hex_matches(core::sha256(joined), f[17]))
                throw Rejected{"identity_mismatch"};
        }
        auto json = nlohmann::json::object();
        core::detail::JsonCleanup cleanup{json};
        json["project_version"] = f[0];
        json["git"] = nlohmann::json::object();
        json["git"]["revision"] = f[1];
        json["git"]["dirty"] = in.git.dirty;
        json["compiler"] = nlohmann::json::object();
        auto& compiler = json["compiler"];
        compiler["id"] = f[2];
        compiler["version"] = f[3];
        compiler["target"] = f[4];
        compiler["cxx_standard"] = f[5];
        compiler["flags"] = f[6];
        json["linker"] = nlohmann::json::object();
        json["linker"]["id"] = f[7];
        json["linker"]["version"] = f[8];
        json["preset"] = f[9];
        json["dependencies"] = nlohmann::json::object();
        auto& dependencies = json["dependencies"];
        constexpr std::array<std::string_view, 7> keys{
            "hwloc", "libnuma", "libfyaml", "CLI11", "nlohmann-json", "fmt", "Catch2"};
        for (std::size_t i = 0; i < keys.size(); ++i)
            dependencies[keys[i]] = f[10 + i];
        json["enabled_features"] = nlohmann::json::array();
        auto& features = json["enabled_features"];
        features.get_ref<nlohmann::json::array_t&>().reserve(in.enabled_features.size());
        for (auto feature : in.enabled_features)
            features.push_back(feature_names[static_cast<std::size_t>(feature)]);
        json["build_identity"] = f[17];
        auto encoded = core::canonicalize_json(json);
        core::detail::ExpectedErrorCleanup encoded_cleanup{encoded};
        if (!encoded || encoded->size() != canonical)
            throw Rejected{"internal_failure"};
        auto output = std::make_unique<Owner::Storage>(ManifestDigest{core::sha256(*encoded)});
        for (std::size_t i = 0; i < f.size(); ++i)
            output->fields[i] = f[i];
        output->dirty = in.git.dirty;
        output->features.assign(in.enabled_features.begin(), in.enabled_features.end());
        output->canonical = std::move(*encoded);
        output->usage = usage;
        return Owner{std::move(output)};
    } catch (const Rejected& rejection) {
        return std::unexpected{error(rejection.code)};
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const std::length_error&) {
        return std::unexpected{error("resource_failure")};
    } catch (...) {
        return std::unexpected{error("internal_failure")};
    }
}
} // namespace epycbench::results::build_manifest
