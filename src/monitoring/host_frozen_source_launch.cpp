#include "monitoring/host_frozen_source_launch.hpp"
#include "monitoring/host_frozen_source_launch_test_access.hpp"
#include "monitoring/host_frozen_process_control.hpp"
#include "core/json_ownership.hpp"
#include "epycbench/core/digest.hpp"
#include "runtime/controller_process_production_authority.hpp"
#include "runtime/kernel_module_session.hpp"
#include "core/governed_repository_source_access.hpp"
#include "monitoring/host_frozen_evidence.hpp"
#include "monitoring/host_input_template_access.hpp"
#include "platform_linux/bounded_sysfs_operations.hpp"
#include "platform_linux/host_baseline_file_export.hpp"
#include "platform_linux/controller_signal_scope.hpp"
#include "platform_linux/host_work_exec_entry.hpp"
#include "epycbench/build_manifest.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstdio>
#include <exception>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>

namespace epycbench::monitoring::qualification::detail {
namespace work = platform_linux::host_work;
namespace {
constexpr std::array<std::byte, 8> magic{
    std::byte{'E'}, std::byte{'B'}, std::byte{'S'}, std::byte{'O'},
    std::byte{'U'}, std::byte{'R'}, std::byte{'C'}, std::byte{'E'}};
constexpr std::uint64_t gib = UINT64_C(1073741824);
using Raw = platform_linux::detail::RawStatusResult;
constexpr std::string_view governed_plan_sha = "22b436daddbafd865c132391367a49040ce039a4ddb19be295f18349afd1cb63";
constexpr std::string_view governed_policy_sha = "54105988475a6031462ce4cc8b7d3e192f8481dc6e80bae9222228076c8aac14";
constexpr std::string_view governed_qid = "MON-007.host-v2.campaign-20261005-01";
constexpr std::string_view post_reboot_plan_sha = "2a9b4e221ad5ed46268f4634666a2bcbed6ee129369248cecf7893c699f2fc6d";
constexpr std::string_view post_reboot_qid = "MON-007.host-v2.campaign-20261018-01";
constexpr std::string_view governed_reboot_plan_sha = "6b7069d68547923d9624da08f8826a48e7fe8484f430dd33774759ba3f288145";
constexpr std::string_view governed_reboot_qid = "MON-007.host-v2.campaign-20261020-01";
constexpr std::string_view environment_recovery_plan_sha = "d5d8f3a21d9625e21c1ae3c550e22fad84492781f7fa14eb1b5543ef7605211c";
constexpr std::string_view environment_recovery_qid = "MON-007.host-v2.campaign-20261024-01";
constexpr std::string_view current_source_plan_sha = "babe0eb38c86210c1436802976dd5f87605fa8f37224f62b524c174e37af34c2";
constexpr std::string_view current_source_qid = "MON-007.host-v2.campaign-20261102-01";
constexpr std::uint64_t policy_exposure = UINT64_C(72000000000000);
constexpr std::uint64_t policy_restore = UINT64_C(30000000000);
auto closed_governed_object(std::string_view raw, std::span<const std::string_view> keys,
    bool empty_cleanup_array = false) -> nlohmann::json {
    if (raw.empty() || raw.size() > 4096U || keys.size() > 16U)
        throw std::invalid_argument{"governed capacity"};
    std::array<bool, 16> seen{};
    const auto callback = [&](int depth, nlohmann::json::parse_event_t event, nlohmann::json& value) {
        using Event = nlohmann::json::parse_event_t;
        if ((event == Event::object_start && depth != 0) || depth > 2 ||
            (event == Event::array_start && (!empty_cleanup_array || depth != 1)))
            throw std::invalid_argument{"governed nesting"};
        if (event == Event::key) {
            const auto& key = value.get_ref<const std::string&>();
            const auto found = std::ranges::find(keys, key);
            if (depth != 1 || found == keys.end()) throw std::invalid_argument{"governed key"};
            const auto index = static_cast<std::size_t>(found - keys.begin());
            if (seen[index]) throw std::invalid_argument{"governed duplicate"};
            seen[index] = true;
        }
        return true;
    };
    auto value = nlohmann::json::parse(raw.begin(), raw.end(), callback);
    if (!value.is_object() || value.size() != keys.size()) throw std::invalid_argument{"governed object"};
    return value;
}
auto governed_unsigned(const nlohmann::json& value, std::string_view key) -> std::uint64_t {
    if (!value.at(key).is_number_unsigned()) throw std::invalid_argument{"governed number"};
    return value.at(key).get<std::uint64_t>();
}
auto governed_hex(std::string_view text, std::size_t size) noexcept -> bool {
    return text.size() == size && std::ranges::all_of(text, [](char value) noexcept {
        return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
    });
}
void store(std::span<std::byte> bytes, std::size_t offset, std::uint64_t value,
           std::size_t width) noexcept {
    for (std::size_t i = 0; i < width; ++i)
        bytes[offset + width - 1U - i] = static_cast<std::byte>((value >> (8U * i)) & 255U);
}
auto load(std::span<const std::byte> bytes, std::size_t offset, std::size_t width) noexcept
    -> std::uint64_t {
    std::uint64_t result{};
    for (std::size_t i = 0; i < width; ++i)
        result = (result << 8U) | std::to_integer<std::uint64_t>(bytes[offset + i]);
    return result;
}
auto valid_resources(const work::Resources& value) noexcept -> bool {
    return value.processes <= 4 && value.threads <= 32 && value.measurement_workers <= 8 &&
        value.processes <= value.threads &&
        value.service_roles <= 2 && value.q2_helpers <= 1 && value.descriptors <= 1024 &&
        value.measurement_workers + value.service_roles + value.q2_helpers <= value.threads &&
        value.address_space_bytes.value() <= 20U * gib &&
        value.stack_bytes.value() <= value.address_space_bytes.value() &&
        value.mapped_bytes.value() <= 16U * gib &&
        value.mapped_bytes.value() <= value.address_space_bytes.value() &&
        value.ordinary_bytes.value() <= 4U * gib &&
        value.ordinary_bytes.value() <= value.address_space_bytes.value();
}
template <std::size_t N>
auto valid_token(const std::array<char, N>& token, std::size_t used, bool hexadecimal) noexcept -> bool {
    if (used > token.size()) return false;
    for (std::size_t i = 0; i < token.size(); ++i) {
        const auto character = token[i];
        if (i >= used) { if (character != '\0') return false; }
        else if (character == '\0' || (hexadecimal &&
            !((character >= '0' && character <= '9') || (character >= 'a' && character <= 'f'))))
            return false;
    }
    return true;
}
auto valid_record(const HostFrozenSourceLaunchRecord& value) noexcept -> bool {
    const auto type = static_cast<std::uint16_t>(value.type);
    const auto phase = static_cast<std::uint16_t>(value.phase);
    if (phase < 5U || phase > 7U || value.phase_generation != phase - 4U ||
        value.invocation_origin_ns >= value.campaign_end_ns ||
        value.campaign_end_ns - value.invocation_origin_ns > UINT64_C(10800000000000) ||
        value.origin_ns < value.invocation_origin_ns || value.origin_ns >= value.campaign_end_ns ||
        (phase == 5U && value.origin_ns != value.invocation_origin_ns)) return false;
    const auto span = phase == 5U ? UINT64_C(600000000000) :
                      phase == 6U ? UINT64_C(6000000000000) : UINT64_C(120000000000);
    if (value.phase_end_ns != value.origin_ns +
        std::min(value.campaign_end_ns - value.origin_ns, span)) return false;
    // The one controller survives accepted publication into Q6/Q7. Its actual
    // original startup pair remains bound to Q5; fresh phase children retain
    // their own phase-specific launch pair. This changes no launch deadline.
    const bool retained_controller = value.role == work::SourceLaunchRole::controller && phase > 5U;
    const auto launch_lower = retained_controller ? value.invocation_origin_ns : value.origin_ns;
    const auto launch_upper = retained_controller ? value.invocation_origin_ns +
        std::min(value.campaign_end_ns - value.invocation_origin_ns, UINT64_C(600000000000))
        : value.phase_end_ns;
    const auto output_limit = value.selector == host_frozen_source_input_selector ||
        value.selector == host_frozen_source_output_selector ||
        (value.type == HostFrozenSourceLaunchType::terminal && value.role == work::SourceLaunchRole::validator)
        ? UINT64_C(67108864) : UINT64_C(2097152);
    if (type < 1 || type > 10 || static_cast<unsigned>(value.role) >= 4 ||
        value.sequence == std::numeric_limits<std::uint64_t>::max() || value.generation == 0 ||
        value.origin_ns >= value.phase_end_ns || value.launch_origin_ns < launch_lower ||
        value.launch_origin_ns >= value.entry_end_ns || value.entry_end_ns > launch_upper ||
        value.entry_end_ns - value.launch_origin_ns > UINT64_C(5000000000) ||
        (value.permit_end_ns != 0 && (value.permit_end_ns <= value.origin_ns ||
                                    value.permit_end_ns > value.phase_end_ns)) ||
        !valid_resources(value.resources) || value.stdout_cap > output_limit ||
        (value.oid_width != 0 && value.oid_width != 40 && value.oid_width != 64) ||
        (value.secondary_oid_width != 0 && value.secondary_oid_width != value.oid_width) ||
        value.path_bytes > value.path.size() || value.rights_count > 3 || value.pid > INT32_MAX ||
        !valid_token(value.oid, value.oid_width, true) ||
        !valid_token(value.secondary_oid, value.secondary_oid_width, true) ||
        !valid_token(value.path, value.path_bytes, false)) return false;
    // Opaque association identities must actually be present. None authorizes a
    // caller on its own; the held channel and real per-message credentials do.
    const auto nonzero = [](const auto& bytes) noexcept {
        return std::any_of(bytes.begin(), bytes.end(), [](std::byte byte) { return byte != std::byte{}; });
    };
    return nonzero(value.invocation) && nonzero(value.build) && nonzero(value.source) && nonzero(value.campaign);
}
} // namespace

auto host_frozen_source_governed_entry_failure(bool invalid_clock, bool invalid_root,
    bool arguments_owned, std::string_view qualification_id) noexcept
    -> std::optional<HostFrozenSourceLaunchReason> {
    if (invalid_clock) return HostFrozenSourceLaunchReason::clock;
    if (invalid_root || !arguments_owned ||
        (qualification_id != governed_qid && qualification_id != post_reboot_qid &&
         qualification_id != governed_reboot_qid && qualification_id != environment_recovery_qid &&
         qualification_id != current_source_qid))
        return HostFrozenSourceLaunchReason::association;
    return std::nullopt;
}

auto encode_host_frozen_source_launch_record(const HostFrozenSourceLaunchRecord& value) noexcept
    -> std::expected<HostFrozenSourceLaunchBytes, HostFrozenSourceLaunchReason> {
    if (!valid_record(value)) return std::unexpected{HostFrozenSourceLaunchReason::malformed};
    HostFrozenSourceLaunchBytes bytes{};
    std::copy(magic.begin(), magic.end(), bytes.begin());
    store(bytes, 8, 1, 2);
    store(bytes, 10, static_cast<std::uint16_t>(value.type), 2);
    store(bytes, 12, static_cast<unsigned>(value.role), 2);
    store(bytes, 16, value.sequence, 8);
    std::copy(value.invocation.begin(), value.invocation.end(), bytes.begin() + 24);
    store(bytes, 40, value.generation, 8);
    store(bytes, 48, value.origin_ns, 8); store(bytes, 56, value.phase_end_ns, 8);
    store(bytes, 64, value.entry_end_ns, 8); store(bytes, 72, value.permit_end_ns, 8);
    const std::array<std::uint64_t, 10> resources{
        value.resources.processes, value.resources.threads, value.resources.measurement_workers,
        value.resources.service_roles, value.resources.q2_helpers, value.resources.descriptors,
        value.resources.stack_bytes.value(), value.resources.mapped_bytes.value(),
        value.resources.ordinary_bytes.value(), value.resources.address_space_bytes.value()};
    for (std::size_t i = 0; i < resources.size(); ++i) store(bytes, 80 + i * 8U, resources[i], 8);
    std::copy(value.build.begin(), value.build.end(), bytes.begin() + 160);
    std::copy(value.source.begin(), value.source.end(), bytes.begin() + 192);
    std::copy(value.campaign.begin(), value.campaign.end(), bytes.begin() + 224);
    store(bytes, 256, value.stdout_cap, 8);
    store(bytes, 264, value.oid_width, 1); store(bytes, 265, value.secondary_oid_width, 1);
    store(bytes, 266, value.path_bytes, 2); store(bytes, 268, value.pid, 8);
    store(bytes, 276, value.wait_status, 4);
    store(bytes, 280, static_cast<std::uint32_t>(value.error_number), 4);
    store(bytes, 284, value.rights_count, 2); store(bytes, 286, value.selector, 2);
    for (std::size_t i = 0; i < value.oid.size(); ++i) {
        bytes[288 + i] = static_cast<std::byte>(value.oid[i]);
        bytes[352 + i] = static_cast<std::byte>(value.secondary_oid[i]);
    }
    for (std::size_t i = 0; i < value.path.size(); ++i)
        bytes[416 + i] = static_cast<std::byte>(value.path[i]);
    store(bytes, 927, value.launch_origin_ns, 8);
    store(bytes, 935, static_cast<std::uint16_t>(value.phase), 2);
    store(bytes, 937, value.phase_generation, 8);
    store(bytes, 945, value.invocation_origin_ns, 8);
    store(bytes, 953, value.campaign_end_ns, 8);
    return bytes;
}
auto decode_host_frozen_source_launch_record(std::span<const std::byte> bytes) noexcept
    -> std::expected<HostFrozenSourceLaunchRecord, HostFrozenSourceLaunchReason> {
    if (bytes.size() != 1024 || !std::equal(magic.begin(), magic.end(), bytes.begin()) ||
        load(bytes, 8, 2) != 1 || load(bytes, 14, 2) != 0 ||
        !std::all_of(bytes.begin() + 961, bytes.end(), [](std::byte byte) { return byte == std::byte{}; }))
        return std::unexpected{HostFrozenSourceLaunchReason::malformed};
    HostFrozenSourceLaunchRecord value{};
    value.type = static_cast<HostFrozenSourceLaunchType>(load(bytes, 10, 2));
    const auto role = load(bytes, 12, 2);
    if (role >= 4) return std::unexpected{HostFrozenSourceLaunchReason::malformed};
    value.role = static_cast<work::SourceLaunchRole>(role);
    value.sequence = load(bytes, 16, 8); value.generation = load(bytes, 40, 8);
    std::copy_n(bytes.begin() + 24, value.invocation.size(), value.invocation.begin());
    value.origin_ns = load(bytes, 48, 8); value.phase_end_ns = load(bytes, 56, 8);
    value.entry_end_ns = load(bytes, 64, 8); value.permit_end_ns = load(bytes, 72, 8);
    std::array<std::uint64_t, 10> resources{};
    for (std::size_t i = 0; i < resources.size(); ++i) resources[i] = load(bytes, 80 + i * 8U, 8);
    for (std::size_t i = 0; i < 6; ++i)
        if (resources[i] > std::numeric_limits<std::size_t>::max())
            return std::unexpected{HostFrozenSourceLaunchReason::capacity};
    value.resources = {static_cast<std::size_t>(resources[0]), static_cast<std::size_t>(resources[1]),
        static_cast<std::size_t>(resources[2]), static_cast<std::size_t>(resources[3]),
        static_cast<std::size_t>(resources[4]), static_cast<std::size_t>(resources[5]),
        core::ByteCount::from_base_units(resources[6]), core::ByteCount::from_base_units(resources[7]),
        core::ByteCount::from_base_units(resources[8]), core::ByteCount::from_base_units(resources[9])};
    std::copy_n(bytes.begin() + 160, value.build.size(), value.build.begin());
    std::copy_n(bytes.begin() + 192, value.source.size(), value.source.begin());
    std::copy_n(bytes.begin() + 224, value.campaign.size(), value.campaign.begin());
    value.stdout_cap = load(bytes, 256, 8);
    value.oid_width = static_cast<std::uint8_t>(load(bytes, 264, 1));
    value.secondary_oid_width = static_cast<std::uint8_t>(load(bytes, 265, 1));
    value.path_bytes = static_cast<std::uint16_t>(load(bytes, 266, 2)); value.pid = load(bytes, 268, 8);
    value.wait_status = static_cast<std::uint32_t>(load(bytes, 276, 4));
    value.error_number = static_cast<std::int32_t>(static_cast<std::uint32_t>(load(bytes, 280, 4)));
    value.rights_count = static_cast<std::uint16_t>(load(bytes, 284, 2));
    value.selector = static_cast<std::uint16_t>(load(bytes, 286, 2));
    for (std::size_t i = 0; i < value.oid.size(); ++i) {
        value.oid[i] = static_cast<char>(std::to_integer<unsigned char>(bytes[288 + i]));
        value.secondary_oid[i] = static_cast<char>(std::to_integer<unsigned char>(bytes[352 + i]));
    }
    for (std::size_t i = 0; i < value.path.size(); ++i)
        value.path[i] = static_cast<char>(std::to_integer<unsigned char>(bytes[416 + i]));
    value.launch_origin_ns = load(bytes, 927, 8);
    value.phase = static_cast<work::SourceLaunchPhase>(load(bytes, 935, 2));
    value.phase_generation = load(bytes, 937, 8);
    value.invocation_origin_ns = load(bytes, 945, 8);
    value.campaign_end_ns = load(bytes, 953, 8);
    if (!valid_record(value)) return std::unexpected{HostFrozenSourceLaunchReason::malformed};
    return value;
}

namespace {
auto control_envelope_type(FrozenProcessType type) noexcept -> HostFrozenSourceLaunchType {
    if (type == FrozenProcessType::finish) return HostFrozenSourceLaunchType::finish;
    if (type == FrozenProcessType::permit || type == FrozenProcessType::grant ||
        type == FrozenProcessType::ack || type == FrozenProcessType::cancel)
        return HostFrozenSourceLaunchType::permit;
    return HostFrozenSourceLaunchType::ready;
}
constexpr std::string_view hexadecimal = "0123456789abcdef";
}
auto source_control_envelope(HostFrozenSourceLaunchRecord source,
    std::span<const std::byte> bytes) noexcept
    -> std::expected<HostFrozenSourceLaunchRecord, HostFrozenSourceLaunchReason> {
    const auto decoded = decode_frozen_process_record(bytes);
    if (source.role != work::SourceLaunchRole::collector || !decoded ||
        decoded->phase != static_cast<unsigned>(source.phase) ||
        (decoded->deadline_ns && decoded->deadline_ns > source.phase_end_ns))
        return std::unexpected{HostFrozenSourceLaunchReason::association};
    source.type = control_envelope_type(decoded->type);
    source.selector = host_frozen_source_control_selector;
    source.rights_count = 0;
    source.oid_width = 0; source.secondary_oid_width = 0;
    source.oid.fill('\0'); source.secondary_oid.fill('\0'); source.path.fill('\0');
    source.stdout_cap = 0; source.pid = 0; source.wait_status = 0; source.error_number = 0;
    source.path_bytes = 128;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        const auto value = std::to_integer<unsigned>(bytes[i]);
        source.path[2U * i] = hexadecimal[value >> 4U];
        source.path[2U * i + 1U] = hexadecimal[value & 15U];
    }
    if (!valid_record(source)) return std::unexpected{HostFrozenSourceLaunchReason::malformed};
    return source;
}
auto source_control_payload(const HostFrozenSourceLaunchRecord& source) noexcept
    -> std::expected<std::array<std::byte, 64>, HostFrozenSourceLaunchReason> {
    if (!valid_record(source) || source.role != work::SourceLaunchRole::collector ||
        source.selector != host_frozen_source_control_selector ||
        source.path_bytes != 128U || source.rights_count || source.stdout_cap || source.oid_width ||
        source.secondary_oid_width || source.wait_status || source.error_number)
        return std::unexpected{HostFrozenSourceLaunchReason::malformed};
    std::array<std::byte, 64> bytes{};
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        const auto high = hexadecimal.find(source.path[2U * i]);
        const auto low = hexadecimal.find(source.path[2U * i + 1U]);
        if (high == std::string_view::npos || low == std::string_view::npos)
            return std::unexpected{HostFrozenSourceLaunchReason::malformed};
        bytes[i] = static_cast<std::byte>((high << 4U) | low);
    }
    const auto decoded = decode_frozen_process_record(bytes);
    if (!decoded || decoded->phase != static_cast<unsigned>(source.phase) ||
        control_envelope_type(decoded->type) != source.type ||
        (decoded->deadline_ns && decoded->deadline_ns > source.phase_end_ns))
        return std::unexpected{HostFrozenSourceLaunchReason::association};
    return bytes;
}

HostFrozenSourceChildQuota::HostFrozenSourceChildQuota(const HostFrozenSourceLaunchBinding* binding,
    work::Resources resources) noexcept : binding_{binding}, resources_{resources} {}
HostFrozenSourceChildQuota::HostFrozenSourceChildQuota(HostFrozenSourceChildQuota&& other) noexcept
    : binding_{std::exchange(other.binding_, nullptr)}, resources_{other.resources_} {}
auto HostFrozenSourceChildQuota::ready() const noexcept -> bool {
    return binding_ && binding_->ready() && !binding_->quota_claimed_ &&
        binding_->grant_.role == work::SourceLaunchRole::collector &&
        resources_ == binding_->grant_.resources && resources_.processes == 1 &&
        resources_.descriptors != 0 && resources_.address_space_bytes.value() != 0;
}
auto HostFrozenSourceChildQuota::parent_pid() const noexcept -> int {
    return binding_ ? binding_->parent_pid_ : 0;
}
auto HostFrozenSourceChildQuota::origin_ns() const noexcept -> std::uint64_t {
    return binding_ ? binding_->grant_.origin_ns : 0;
}
auto HostFrozenSourceChildQuota::phase_end_ns() const noexcept -> std::uint64_t {
    return binding_ ? binding_->grant_.phase_end_ns : 0;
}
auto HostFrozenSourceChildQuota::guard() const noexcept
    -> std::expected<void, HostFrozenSourceLaunchReason> {
    if (!binding_ || !binding_->ready() || resources_ != binding_->grant_.resources)
        return std::unexpected{HostFrozenSourceLaunchReason::association};
    return binding_->guard();
}
auto HostFrozenSourceChildQuota::claim_ledger() const noexcept
    -> std::expected<work::ResourceLedger, HostFrozenSourceLaunchReason> {
    if (!ready() || !guard())
        return std::unexpected{HostFrozenSourceLaunchReason::association};
    auto ledger = work::ResourceLedger::prepare(resources_);
    if (!ledger) return std::unexpected{HostFrozenSourceLaunchReason::capacity};
    // No resource acquisition occurs in ledger preparation. Recheck the actual
    // owner before the sole nonallocating authority transition.
    if (!ready() || !guard())
        return std::unexpected{HostFrozenSourceLaunchReason::association};
    binding_->quota_claimed_ = true;
    return std::move(*ledger);
}
HostFrozenSourceLaunchBinding::HostFrozenSourceLaunchBinding(int channel, int parent,
    std::uint32_t uid, runtime::RunnerCancellation& cancellation, HostFrozenSourceLaunchIO io,
    GitOperations git, HostFrozenSourceLaunchRecord expected, std::span<const int> borrowed) noexcept
    : channel_{channel}, parent_pid_{parent}, uid_{uid}, cancellation_{&cancellation}, io_{io},
      git_{git}, grant_{expected}, last_ns_{expected.origin_ns} {
    borrowed_descriptors_.fill(-1);
    for (const auto descriptor : {0, 1, 2, channel}) borrowed_descriptors_[borrowed_count_++] = descriptor;
    for (const auto descriptor : borrowed) {
        if (descriptor < 0) {
            static_cast<void>(fail(HostFrozenSourceLaunchReason::association));
            continue;
        }
        if (std::find(borrowed_descriptors_.begin(), borrowed_descriptors_.end(), descriptor) !=
            borrowed_descriptors_.end()) continue;
        if (borrowed_count_ == borrowed_descriptors_.size()) {
            static_cast<void>(fail(HostFrozenSourceLaunchReason::capacity));
            break;
        }
        borrowed_descriptors_[borrowed_count_++] = descriptor;
    }
}
HostFrozenSourceLaunchBinding::~HostFrozenSourceLaunchBinding() noexcept {
    if (!channel_closed_ || uncertainty_ ||
        std::any_of(pending_rights_.begin(), pending_rights_.end(),
                    [](const auto& right) noexcept { return right.descriptor >= 0 && !right.consumed; }))
        std::terminate();
}
auto HostFrozenSourceLaunchBinding::fail(HostFrozenSourceLaunchReason reason,
    HostFrozenSourceBindingOperation operation, Raw native, int descriptor) const noexcept
    -> std::unexpected<HostFrozenSourceLaunchReason> {
    const HostFrozenSourceBindingFact fact{reason, operation, native.value, native.error_number, descriptor};
    if (!primary_) { primary_ = reason; first_fact_ = fact; }
    else if (secondary_count_ < secondary_.size()) secondary_[secondary_count_++] = fact;
    else uncertainty_ = true;
    return std::unexpected{*primary_};
}
auto HostFrozenSourceLaunchBinding::guard() const noexcept
    -> std::expected<void, HostFrozenSourceLaunchReason> {
    if (primary_) return std::unexpected{*primary_};
    if (!io_.guard.now || !io_.receive || !io_.send || !io_.close || channel_closed_)
        return fail(HostFrozenSourceLaunchReason::association);
    if (cancellation_->requested()) return fail(HostFrozenSourceLaunchReason::cancelled);
    std::uint64_t now{};
    const auto sampled = io_.guard.now(io_.guard.context, &now);
    if (sampled.value != 0 || sampled.error_number || now < last_ns_) {
        auto original = sampled;
        if (io_.raw_clock && io_.raw_clock->raw_present &&
            io_.raw_clock->operation == work::SourceLaunchChildOperation::clock) {
            original = {static_cast<long>(io_.raw_clock->native_result), io_.raw_clock->error_number};
        }
        return fail(HostFrozenSourceLaunchReason::clock, HostFrozenSourceBindingOperation::guard, original);
    }
    last_ns_ = now;
    if (cancellation_->requested()) return fail(HostFrozenSourceLaunchReason::cancelled);
    const auto end = grant_.permit_end_ns == 0 ? grant_.phase_end_ns
                                              : std::min(grant_.phase_end_ns, grant_.permit_end_ns);
    if (now >= end) return fail(HostFrozenSourceLaunchReason::deadline);
    return {};
}
auto HostFrozenSourceLaunchBinding::ready() const noexcept -> bool {
    return authenticated_ && !channel_closed_ && !uncertainty_ && !primary_;
}
auto HostFrozenSourceLaunchBinding::write_payload(const FrozenProcessRecord& record,
    std::string_view body, work::SourceLaunchStreamOperations operations) noexcept
    -> std::expected<void, HostFrozenSourceLaunchReason> {
    if (const auto checked = guard(); !checked) return checked;
    if (!ready() || !operations.write || !operations.pause)
        return fail(HostFrozenSourceLaunchReason::association);
    if ((record.type != FrozenProcessType::cell && record.type != FrozenProcessType::failure &&
         record.type != FrozenProcessType::finish) || record.phase != static_cast<unsigned>(grant_.phase) ||
        record.payload_bytes != body.size() || record.commitment != core::sha256(body).bytes())
        return fail(HostFrozenSourceLaunchReason::malformed, HostFrozenSourceBindingOperation::decode);
    const auto header = encode_frozen_process_record(record);
    if (!header) return fail(HostFrozenSourceLaunchReason::malformed, HostFrozenSourceBindingOperation::decode);
    // The authenticated socket carries the same record as control metadata;
    // the validator's separate stdin stream needs this header before the body.
    const std::array parts{std::span<const std::byte>{*header},
                          std::as_bytes(std::span{body.data(), body.size()})};
    for (auto remainder : parts) {
        while (!remainder.empty()) {
            if (const auto checked = guard(); !checked) return checked;
            const auto requested = remainder.first(std::min(remainder.size(), std::size_t{65536}));
            const auto written = operations.write(1, requested);
            if (written.value > 0 && !written.error_number &&
                static_cast<std::size_t>(written.value) <= requested.size()) {
                remainder = remainder.subspan(static_cast<std::size_t>(written.value));
                continue;
            }
            if (written.value < 0 && (written.error_number == EINTR || written.error_number == EAGAIN)) {
                if (const auto checked = guard(); !checked) return checked;
                const auto paused = operations.pause(5);
                if ((paused.value < 0 || paused.error_number) && paused.error_number != EINTR)
                    return fail(HostFrozenSourceLaunchReason::io, HostFrozenSourceBindingOperation::send, paused, 1);
                continue;
            }
            return fail(HostFrozenSourceLaunchReason::io, HostFrozenSourceBindingOperation::send, written, 1);
        }
    }
    return guard();
}
auto HostFrozenSourceLaunchBinding::close_pending() noexcept
    -> std::expected<void, HostFrozenSourceLaunchReason> {
    bool failed{};
    for (std::size_t i = 0; i < rights_count_; ++i) {
        auto& right = pending_rights_[i];
        if (right.descriptor < 0 || right.consumed) continue;
        right.consumed = true;
        const auto closed = io_.close(io_.context, right.descriptor);
        right.close_result = closed;
        if (closed.value != 0 || closed.error_number) {
            right.uncertain = true;
            failed = true;
            uncertainty_ = true;
            static_cast<void>(fail(HostFrozenSourceLaunchReason::uncertain,
                HostFrozenSourceBindingOperation::close, closed, right.descriptor));
        }
    }
    if (failed) return std::unexpected{*primary_};
    return {};
}
auto HostFrozenSourceLaunchBinding::authenticate() noexcept
    -> std::expected<void, HostFrozenSourceLaunchReason> {
    if (authenticated_ || primary_ || rights_count_ != 0 || !valid_record(grant_) || grant_.type != HostFrozenSourceLaunchType::grant ||
        channel_ < 3 || parent_pid_ <= 0 || uid_ == 0 || grant_.rights_count != 0)
        return fail(HostFrozenSourceLaunchReason::association);
    auto checked = guard();
    if (!checked) return checked;
    HostFrozenSourceLaunchBytes bytes{};
    HostFrozenSourceAncillary ancillary{};
    const auto received = io_.receive(io_.context, channel_, bytes, &ancillary);
    // Register all actual returned owners before guard, credential or payload
    // validation. Native buffer capacity and the injected leaf contract are16.
    for (std::size_t i = 0; i < std::min(ancillary.count, pending_rights_.size()); ++i) {
        const auto descriptor = ancillary.descriptors[i];
        if (descriptor < 0 ||
            std::find(borrowed_descriptors_.begin(), borrowed_descriptors_.end(), descriptor) != borrowed_descriptors_.end() ||
            std::any_of(pending_rights_.begin(), pending_rights_.end(),
                        [descriptor](const auto& right) noexcept { return right.descriptor == descriptor; })) {
            ancillary.malformed = true;
            continue;
        }
        pending_rights_[rights_count_++].descriptor = descriptor;
    }
    if (ancillary.count > pending_rights_.size()) ancillary.malformed = true;
    if (received.value != static_cast<long>(bytes.size()) || received.error_number) {
        const auto reason = received.value < 0 || received.error_number
            ? HostFrozenSourceLaunchReason::io
            : received.value == 0 ? HostFrozenSourceLaunchReason::eof : HostFrozenSourceLaunchReason::malformed;
        static_cast<void>(fail(reason, HostFrozenSourceBindingOperation::receive, received, channel_));
        static_cast<void>(close_pending());
        return std::unexpected{*primary_};
    }
    if (ancillary.truncated || ancillary.malformed || ancillary.count != 0) {
        static_cast<void>(fail(HostFrozenSourceLaunchReason::rights,
            HostFrozenSourceBindingOperation::receive, received, channel_));
        static_cast<void>(close_pending());
        return std::unexpected{*primary_};
    }
    if (!ancillary.credentials || ancillary.peer.pid != parent_pid_ || ancillary.peer.uid != uid_) {
        static_cast<void>(fail(HostFrozenSourceLaunchReason::credentials,
            HostFrozenSourceBindingOperation::credentials, received, channel_));
        static_cast<void>(close_pending());
        return std::unexpected{*primary_};
    }
    auto decoded = decode_host_frozen_source_launch_record(bytes);
    if (!decoded || *decoded != grant_) return fail(HostFrozenSourceLaunchReason::association,
        HostFrozenSourceBindingOperation::decode, received, channel_);
    checked = guard();
    if (!checked) return checked;
    authenticated_ = true;
    return {};
}
auto HostFrozenSourceLaunchBinding::child_quota() const noexcept
    -> std::expected<HostFrozenSourceChildQuota, HostFrozenSourceLaunchReason> {
    if (!ready() || quota_issued_ || grant_.role != work::SourceLaunchRole::collector)
        return fail(HostFrozenSourceLaunchReason::association);
    quota_issued_ = true;
    return HostFrozenSourceChildQuota{this, grant_.resources};
}
auto HostFrozenSourceLaunchBinding::git(const MonitoringQ1GitExecutable& executable,
    int repository, std::uint16_t selector, std::string_view oid, std::string_view secondary_oid,
    std::string_view path, std::size_t stdout_cap) const
    -> std::expected<MonitoringQ1GitBoundedOutput, MonitoringQ1GitCommandFailure> {
    if (!ready() || !git_.run || repository < 0) {
        static_cast<void>(fail(HostFrozenSourceLaunchReason::association));
        throw std::logic_error{"inactive source Git binding"};
    }
    if (!guard()) throw std::logic_error{"source Git binding guard refused"};
    return git_.run(git_.context, executable, repository, selector, oid, secondary_oid, path, stdout_cap);
}
auto HostFrozenSourceLaunchBinding::retire() noexcept
    -> std::expected<void, HostFrozenSourceLaunchReason> {
    static_cast<void>(close_pending());
    if (!channel_closed_) {
        channel_closed_ = true;
        const auto descriptor = std::exchange(channel_, -1);
        const auto closed = io_.close(io_.context, descriptor);
        if (closed.value != 0 || closed.error_number) {
            uncertainty_ = true;
            return fail(HostFrozenSourceLaunchReason::uncertain,
                HostFrozenSourceBindingOperation::close, closed, descriptor);
        }
    }
    if (uncertainty_) return std::unexpected{primary_.value_or(HostFrozenSourceLaunchReason::uncertain)};
    return {};
}
auto production_host_frozen_source_launch_io(HostFrozenSourceLaunchClock& clock) noexcept
    -> HostFrozenSourceLaunchIO {
    const auto native = work::production_source_launch_channel_operations();
    return {native.context, native.receive, native.send, native.close,
        {&clock, [](void* raw, std::uint64_t* output) noexcept -> Raw {
            auto& current = *static_cast<HostFrozenSourceLaunchClock*>(raw);
            return work::source_launch_raw_now(current.current, output, &current.actual);
        }, [](void* raw) noexcept -> bool {
            const auto* token = static_cast<HostFrozenSourceLaunchClock*>(raw)->cancellation;
            return !token || token->requested();
        }, &clock.actual}, &clock.current};
}
auto host_frozen_source_linux_limits(work::SourceLaunchPhase phase) noexcept
    -> std::expected<platform_linux::detail::TopologySourceLimits, HostFrozenSourceLaunchReason> {
    if (phase != work::SourceLaunchPhase::q5 && phase != work::SourceLaunchPhase::q6)
        return std::unexpected{HostFrozenSourceLaunchReason::malformed};
    const std::uint64_t acquisitions = phase == work::SourceLaunchPhase::q5 ? 9U : 385U;
    // These are deliberately conservative operating limits, not an assertion
    // about dependency allocation or the maximum possible Linux machine. The
    // cumulative work/text counters cover the whole phase; shapes bound each
    // materialized topology. Exceeding any limit fails the attempt.
    return platform_linux::detail::TopologySourceLimits{
        .acquisitions = acquisitions,
        .io_calls = acquisitions * UINT64_C(2000000),
        .guard_checks = acquisitions * UINT64_C(6000000),
        .conversion_work = acquisitions * (UINT64_C(1) << 36U),
        .path_bytes = 4095U, .component_bytes = 255U, .path_depth = 64U,
        .link_expansions = 40U, .file_bytes = UINT64_C(1048576),
        .total_text_bytes = acquisitions * (UINT64_C(1) << 29U),
        .entries_per_directory = 131072U,
        .total_entries = acquisitions * UINT64_C(1048576),
        .refills = acquisitions * UINT64_C(1048576),
        .online_cpus = 4096U, .enumerated_cpus = 4096U,
        .cache_entries_per_cpu = 32U, .cache_entries = 131072U,
        .online_nodes = 1024U, .pci_entries = 65536U,
        .memberships = UINT64_C(1) << 26U, .bitmap_groups = 2048U,
        .maximum_cpu_index = 65535U, .maximum_node_index = 65535U,
        .maximum_cache_index = 65535U,
        .cluster_work = UINT64_C(1) << 36U, .distance_values = 1048576U};
}

// The actual source invocation's owning storage. A packet is made only from
// ready, authenticated Admission borrows and is registered here before Control
// consumes that Admission. It conveys bytes/lineage, never a serialized Session.
struct HostFrozenSourceLaunchState final {
    HostFrozenSourceLaunchState(runtime::ControllerProcessProductionAuthority& authority_value,
        runtime::RunnerCancellation& cancellation_value, HostFrozenSourceLaunchClock& clock_value,
        work::ResourceLedger& ledger_value) noexcept
        : authority{authority_value}, cancellation{cancellation_value}, clock{clock_value},
          ledger{ledger_value} { control_clock.cancellation = &cancellation_value; }
    HostFrozenSourceLaunchState(const HostFrozenSourceLaunchState&) = delete;
    HostFrozenSourceLaunchState(HostFrozenSourceLaunchState&&) = delete;
    ~HostFrozenSourceLaunchState() noexcept {
        if (control || root_descriptor >= 0 || output_descriptor >= 0 || retained_descriptor >= 0 ||
            qualification_descriptor >= 0 || std::any_of(walking.begin(), walking.end(), [](int fd) { return fd >= 0; }))
            std::terminate();
        if (raw_input_failure) core::detail::retire_json(raw_input_failure->context);
        packet.clear();
        std::string{}.swap(packet); // Actual buffer ends before its reservation.
        if (packet_lease && packet_lease->active() && !packet_lease->retire()) std::terminate();
        std::string{}.swap(retained_bytes);
        if (retained_lease && retained_lease->active() && !retained_lease->retire()) std::terminate();
    }
    runtime::ControllerProcessProductionAuthority& authority;
    runtime::RunnerCancellation& cancellation;
    HostFrozenSourceLaunchClock& clock;
    HostFrozenSourceLaunchClock control_clock;
    std::uint64_t control_last{}, original_phase_end{};
    std::atomic<std::uint64_t> control_permit_end{};
    work::ResourceLedger& ledger;
    std::optional<work::ResourceLease> packet_lease;
    std::string packet;
    std::optional<core::Sha256Digest> packet_sha;
    std::optional<FrozenSourceControlOwner> control;
    std::optional<HostFrozenSourceAdmissionFailure> admission_failure;
    std::optional<MonitoringRunningBuildAuthority> running_build;
    std::optional<core::Error> raw_input_failure;
    std::optional<work::ResourceLease> input_lease;
    std::optional<work::ResourceLease> evidence_lease;
    std::array<char, work::SourceInputFileOwner::input_capacity> input_scratch{};
    std::optional<work::SourceInputFileOwner> input_reader;
    HostFrozenSourceLaunchBinding* binding{};
    int root_descriptor{-1}, output_descriptor{-1};
    int retained_descriptor{-1}, qualification_descriptor{-1};
    std::array<int, 3> walking{-1,-1,-1};
    std::optional<work::ResourceLease> retained_lease;
    std::string retained_bytes;
    std::filesystem::path repository_path, qualification_path;
    std::array<std::string_view, 9> retained_artifacts{};
    std::string_view original_packet, retained_build;
    std::uint64_t qualification_device{}, qualification_inode{};
    std::uint64_t last{}, incoming_sequence{}, outgoing_sequence{};
    bool cleanup_uncertain{};
    static auto collect(int, int, int, int, int, HostFrozenSourceLaunchRecord,
        const char*, const char*, runtime::ControllerProcessProductionAuthority&,
        runtime::RunnerCancellation&, HostFrozenSourceLaunchClock&,
        const HostFrozenSourceSyntheticDispatcher* = nullptr) noexcept -> int;

    [[nodiscard]] auto run_guard() noexcept -> work::GuardOperations {
        return {this,
            +[](void* raw, std::uint64_t* output) noexcept -> Raw {
                auto& state = *static_cast<HostFrozenSourceLaunchState*>(raw);
                const auto sampled = work::source_launch_raw_now(state.control_clock.current,
                    output, &state.control_clock.actual);
                if (sampled.value || sampled.error_number) return sampled;
                if (*output < state.control_last) return {-1, ERANGE};
                state.control_last = *output;
                const auto permit = state.control_permit_end.load(std::memory_order_acquire);
                if (*output >= (permit ? std::min(permit, state.original_phase_end) : state.original_phase_end))
                    return {-1, ETIMEDOUT};
                return sampled;
            },
            +[](void* raw) noexcept -> bool {
                return static_cast<HostFrozenSourceLaunchState*>(raw)->cancellation.requested();
            }, &control_clock.actual};
    }

    [[nodiscard]] auto close_walking(std::size_t slot) noexcept -> bool {
        if (walking[slot] < 0) return true;
        const auto fd = std::exchange(walking[slot], -1);
        const auto native = platform_linux::detail::production_topology_source_operations();
        const auto closed = native.close(native.context, fd);
        if (closed.value || closed.error_number) {
            cleanup_uncertain = true;
            static_cast<void>(binding->fail(HostFrozenSourceLaunchReason::uncertain,
                HostFrozenSourceBindingOperation::close, closed, fd));
            return false;
        }
        return true;
    }
    [[nodiscard]] auto open_walking(int parent, const char* name, int flags, std::size_t slot) noexcept -> bool {
        if (!binding->guard() || walking[slot] >= 0) return false;
        const auto native = platform_linux::detail::production_topology_source_operations();
        const auto opened = native.open_at(native.context, parent, name, flags);
        if (opened.value >= 0 && opened.value <= INT32_MAX) {
            const auto fd = static_cast<int>(opened.value);
            if (fd == root_descriptor || fd == output_descriptor || fd == retained_descriptor ||
                fd == qualification_descriptor || fd == binding->channel_ ||
                std::find(walking.begin(), walking.end(), fd) != walking.end()) {
                static_cast<void>(binding->fail(HostFrozenSourceLaunchReason::association,
                    HostFrozenSourceBindingOperation::open, opened, fd)); return false;
            }
            walking[slot] = fd;
        }
        if (opened.value < 3 || opened.value > INT32_MAX || opened.error_number) {
            static_cast<void>(binding->fail(HostFrozenSourceLaunchReason::native,
                HostFrozenSourceBindingOperation::open, opened, parent)); return false;
        }
        return binding->guard().has_value();
    }
    [[nodiscard]] auto directory_matches(int fd, bool root) noexcept -> bool {
        const auto native = platform_linux::detail::production_topology_source_operations();
        std::uint64_t mask{}; std::size_t visited{};
        for (unsigned refill{}; refill < 8U; ++refill) {
            if (!binding->guard()) return false;
            const auto read = native.entries(native.context, fd, std::span{input_scratch}.first(4096U));
            if (read.value < 0 || read.error_number || read.value > 4096) {
                static_cast<void>(binding->fail(HostFrozenSourceLaunchReason::io,
                    HostFrozenSourceBindingOperation::read, read, fd)); return false;
            }
            if (!binding->guard()) return false;
            if (!read.value) return mask == (root ? UINT64_C(511) : UINT64_C(1));
            const auto parsed = work::source_launch_directory_names(
                std::span{input_scratch}.first(static_cast<std::size_t>(read.value)),
                work::SourceLaunchPhase::q5, root, mask, visited);
            if (!parsed) return false;
        }
        return false;
    }
    [[nodiscard]] auto recheck_retained(const std::filesystem::path& path,
        std::uint64_t device, std::uint64_t inode) noexcept
        -> std::expected<void, HostFrozenSourceLaunchReason> {
        const auto native = platform_linux::detail::production_topology_source_operations();
        bool accepted{};
        const auto run = [&]() -> bool {
            if (path != qualification_path || device != qualification_device || inode != qualification_inode ||
                !binding->guard()) return false;
            struct stat inherited{};
            const auto observed = native.stat_fd(native.context, qualification_descriptor, &inherited);
            if (observed.value || observed.error_number) {
                static_cast<void>(binding->fail(HostFrozenSourceLaunchReason::native,
                    HostFrozenSourceBindingOperation::metadata, observed, qualification_descriptor)); return false;
            }
            if (!S_ISDIR(inherited.st_mode) ||
                static_cast<std::uint64_t>(inherited.st_dev) != device ||
                static_cast<std::uint64_t>(inherited.st_ino) != inode) return false;
            constexpr int dirs = O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;
            if (!open_walking(root_descriptor, ".local", dirs, 0U) ||
                !open_walking(walking[0], "monitoring-qualification", dirs, 1U) || !close_walking(0U)) return false;
            const auto qid = qualification_path.filename().native();
            if (!open_walking(walking[1], qid.c_str(), dirs, 0U) || !close_walking(1U)) return false;
            struct stat held{};
            const auto root_stat = native.stat_fd(native.context, walking[0], &held);
            if (root_stat.value || root_stat.error_number) {
                static_cast<void>(binding->fail(HostFrozenSourceLaunchReason::native,
                    HostFrozenSourceBindingOperation::metadata, root_stat, walking[0])); return false;
            }
            if (held.st_dev != inherited.st_dev ||
                held.st_ino != inherited.st_ino || !directory_matches(walking[0], true)) return false;
            constexpr std::array<const char*, 9> names{
                "host-environment.json", "q0b", "q1", "q2", "q3", "q4", "q5", "manifest.json", "checksums.sha256"};
            for (std::size_t index{}; index < names.size(); ++index) {
                int parent = walking[0]; const char* leaf = names[index];
                if (index >= 1U && index <= 6U) {
                    if (!open_walking(parent, leaf, dirs, 1U) || !directory_matches(walking[1], false)) return false;
                    parent = walking[1]; leaf = "evidence.json";
                }
                if (!open_walking(parent, leaf, O_RDONLY | O_NOFOLLOW | O_CLOEXEC, 2U)) return false;
                struct stat before{}, after{}, named{};
                const auto before_stat = native.stat_fd(native.context, walking[2], &before);
                const auto expected = retained_artifacts[index];
                if (before_stat.value || before_stat.error_number) {
                    static_cast<void>(binding->fail(HostFrozenSourceLaunchReason::native,
                        HostFrozenSourceBindingOperation::metadata, before_stat, walking[2])); return false;
                }
                if (!S_ISREG(before.st_mode) ||
                    before.st_size < 0 || static_cast<std::uint64_t>(before.st_size) != expected.size()) return false;
                std::size_t position{};
                while (position < expected.size()) {
                    if (!binding->guard()) return false;
                    const auto read = native.read(native.context, walking[2],
                        std::span{input_scratch}.first(std::min(input_scratch.size(), expected.size() - position)));
                    if (read.value <= 0 || read.error_number) {
                        static_cast<void>(binding->fail(HostFrozenSourceLaunchReason::io,
                            HostFrozenSourceBindingOperation::read, read, walking[2])); return false;
                    }
                    if (static_cast<std::size_t>(read.value) > expected.size() - position ||
                        std::string_view{input_scratch.data(), static_cast<std::size_t>(read.value)} !=
                            expected.substr(position, static_cast<std::size_t>(read.value))) return false;
                    position += static_cast<std::size_t>(read.value);
                }
                if (!binding->guard()) return false;
                const auto after_stat = native.stat_fd(native.context, walking[2], &after);
                if (after_stat.value || after_stat.error_number) {
                    static_cast<void>(binding->fail(HostFrozenSourceLaunchReason::native,
                        HostFrozenSourceBindingOperation::metadata, after_stat, walking[2])); return false;
                }
                const auto named_stat = native.stat_at(native.context, parent, leaf, &named);
                if (named_stat.value || named_stat.error_number) {
                    static_cast<void>(binding->fail(HostFrozenSourceLaunchReason::native,
                        HostFrozenSourceBindingOperation::metadata, named_stat, parent)); return false;
                }
                if (
                    before.st_dev != after.st_dev || before.st_ino != after.st_ino ||
                    before.st_size != after.st_size || before.st_mtim.tv_sec != after.st_mtim.tv_sec ||
                    before.st_mtim.tv_nsec != after.st_mtim.tv_nsec || named.st_dev != after.st_dev ||
                    named.st_ino != after.st_ino || !close_walking(2U) || !close_walking(1U)) return false;
            }
            return binding->guard().has_value();
        };
        // path.filename() may allocate in ordinary preparation. A failure still
        // drains every already-adopted sibling exactly once.
        try { accepted = run(); } catch (...) { accepted = false; }
        if (!accepted && !binding->raw_primary())
            static_cast<void>(binding->fail(HostFrozenSourceLaunchReason::association));
        for (std::size_t slot{}; slot < walking.size(); ++slot)
            if (!close_walking(slot)) accepted = false;
        if (!accepted) {
            if (!binding->raw_primary()) static_cast<void>(binding->fail(HostFrozenSourceLaunchReason::association));
            return std::unexpected{binding->raw_primary().value_or(HostFrozenSourceLaunchReason::association)};
        }
        return {};
    }
    auto read_retained_input() -> bool {
        if (!binding->guard() || retained_descriptor < 3 || qualification_descriptor < 3) return false;
        const auto native = platform_linux::detail::production_topology_source_operations();
        struct stat before{};
        const auto observed = native.stat_fd(native.context, retained_descriptor, &before);
        if (observed.value || observed.error_number) {
            static_cast<void>(binding->fail(HostFrozenSourceLaunchReason::native,
                HostFrozenSourceBindingOperation::metadata, observed, retained_descriptor)); return false;
        }
        if (!S_ISREG(before.st_mode) ||
            before.st_size < 68 || before.st_size > INT64_C(67108864)) return false;
        work::Resources demand{};
        demand.ordinary_bytes = core::ByteCount::from_base_units(static_cast<std::uint64_t>(before.st_size) + 1U);
        demand.address_space_bytes = demand.ordinary_bytes;
        auto reserved = ledger.reserve(demand);
        if (!reserved) return false;
        retained_lease.emplace(std::move(*reserved));
        retained_bytes.resize(static_cast<std::size_t>(before.st_size));
        std::size_t offset{};
        while (offset < retained_bytes.size()) {
            if (!binding->guard()) return false;
            const auto bytes = std::as_writable_bytes(std::span{retained_bytes.data(), retained_bytes.size()}).subspan(offset);
            const auto read = work::production_source_launch_stream_operations().read_at(retained_descriptor,
                bytes.first(std::min(bytes.size(), std::size_t{65536})), offset);
            if (read.value <= 0 || read.error_number) {
                static_cast<void>(binding->fail(HostFrozenSourceLaunchReason::io,
                    HostFrozenSourceBindingOperation::read, read, retained_descriptor)); return false;
            }
            if (static_cast<std::size_t>(read.value) > bytes.size()) return false;
            offset += static_cast<std::size_t>(read.value);
        }
        struct stat after{};
        const auto reobserved = native.stat_fd(native.context, retained_descriptor, &after);
        if (reobserved.value || reobserved.error_number) {
            static_cast<void>(binding->fail(HostFrozenSourceLaunchReason::native,
                HostFrozenSourceBindingOperation::metadata, reobserved, retained_descriptor)); return false;
        }
        if (before.st_dev != after.st_dev ||
            before.st_ino != after.st_ino || before.st_size != after.st_size ||
            before.st_mtim.tv_sec != after.st_mtim.tv_sec || before.st_mtim.tv_nsec != after.st_mtim.tv_nsec) return false;
        auto bytes = std::as_bytes(std::span{retained_bytes.data(), retained_bytes.size()});
        if (retained_bytes.substr(0U, 8U) != std::string_view{"EBSRETN\0", 8U} ||
            load(bytes, 8U, 2U) != 1U || load(bytes, 10U, 2U) || load(bytes, 12U, 8U) != bytes.size()) return false;
        struct stat repository{};
        const auto root_stat = native.stat_fd(native.context, root_descriptor, &repository);
        if (root_stat.value || root_stat.error_number) {
            static_cast<void>(binding->fail(HostFrozenSourceLaunchReason::native,
                HostFrozenSourceBindingOperation::metadata, root_stat, root_descriptor)); return false;
        }
        if (load(bytes, 20U, 8U) != static_cast<std::uint64_t>(repository.st_dev) ||
            load(bytes, 28U, 8U) != static_cast<std::uint64_t>(repository.st_ino)) return false;
        qualification_device = load(bytes, 36U, 8U); qualification_inode = load(bytes, 44U, 8U);
        offset = 52U;
        const auto block = [&](std::string_view wire, std::size_t& cursor) -> std::string_view {
            if (cursor > wire.size() || wire.size() - cursor < 4U) throw std::invalid_argument{"retained input truncated"};
            const auto source = std::as_bytes(std::span{wire.data(), wire.size()});
            const auto count = load(source, cursor, 4U); cursor += 4U;
            if (count > wire.size() - cursor) throw std::invalid_argument{"retained input truncated"};
            const auto result = wire.substr(cursor, static_cast<std::size_t>(count)); cursor += count; return result;
        };
        original_packet = block(retained_bytes, offset);
        retained_artifacts[6] = block(retained_bytes, offset);
        retained_artifacts[7] = block(retained_bytes, offset);
        retained_artifacts[8] = block(retained_bytes, offset);
        if (offset != retained_bytes.size() || original_packet.size() < 20U || original_packet.substr(0U, 8U) != "EBSINPUT")
            return false;
        std::size_t cursor = 20U;
        static_cast<void>(block(original_packet, cursor));
        if (original_packet.size() - cursor < 16U) return false;
        cursor += 16U;
        static_cast<void>(block(original_packet, cursor)); static_cast<void>(block(original_packet, cursor));
        if (original_packet.size() - cursor < 32U) return false;
        cursor += 32U; retained_build = block(original_packet, cursor);
        static_cast<void>(block(original_packet, cursor)); static_cast<void>(block(original_packet, cursor));
        for (unsigned count{}; count < 6U; ++count) static_cast<void>(block(original_packet, cursor));
        for (std::size_t count{}; count < std::tuple_size_v<MonitoringQ4GovernedArtifactSlots>;
             ++count) {
            static_cast<void>(block(original_packet, cursor));
            if (original_packet.size() - cursor < 32U) return false;
            cursor += 32U;
        }
        for (std::size_t index{}; index < 6U; ++index) retained_artifacts[index] = block(original_packet, cursor);
        static_cast<void>(block(original_packet, cursor)); static_cast<void>(block(original_packet, cursor));
        return cursor == original_packet.size() && binding->guard().has_value();
    }

    [[nodiscard]] auto input_error(std::string_view operation) -> core::Error {
        return {.category = core::ErrorCategory::monitoring,
            .code = core::StableErrorCode{"monitoring.source.input"},
            .message = "Source input acquisition refused", .operation = std::string{operation},
            .os_error = std::nullopt, .context = nlohmann::json::object(),
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_command};
    }
    [[nodiscard]] auto read_input(std::string_view path)
        -> std::expected<core::GovernedRepositoryFile, core::Error> {
        work::SourceInputPath selected;
        if (path == "config/monitoring-qualification/q0b/host-v2-inputs.json")
            selected = work::SourceInputPath::manifest;
        else if (path == "config/monitoring-qualification/host-v2-policy.json")
            selected = work::SourceInputPath::policy;
        else if (path == "config/monitoring-qualification/q0b/host-v2-input-template.json")
            selected = work::SourceInputPath::input_template;
        else return std::unexpected{input_error("source_input_path")};
        const auto bytes = input_reader->read(selected);
        if (!bytes) return std::unexpected{input_error("source_input_read")};
        auto file = core::detail::GovernedRepositorySourceAccess::make(
            std::filesystem::path{path}, std::string{*bytes});
        return file;
    }
    [[nodiscard]] auto transport_pause() noexcept -> bool {
        if (!binding->guard()) return false;
        const auto result = work::production_source_launch_stream_operations().pause(5);
        if (result.value < 0 && result.error_number != EINTR) {
            static_cast<void>(binding->fail(HostFrozenSourceLaunchReason::io,
                HostFrozenSourceBindingOperation::receive, result));
            return false;
        }
        return binding->guard().has_value();
    }
    [[nodiscard]] auto send_source(HostFrozenSourceLaunchRecord record,
                                  std::span<const int> rights = {}) noexcept -> bool {
        if (!binding->guard() || outgoing_sequence == UINT64_MAX - 1U) return false;
        record.sequence = outgoing_sequence;
        const auto identity = platform_linux::detail::production_topology_source_operations().pid(nullptr);
        if (identity.value <= 0 || identity.value > INT32_MAX || identity.error_number) return false;
        record.pid = static_cast<std::uint64_t>(identity.value);
        record.rights_count = static_cast<std::uint16_t>(rights.size());
        const auto encoded = encode_host_frozen_source_launch_record(record);
        if (!encoded) return false;
        for (;;) {
            if (!binding->guard()) return false;
            const auto result = binding->io_.send(binding->io_.context, binding->channel_, *encoded, rights);
            if (result.value == static_cast<long>(encoded->size()) && !result.error_number) {
                ++outgoing_sequence; return binding->guard().has_value();
            }
            if (result.value < 0 && (result.error_number == EAGAIN || result.error_number == EWOULDBLOCK ||
                                     result.error_number == EINTR)) {
                if (!transport_pause()) return false;
                continue;
            }
            static_cast<void>(binding->fail(HostFrozenSourceLaunchReason::io,
                HostFrozenSourceBindingOperation::send, result, binding->channel_));
            return false;
        }
    }
    [[nodiscard]] auto send_control(const FrozenProcessRecord& record) noexcept -> bool {
        auto bytes = encode_frozen_process_record(record);
        if (!bytes) return false;
        auto envelope = source_control_envelope(binding->grant_, *bytes);
        return envelope && send_source(*envelope);
    }
    [[nodiscard]] auto receive_control() noexcept -> std::optional<FrozenProcessRecord> {
        for (;;) {
            if (!binding->guard()) return std::nullopt;
            HostFrozenSourceLaunchBytes bytes{};
            HostFrozenSourceAncillary ancillary{};
            const auto result = binding->io_.receive(binding->io_.context, binding->channel_, bytes, &ancillary);
            // All receiver copies enter the precreated bank before any parse.
            for (std::size_t i{}; i < std::min(ancillary.count, binding->pending_rights_.size()); ++i) {
                const auto descriptor = ancillary.descriptors[i];
                const bool alias = descriptor < 0 || descriptor == root_descriptor || descriptor == output_descriptor ||
                    std::find(binding->borrowed_descriptors_.begin(), binding->borrowed_descriptors_.end(), descriptor)
                        != binding->borrowed_descriptors_.end() ||
                    std::any_of(binding->pending_rights_.begin(), binding->pending_rights_.end(),
                        [descriptor](const auto& right) noexcept { return right.descriptor == descriptor; });
                if (alias) ancillary.malformed = true;
                else binding->pending_rights_[binding->rights_count_++].descriptor = descriptor;
            }
            if (result.value < 0 && ancillary.count == 0 &&
                (result.error_number == EAGAIN || result.error_number == EWOULDBLOCK || result.error_number == EINTR)) {
                if (!transport_pause()) return std::nullopt;
                continue;
            }
            if (result.value != static_cast<long>(bytes.size()) || result.error_number || ancillary.count ||
                ancillary.truncated || ancillary.malformed || !ancillary.credentials ||
                ancillary.peer.pid != binding->parent_pid_ || ancillary.peer.uid != binding->uid_) {
                static_cast<void>(binding->fail(HostFrozenSourceLaunchReason::association,
                    HostFrozenSourceBindingOperation::receive, result, binding->channel_));
                static_cast<void>(binding->close_pending()); return std::nullopt;
            }
            const auto envelope = decode_host_frozen_source_launch_record(bytes);
            if (!envelope || envelope->sequence <= incoming_sequence ||
                envelope->role != binding->grant_.role || envelope->generation != binding->grant_.generation ||
                envelope->invocation != binding->grant_.invocation || envelope->build != binding->grant_.build ||
                envelope->source != binding->grant_.source || envelope->campaign != binding->grant_.campaign ||
                envelope->resources != binding->grant_.resources || envelope->phase != binding->grant_.phase ||
                envelope->phase_generation != binding->grant_.phase_generation ||
                envelope->origin_ns != binding->grant_.origin_ns || envelope->phase_end_ns != binding->grant_.phase_end_ns ||
                envelope->entry_end_ns != binding->grant_.entry_end_ns ||
                envelope->launch_origin_ns != binding->grant_.launch_origin_ns ||
                envelope->invocation_origin_ns != binding->grant_.invocation_origin_ns ||
                envelope->campaign_end_ns != binding->grant_.campaign_end_ns) return std::nullopt;
            const auto payload = source_control_payload(*envelope);
            if (!payload) return std::nullopt;
            const auto record = decode_frozen_process_record(*payload);
            if (!record) return std::nullopt;
            incoming_sequence = envelope->sequence;
            if (record->type == FrozenProcessType::permit) {
                binding->grant_.permit_end_ns = record->deadline_ns;
                control_permit_end.store(record->deadline_ns, std::memory_order_release);
            } else if (record->type == FrozenProcessType::ack) {
                binding->grant_.permit_end_ns = 0;
                control_permit_end.store(0, std::memory_order_release);
            }
            return *record;
        }
    }
    [[nodiscard]] auto settle_control() noexcept -> bool {
        while (control->status() == FrozenSourceControlStatus::pending) {
            if (!binding->guard() || !control->service_once() || !transport_pause()) return false;
        }
        return control->status() == FrozenSourceControlStatus::complete && control->service_once().has_value();
    }
    [[nodiscard]] auto cleanup() noexcept -> bool {
        if (admission_failure) {
            const auto& facts = admission_failure->authority_failure().cleanup;
            // A failed consuming retirement cannot release the hidden lower
            // owners or their live Binding/token borrows through stack unwind.
            if (facts.count || facts.overflowed) cleanup_uncertain = true;
        }
        if (control) {
            const auto result = control->retire({binding->grant_.phase_end_ns});
            if (result) control.reset(); else cleanup_uncertain = true;
        }
        if (running_build) {
            auto retired = running_build->retire();
            core::detail::ExpectedErrorCleanup cleanup_guard{retired};
            if (!retired) cleanup_uncertain = true;
            else running_build.reset();
        }
        if (input_reader) {
            if (!input_reader->retire()) cleanup_uncertain = true;
            else input_reader.reset();
        }
        const auto native = platform_linux::detail::production_topology_source_operations();
        for (std::size_t slot{}; slot < walking.size(); ++slot) static_cast<void>(close_walking(slot));
        for (auto* descriptor : std::array{&output_descriptor, &root_descriptor, &retained_descriptor, &qualification_descriptor}) {
            if (*descriptor < 0) continue;
            const auto value = std::exchange(*descriptor, -1);
            const auto closed = native.close(native.context, value);
            if (closed.value || closed.error_number) {
                cleanup_uncertain = true;
                static_cast<void>(binding->fail(HostFrozenSourceLaunchReason::uncertain,
                    HostFrozenSourceBindingOperation::close, closed, value));
            }
        }
        if (!cleanup_uncertain && input_lease && input_lease->active()) {
            if (!input_lease->retire()) cleanup_uncertain = true;
        }
        if (!cleanup_uncertain && evidence_lease && evidence_lease->active()) {
            if (!evidence_lease->retire()) cleanup_uncertain = true;
        }
        return !cleanup_uncertain;
    }

    auto prepare_packet(const HostFrozenSourceAdmission& admission, std::string_view policy,
                        const HostFrozenSourceLaunchRecord& source_binding)
        -> std::expected<void, HostFrozenSourceLaunchReason> {
        if (packet_lease || !packet.empty() || packet_sha || cancellation.requested())
            return std::unexpected{HostFrozenSourceLaunchReason::association};
        const auto values = admission.values();
        const auto source = admission.retained_source_provenance();
        const auto history = admission.retained_history();
        const auto governed = admission.governed_inputs();
        const auto q4 = admission.completed_q4_bytes();
        if (!values || !source || !history || !governed || !q4 || policy.empty() ||
            policy.size() > UINT64_C(1048576) || (*source)->current_build_identity.bytes() != source_binding.build ||
            (*governed)->captured_head() != (*history)->current_commit() ||
            (*values)->qualification_id().empty())
            return std::unexpected{HostFrozenSourceLaunchReason::association};
        const auto root = (*source)->accepted_repository_path.string();
        if (root.empty() || root.front() != '/' || root.size() > 511U ||
            (*values)->qualification_id().size() > 128U)
            return std::unexpected{HostFrozenSourceLaunchReason::capacity};
        auto profile = core::Sha256Digest::parse((*values)->profile_sha256());
        core::detail::ExpectedErrorCleanup profile_cleanup{profile};
        if (!profile || core::sha256(policy).to_hex() != (*values)->policy_sha256())
            return std::unexpected{HostFrozenSourceLaunchReason::association};
        auto original = nlohmann::json::parse(*q4, nullptr, false);
        core::detail::JsonCleanup original_cleanup{original};
        if (!original.is_object()) return std::unexpected{HostFrozenSourceLaunchReason::association};
        const auto provenance = original.find("provenance");
        if (provenance == original.end() || !provenance->is_object())
            return std::unexpected{HostFrozenSourceLaunchReason::association};
        const auto claim = provenance->find("build_identity");
        if (claim == provenance->end() || !claim->is_string())
            return std::unexpected{HostFrozenSourceLaunchReason::association};
        const auto& original_build = claim->get_ref<const std::string&>();
        if (original_build.empty() || original_build.size() > UINT64_C(1048576))
            return std::unexpected{HostFrozenSourceLaunchReason::capacity};

        const std::array history_commits{(*history)->q0_commit(), (*history)->q1_commit(),
            (*history)->q2_commit(), (*history)->q3_commit(), (*history)->current_commit(),
            (*history)->completed_q4_producer_commit()};
        const auto& inputs = **governed;
        const std::array<const MonitoringQ4GovernedArtifact*, 18> files{
            &inputs.profile(), &inputs.q1_plan(), &inputs.q2_plan(), &inputs.q2_plan_schema(),
            &inputs.q2_result_schema(), &inputs.q2_error_schema(), &inputs.q2_authored_schema(),
            &inputs.q3_plan(), &inputs.q3_plan_schema(), &inputs.q3_evidence_schema(),
            &inputs.q3_result_schema(), &inputs.q4_plan(), &inputs.q4_plan_schema(),
            &inputs.q4_evidence_schema(), &inputs.q4_result_schema(), &inputs.q4_error_schema(),
            &inputs.q4_authored_schema(), &inputs.current_q2_result_schema()};
        const std::array prior_selectors{HostFrozenPredecessorArtifact::environment,
            HostFrozenPredecessorArtifact::q0b, HostFrozenPredecessorArtifact::q1,
            HostFrozenPredecessorArtifact::q2, HostFrozenPredecessorArtifact::q3,
            HostFrozenPredecessorArtifact::manifest, HostFrozenPredecessorArtifact::checksums};
        std::array<std::string_view, 7> prior{};
        for (std::size_t index{}; index < prior.size(); ++index) {
            const auto item = admission.completed_predecessor_bytes(prior_selectors[index]);
            if (!item) return std::unexpected{HostFrozenSourceLaunchReason::association};
            prior[index] = *item;
        }
        constexpr std::size_t packet_limit = UINT64_C(67108864);
        std::size_t total = 20U + 16U + 32U;
        const auto account = [&](std::size_t bytes) noexcept {
            if (bytes > UINT32_MAX || total > packet_limit - 4U || bytes > packet_limit - total - 4U)
                return false;
            total += bytes + 4U; return true;
        };
        if (!account(root.size()) || !account((*values)->qualification_id().size()) ||
            !account((*history)->current_commit().size()) ||
            !account((*source)->current_build_bytes.size()) || !account(policy.size()) ||
            !account(original_build.size())) return std::unexpected{HostFrozenSourceLaunchReason::capacity};
        for (const auto oid : history_commits)
            if (!((oid.size() == 40U || oid.size() == 64U) &&
                std::all_of(oid.begin(), oid.end(), [](char c) {
                    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); })) || !account(oid.size()))
                return std::unexpected{HostFrozenSourceLaunchReason::association};
        for (const auto* file : files) {
            if (file->sha256 != core::sha256(file->bytes) || !account(file->bytes.size()) ||
                total > packet_limit - 32U)
                return std::unexpected{HostFrozenSourceLaunchReason::capacity};
            total += 32U;
        }
        for (const auto bytes : prior)
            if (!account(bytes.size())) return std::unexpected{HostFrozenSourceLaunchReason::capacity};
        if (!account(q4->size())) return std::unexpected{HostFrozenSourceLaunchReason::capacity};
        work::Resources storage{};
        // The string's payload and terminating byte are actual application
        // storage. Ordinary dependency metadata uses the approved reserve.
        storage.ordinary_bytes = core::ByteCount::from_base_units(total + 1U);
        storage.address_space_bytes = storage.ordinary_bytes;
        auto reserved = ledger.reserve(storage);
        if (!reserved) return std::unexpected{HostFrozenSourceLaunchReason::capacity};
        packet_lease.emplace(std::move(*reserved));
        packet.resize(total);
        auto output = std::as_writable_bytes(std::span{packet.data(), packet.size()});
        const std::array<std::byte, 8> packet_magic{std::byte{'E'}, std::byte{'B'}, std::byte{'S'},
            std::byte{'I'}, std::byte{'N'}, std::byte{'P'}, std::byte{'U'}, std::byte{'T'}};
        std::copy(packet_magic.begin(), packet_magic.end(), output.begin());
        store(output, 8U, 1U, 2U); store(output, 10U, 0U, 2U); store(output, 12U, total, 8U);
        std::size_t position = 20U;
        const auto block = [&](std::string_view bytes) noexcept {
            store(output, position, bytes.size(), 4U); position += 4U;
            std::memcpy(packet.data() + position, bytes.data(), bytes.size()); position += bytes.size();
        };
        const auto digest = [&](const core::Sha256Digest& sha) noexcept {
            std::copy(sha.bytes().begin(), sha.bytes().end(), output.begin() + static_cast<std::ptrdiff_t>(position));
            position += 32U;
        };
        block(root);
        store(output, position, (*source)->repository_identity.device, 8U); position += 8U;
        store(output, position, (*source)->repository_identity.inode, 8U); position += 8U;
        block((*values)->qualification_id()); block((*history)->current_commit()); digest(*profile);
        block((*source)->current_build_bytes); block(policy); block(original_build);
        for (const auto oid : history_commits) block(oid);
        for (const auto* file : files) { block(file->bytes); digest(file->sha256); }
        for (std::size_t index{}; index < 5U; ++index) block(prior[index]);
        block(*q4); block(prior[5]); block(prior[6]);
        if (position != packet.size()) return std::unexpected{HostFrozenSourceLaunchReason::capacity};
        packet_sha.emplace(core::sha256(packet));
        return {};
    }
};
auto host_frozen_source_retirement_tag(const MonitoringQ4AuthorityFailure& failure) noexcept
    -> std::array<char, 50> {
    std::array<char, 50> tag{};
    if (failure.stage != MonitoringQ4AuthorityStage::retirement) return tag;
    // Lowercase hexadecimal keeps every scalar within the parent's 50-byte
    // projection. Native error is its unsigned 32-bit representation; ff source
    // and kind with count zero mean no first fact. The original ledger is untouched.
    static_assert(sizeof(unsigned) == 4U && sizeof(std::size_t) <= 8U);
    const auto* first = failure.cleanup.count == 0U ? nullptr : &failure.cleanup.facts.front();
    static_cast<void>(std::snprintf(tag.data(), tag.size(), "ar_s%xk%xd%xn%xc%zxo%u",
        first ? static_cast<unsigned>(first->source) : 255U,
        first ? static_cast<unsigned>(first->kind) : 255U,
        first ? first->detail : 0U, first ? static_cast<unsigned>(first->native_error) : 0U,
        failure.cleanup.count, static_cast<unsigned>(failure.cleanup.overflowed)));
    return tag;
}
auto host_frozen_source_authority_tag(const MonitoringQ4AuthorityFailure& failure) noexcept
    -> std::array<char, 50> {
    if (failure.stage != MonitoringQ4AuthorityStage::final_recheck)
        return host_frozen_source_retirement_tag(failure);
    unsigned typed_kind{255U};
    int native = failure.primary.native_error;
    const core::Error* error{};
    if (const auto* git = std::get_if<MonitoringQ1GitCommandFailure>(&failure.detail))
        error = &git->primary;
    else if (const auto* governed = std::get_if<MonitoringQ4GovernedInputFailure>(&failure.detail)) {
        typed_kind = static_cast<unsigned>(governed->kind);
        if (!native) native = governed->native_error;
    } else error = std::get_if<core::Error>(&failure.detail);
    if (error) {
        typed_kind = static_cast<unsigned>(error->category);
        if (!native && error->os_error) native = error->os_error->value;
    }
    std::array<char, 50> tag{};
    // Even full unsigned widths fit before the unchanged parent truncation.
    // These are best-available closed facts, never a reconstructed error string.
    static_assert(sizeof(unsigned) == 4U &&
        std::variant_size_v<MonitoringQ4AuthorityFailure::Detail> <= 16U);
    static_cast<void>(std::snprintf(tag.data(), tag.size(), "af_a%xs%xk%xd%xv%xt%xn%x",
        static_cast<unsigned>(failure.stage), static_cast<unsigned>(failure.primary.source),
        static_cast<unsigned>(failure.primary.kind), failure.primary.detail,
        static_cast<unsigned>(failure.detail.index()), typed_kind, static_cast<unsigned>(native)));
    return tag;
}

auto host_frozen_source_headroom_tag(const work::Failure& failure, unsigned preparation_step,
    HostFrozenSourceLaunchReason outer_primary) noexcept -> std::array<char, 50> {
    std::array<char, 50> tag{};
    std::array<unsigned, 4> cleanup{255U, 255U, 255U, 255U};
    static_assert(sizeof(unsigned) == 4U && sizeof(std::size_t) <= 8U);
    static_assert(std::tuple_size_v<decltype(failure.cleanup)> == 4U);
    for (std::size_t index{}; index < std::min(failure.cleanup_count, failure.cleanup.size()); ++index)
        cleanup[index] = static_cast<unsigned>(failure.cleanup[index]);
    // Lowercase hex preserves even an invalid count without indexing beyond the
    // fixed owner. ff means an absent cleanup slot, distinguished by the count.
    // The widest scalar token is48 bytes, before the parent's50-byte projection.
    static_cast<void>(std::snprintf(tag.data(), tag.size(), "ah_s%xp%xc%zx_%x_%x_%x_%x_r%x",
        preparation_step, static_cast<unsigned>(failure.primary), failure.cleanup_count,
        cleanup[0], cleanup[1], cleanup[2], cleanup[3], static_cast<unsigned>(outer_primary)));
    return tag;
}

auto host_frozen_source_admission_tag(HostFrozenAdmissionReason reason,
    HostFrozenBindingCheckpoint checkpoint, const core::Error* lower, unsigned preparation_step,
    HostFrozenSourceLaunchReason outer_primary) noexcept -> std::array<char, 50> {
    std::array<char, 50> tag{};
    std::array<char, 25> code{};
    if (lower) {
        const auto& input = lower->code.value();
        const auto count = std::min(input.size(), code.size() - 1U);
        const auto offset = input.size() - count;
        for (std::size_t index{}; index < count; ++index) {
            const auto value = input[offset + index];
            code[index] = value >= 'A' && value <= 'Z' ? static_cast<char>(value - 'A' + 'a') :
                ((value >= 'a' && value <= 'z') || (value >= '0' && value <= '9') ? value : '_');
        }
    }
    const auto byte = [](unsigned value) noexcept { return value <= 255U ? value : 255U; };
    // At most49 bytes: 8 hex stage digits, four 2-digit enum slots and the
    // existing24-character sanitized lower-code suffix. ff marks unknown.
    static_assert(sizeof(unsigned) == 4U);
    static_cast<void>(std::snprintf(tag.data(), tag.size(), "aa_s%xr%xb%xc%xo%x_%s",
        preparation_step, byte(static_cast<unsigned>(reason)), static_cast<unsigned>(checkpoint),
        lower ? byte(static_cast<unsigned>(lower->category)) : 255U,
        static_cast<unsigned>(outer_primary), code[0] ? code.data() : "none"));
    return tag;
}

auto HostFrozenSourceLaunchState::collect(int channel, int parent, int output, int retained, int qualification,
    HostFrozenSourceLaunchRecord grant, const char* root, const char* qualification_id,
    runtime::ControllerProcessProductionAuthority& authority,
    runtime::RunnerCancellation& cancellation, HostFrozenSourceLaunchClock& clock,
    const HostFrozenSourceSyntheticDispatcher* synthetic) noexcept -> int {
    const auto streams = work::production_source_launch_stream_operations();
    const auto io = production_host_frozen_source_launch_io(clock);
    // Q5 has no retained-Q5 or qualification-directory descriptors. Register
    // only actual owners; absent optional inputs are not invalid FD borrows.
    std::array<int, 4> input_borrows{channel, output, -1, -1};
    std::size_t input_borrow_count{2U};
    if (retained >= 0) input_borrows[input_borrow_count++] = retained;
    if (qualification >= 0) input_borrows[input_borrow_count++] = qualification;
    const auto actual_input_borrows = std::span{input_borrows}.first(input_borrow_count);
    const auto borrowed = actual_input_borrows.subspan(1U);
    HostFrozenSourceLaunchBinding binding{channel, parent, streams.effective_uid(), cancellation,
        io, {}, grant, borrowed};
    unsigned preparation_step{1U}, authority_stage{255U}, core_category{255U};
    unsigned control_reason{255U}, collection_reason{255U}, pairwise_code{255U}, pairwise_reason{255U};
    unsigned monitoring_channel{255U};
    HostFrozenSourceLaunchReason refused_reason{HostFrozenSourceLaunchReason::contained};
    std::array<char, 25> core_code{};
    std::array<char, 50> authority_diagnostic{};
    std::array<char, 50> headroom_diagnostic{};
    std::array<char, 50> admission_diagnostic{};
    const auto retain_core = [&](const core::Error& error) noexcept {
        core_category = static_cast<unsigned>(error.category);
        const auto& code = error.code.value();
        const auto count = std::min(code.size(), core_code.size() - 1U);
        const auto offset = code.size() - count;
        for (std::size_t index{}; index < count; ++index) {
            const auto value = code[offset + index];
            core_code[index] = value >= 'A' && value <= 'Z' ? static_cast<char>(value - 'A' + 'a') :
                ((value >= 'a' && value <= 'z') || (value >= '0' && value <= '9') ? value : '_');
        }
    };
    const auto diagnose = [&]() noexcept {
        if (authority_diagnostic[0]) {
            std::fprintf(stderr, "source_collector_refusal operation=%s_s%u_a%u\n",
                authority_diagnostic.data(), preparation_step, authority_stage);
            return;
        }
        if (headroom_diagnostic[0]) {
            std::fprintf(stderr, "source_collector_refusal operation=%s\n", headroom_diagnostic.data());
            return;
        }
        if (admission_diagnostic[0]) {
            std::fprintf(stderr, "source_collector_refusal operation=%s\n", admission_diagnostic.data());
            return;
        }
        const auto& fact = binding.first_raw_fact();
        const long value = fact ? fact->native_result : 0;
        const auto magnitude = value < 0 ? 0UL - static_cast<unsigned long>(value) :
                                         static_cast<unsigned long>(value);
        // Closed scalar/typed facts only. No error message, input or pathname
        // is printed, and preparation failure never becomes a FINISH.
        std::fprintf(stderr, "source_collector_refusal operation=s%u_k%u_f%u_x%u_y%u_m%u_r%u_o%u_v%c%lu_e%d_a%u_c%u_%s\n",
            preparation_step, control_reason, collection_reason, pairwise_code, pairwise_reason, monitoring_channel,
            static_cast<unsigned>(binding.raw_primary().value_or(refused_reason)),
            fact ? static_cast<unsigned>(fact->operation) : 255U, value < 0 ? 'n' : 'p', magnitude,
            fact ? fact->error_number : 0, authority_stage, core_category,
            core_code[0] ? core_code.data() : "none");
    };
    const auto refuse_before_state = [&]() noexcept -> int {
        diagnose();
        bool uncertain{};
        for (const int descriptor : borrowed) {
            if (descriptor < 0) continue;
            const auto closed = io.close(io.context, descriptor);
            if (closed.value || closed.error_number) uncertain = true;
        }
        const auto closed_binding = binding.retire();
        if (uncertain || !closed_binding) streams.retain_until_outer_termination();
        return 78;
    };
    // The inherited channel/output copies are enclosed by the authenticated
    // parent's existing launch reservation. No byte authority is inferred from
    // the old five-field control GRANT.
    if (!binding.authenticate()) return refuse_before_state();
    preparation_step = 2U;
    if (synthetic && (!synthetic->admit || !synthetic->acquire || !synthetic->start ||
        !synthetic->cell || !synthetic->termination || !synthetic->build.manifest ||
        !synthetic->build.fault)) {
        refused_reason = HostFrozenSourceLaunchReason::association;
        return refuse_before_state();
    }
    preparation_step = 3U;
    auto quota = binding.child_quota();
    if (!quota) { refused_reason = quota.error(); return refuse_before_state(); }
    preparation_step = 4U;
    auto ledger = quota->claim_ledger();
    if (!ledger) { refused_reason = HostFrozenSourceLaunchReason::capacity; return refuse_before_state(); }
    HostFrozenSourceLaunchState state{authority, cancellation, clock, *ledger};
    state.binding = &binding;
    state.output_descriptor = output; // Owner registration precedes all preparation.
    state.retained_descriptor = retained; state.qualification_descriptor = qualification;
    state.last = grant.origin_ns;
    state.control_last = grant.origin_ns;
    state.original_phase_end = grant.phase_end_ns;
    int outcome = 78;
    try {
        preparation_step = 5U;
        work::Resources bootstrap{};
        bootstrap.descriptors = 24U; // channel/output/root, retained copies/walk roles, sixteen-right bank.
        bootstrap.ordinary_bytes = core::ByteCount::from_base_units(state.input_scratch.size());
        bootstrap.address_space_bytes = bootstrap.ordinary_bytes;
        auto reserved = ledger->reserve(bootstrap);
        if (!reserved) throw HostFrozenSourceLaunchReason::capacity;
        state.input_lease.emplace(std::move(*reserved));
        // Keep module discovery inside the original launch fence, after all
        // inherited descriptors have owners and before source/native workers.
        if (!synthetic) {
            if (!binding.guard()) throw HostFrozenSourceLaunchReason::deadline;
            if (!runtime::initialize_qualification_kernel_modules()) {
                constexpr std::string_view diagnostic{"kernel_module_initialization"};
                std::copy(diagnostic.begin(), diagnostic.end(), admission_diagnostic.begin());
                throw HostFrozenSourceLaunchReason::association;
            }
            if (!binding.guard()) throw HostFrozenSourceLaunchReason::deadline;
        }
        state.repository_path = root;
        state.qualification_path = state.repository_path / ".local" / "monitoring-qualification" / qualification_id;
        auto launched = grant;
        launched.type = HostFrozenSourceLaunchType::launched;
        if (!state.send_source(launched)) throw HostFrozenSourceLaunchReason::io;

        preparation_step = 6U;
        const auto native = platform_linux::detail::production_topology_source_operations();
        if (!binding.guard()) throw HostFrozenSourceLaunchReason::deadline;
        const auto opened = native.open_root(native.context, root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (opened.value >= 0 && opened.value <= INT32_MAX)
            state.root_descriptor = static_cast<int>(opened.value);
        if (opened.value < 3 || opened.value > INT32_MAX || opened.error_number) {
            static_cast<void>(binding.fail(HostFrozenSourceLaunchReason::native,
                HostFrozenSourceBindingOperation::open, opened));
            throw HostFrozenSourceLaunchReason::native;
        }
        struct stat root_identity{};
        const auto metadata = native.stat_fd(native.context, state.root_descriptor, &root_identity);
        if (metadata.value || metadata.error_number) {
            static_cast<void>(binding.fail(HostFrozenSourceLaunchReason::native,
                HostFrozenSourceBindingOperation::metadata, metadata, state.root_descriptor));
            throw HostFrozenSourceLaunchReason::native;
        }
        if (!S_ISDIR(root_identity.st_mode) || !binding.guard())
            throw HostFrozenSourceLaunchReason::association;
        state.input_reader.emplace(*ledger, io.guard, native, state.root_descriptor,
            work::SourceInputRootIdentity{static_cast<std::uint64_t>(root_identity.st_dev),
                static_cast<std::uint64_t>(root_identity.st_ino)}, state.input_scratch,
            grant.origin_ns, grant.phase_end_ns, state.last, &clock.current, actual_input_borrows);
        detail_MonitoringHostInputTemplateAccess::SourceOperations source_input{
            &state,
            +[](void* raw, std::string_view path) {
                return static_cast<HostFrozenSourceLaunchState*>(raw)->read_input(path);
            },
            +[](void* raw) -> std::expected<void, core::Error> {
                auto& current = *static_cast<HostFrozenSourceLaunchState*>(raw);
                if (!current.binding->guard()) return std::unexpected{current.input_error("source_input_guard")};
                return {};
            }};
        preparation_step = 7U;
        auto input = detail_MonitoringHostInputTemplateAccess::load_source_fallible(source_input, state.raw_input_failure);
        core::detail::ExpectedErrorCleanup input_cleanup{input};
        if (!input) {
            retain_core(state.raw_input_failure ? *state.raw_input_failure : input.error());
            throw HostFrozenSourceLaunchReason::io;
        }
        if (!state.input_reader->retire()) throw HostFrozenSourceLaunchReason::io;
        state.input_reader.reset();
        preparation_step = 8U;
        auto admission = [&]() -> std::expected<HostFrozenSourceAdmission, HostFrozenSourceAdmissionFailure> {
            if (grant.phase == work::SourceLaunchPhase::q5)
                return synthetic ? synthetic->admit(synthetic->context, binding, state.repository_path,
                    qualification_id, nullptr) : HostFrozenSourceAdmission::authenticate_source(
                        state.repository_path, qualification_id, binding);
            if (grant.phase != work::SourceLaunchPhase::q6 || !state.read_retained_input())
                throw HostFrozenSourceLaunchReason::association;
            std::array<char, 64> build_hex{};
            for (std::size_t index{}; index < grant.build.size(); ++index) {
                const auto byte = std::to_integer<unsigned>(grant.build[index]);
                build_hex[2U * index] = hexadecimal[byte >> 4U];
                build_hex[2U * index + 1U] = hexadecimal[byte & 15U];
            }
            auto build_identity = core::Sha256Digest::parse(std::string_view{build_hex.data(), build_hex.size()});
            core::detail::ExpectedErrorCleanup identity_cleanup{build_identity};
            if (!build_identity) throw HostFrozenSourceLaunchReason::association;
            HostFrozenSourceRetainedInput retained_input{binding, state.original_packet,
                state.retained_artifacts[6], state.retained_artifacts[7], state.retained_artifacts[8],
                state.repository_path, static_cast<std::uint64_t>(root_identity.st_dev),
                static_cast<std::uint64_t>(root_identity.st_ino), state.qualification_path,
                state.qualification_device, state.qualification_inode, *build_identity,
                state.retained_build, &state,
                +[](void* raw, const std::filesystem::path& path, std::uint64_t device, std::uint64_t inode) noexcept {
                    return static_cast<HostFrozenSourceLaunchState*>(raw)->recheck_retained(path, device, inode);
                }};
            return synthetic ? synthetic->admit(synthetic->context, binding, state.repository_path,
                qualification_id, &retained_input) :
                HostFrozenSourceAdmission::authenticate_retained_source(retained_input);
        }();
        if (!admission) {
            state.admission_failure.emplace(std::move(admission.error()));
            const auto& failure = state.admission_failure->authority_failure();
            authority_stage = static_cast<unsigned>(failure.stage);
            authority_diagnostic = host_frozen_source_authority_tag(failure);
            if (const auto* error = std::get_if<core::Error>(&failure.detail)) retain_core(*error);
            throw HostFrozenSourceLaunchReason::association;
        }
        preparation_step = 9U;
        const auto public_values = admission->values();
        const auto provenance = admission->retained_source_provenance();
        const auto history = admission->retained_history();
        if (!public_values || !provenance || !history) throw HostFrozenSourceLaunchReason::association;
        preparation_step = 10U;
        auto build = synthetic ? MonitoringRunningBuildAuthority::retain_for_composition(
            (*history)->current_commit(), synthetic->build) :
            MonitoringRunningBuildAuthority::retain((*history)->current_commit());
        core::detail::ExpectedErrorCleanup build_cleanup{build};
        if (!build) { retain_core(build.error()); throw HostFrozenSourceLaunchReason::association; }
        if (build->identity().bytes() != grant.build) throw HostFrozenSourceLaunchReason::association;
        state.running_build.emplace(std::move(*build));
        preparation_step = 11U;
        if (!state.prepare_packet(*admission, input->policy_file().bytes(), grant))
            throw HostFrozenSourceLaunchReason::capacity;
        const auto writer = platform_linux::host_baseline_file::production_operations();
        std::size_t written{};
        while (written < state.packet.size()) {
            if (!binding.guard()) throw HostFrozenSourceLaunchReason::deadline;
            const auto bytes = std::as_bytes(std::span{state.packet.data(), state.packet.size()}).subspan(written);
            const auto result = writer.write_at(writer.context, output,
                bytes.first(std::min(bytes.size(), std::size_t{65536})), written);
            if (result.value <= 0 || result.error_number || static_cast<std::size_t>(result.value) > bytes.size()) {
                static_cast<void>(binding.fail(HostFrozenSourceLaunchReason::io,
                    HostFrozenSourceBindingOperation::send, result, output));
                throw HostFrozenSourceLaunchReason::io;
            }
            written += static_cast<std::size_t>(result.value);
        }
        const auto synced = writer.sync(writer.context, output);
        if (synced.value || synced.error_number) {
            static_cast<void>(binding.fail(HostFrozenSourceLaunchReason::native,
                HostFrozenSourceBindingOperation::sync, synced, output));
            throw HostFrozenSourceLaunchReason::io;
        }
        if (!binding.guard()) throw HostFrozenSourceLaunchReason::deadline;
        auto packet_record = grant;
        packet_record.type = HostFrozenSourceLaunchType::ready;
        packet_record.selector = host_frozen_source_input_selector;
        packet_record.stdout_cap = state.packet.size();
        const auto packet_hash = state.packet_sha->to_hex();
        packet_record.path_bytes = static_cast<std::uint16_t>(packet_hash.size());
        std::copy(packet_hash.begin(), packet_hash.end(), packet_record.path.begin());
        const std::array packet_right{output};
        if (!state.send_source(packet_record, packet_right)) throw HostFrozenSourceLaunchReason::io;
        preparation_step = 12U;
        const auto original_grant = state.receive_control();
        if (!original_grant || original_grant->type != FrozenProcessType::grant)
            throw HostFrozenSourceLaunchReason::association;
        const auto limits = host_frozen_source_linux_limits(grant.phase);
        if (!limits) throw HostFrozenSourceLaunchReason::capacity;
        preparation_step = 13U;
        auto control = synthetic ? synthetic->acquire(synthetic->context, std::move(*admission),
            std::move(*input), authority.request_ids(), binding, *original_grant, *ledger, cancellation) :
            FrozenSourceControlOwner::acquire_source_on_original_main(
            std::move(*admission), std::move(*input), authority.request_ids(), binding,
            static_cast<std::uint8_t>(grant.phase), grant.resources.measurement_workers,
            *original_grant, *ledger, cancellation, 65536U, *limits);
        if (!control) throw HostFrozenSourceLaunchReason::native;
        state.control.emplace(std::move(*control));
        HostFrozenCollectionDependencies dependencies{
            {runtime::detail::PairwisePlatformLeafFacade::production(),
             runtime::detail::PairwiseKernelLeafFacade{},
             runtime::detail::PairwiseWorkerOwningFacade::production_dependencies()},
            production_host_frozen_capacity_operations(), work::production_read_operations(),
            io.guard, measurement::detail::production_raw_monotonic_clock_operations()};
        dependencies.headroom.guard = state.run_guard();
        dependencies.run.source_capacity_from_admission = true;
        dependencies.run.sources.temperature_operations = platform_linux::detail::production_bounded_sysfs_operations();
        dependencies.run.sources.frequency_operations = platform_linux::detail::production_bounded_sysfs_operations();
        preparation_step = 14U;
        const auto started = synthetic ? synthetic->start(synthetic->context, *state.control) :
                                        state.control->start_source_control(dependencies);
        if (!started || !state.control->enqueue_prepare() ||
            !state.settle_control()) throw HostFrozenSourceLaunchReason::native;
        auto ready = state.control->ready();
        if (!ready || !state.send_control(*ready)) throw HostFrozenSourceLaunchReason::io;
        preparation_step = 15U;
        for (;;) {
            preparation_step = 151U;
            const auto snapshot = state.control->snapshot();
            if (!snapshot || snapshot->failure) throw HostFrozenSourceLaunchReason::native;
            if (snapshot->completed) break;
            preparation_step = 152U;
            auto begin = state.control->begin();
            if (!begin || !state.send_control(*begin)) throw HostFrozenSourceLaunchReason::io;
            const auto permit = state.receive_control();
            preparation_step = 153U;
            if (!permit || permit->type != FrozenProcessType::permit ||
                !state.control->enqueue_next(*permit))
                throw HostFrozenSourceLaunchReason::native;
            preparation_step = 154U;
            if (!state.settle_control()) throw HostFrozenSourceLaunchReason::native;
            preparation_step = 155U;
            const auto* cell = state.control->cell();
            if (!cell) throw HostFrozenSourceLaunchReason::native;
            work::Resources evidence_demand{};
            evidence_demand.ordinary_bytes = core::ByteCount::from_base_units(2U * HostFrozenCellEvidence::hard_limit);
            evidence_demand.address_space_bytes = evidence_demand.ordinary_bytes;
            auto reserved_evidence = ledger->reserve(evidence_demand);
            if (!reserved_evidence) throw HostFrozenSourceLaunchReason::capacity;
            state.evidence_lease.emplace(std::move(*reserved_evidence));
            // Projection and owning byte transport occur after actual Run
            // retirement. They never enter the timed/prepared critical path.
            preparation_step = 156U;
            auto projected = synthetic ? synthetic->cell(synthetic->context, *cell, **public_values,
                *state.running_build, HostFrozenCellEvidence::hard_limit) :
                HostFrozenCellEvidence::create(*cell, **public_values, *state.running_build);
            if (!projected) throw HostFrozenSourceLaunchReason::native;
            std::optional<HostFrozenCellEvidence> evidence{std::move(*projected)};
            const auto view = evidence->view();
            if (!view) throw HostFrozenSourceLaunchReason::native;
            preparation_step = 157U;
            auto payload = state.control->payload(FrozenProcessType::cell, view->canonical_json.size(),
                core::sha256(view->canonical_json).bytes());
            if (!payload || !state.send_control(*payload) || !binding.write_payload(*payload,
                    view->canonical_json, work::production_source_launch_stream_operations()))
                throw HostFrozenSourceLaunchReason::io;
            const auto acknowledgment = state.receive_control();
            if (!acknowledgment || acknowledgment->type != FrozenProcessType::ack)
                throw HostFrozenSourceLaunchReason::association;
            // End every serializer borrow/buffer before consuming the Cell.
            evidence.reset();
            if (!state.evidence_lease->retire()) throw HostFrozenSourceLaunchReason::uncertain;
            state.evidence_lease.reset();
            preparation_step = 158U;
            if (!state.control->enqueue_consume(*acknowledgment) ||
                !state.settle_control()) throw HostFrozenSourceLaunchReason::native;
        }
        preparation_step = 16U;
        const auto* collection = state.control->quiescent_collection();
        if (!collection) throw HostFrozenSourceLaunchReason::native;
        work::Resources termination_demand{};
        termination_demand.ordinary_bytes = core::ByteCount::from_base_units(2U * HostFrozenCellEvidence::hard_limit);
        termination_demand.address_space_bytes = termination_demand.ordinary_bytes;
        auto termination_reservation = ledger->reserve(termination_demand);
        if (!termination_reservation) throw HostFrozenSourceLaunchReason::capacity;
        state.evidence_lease.emplace(std::move(*termination_reservation));
        auto projected_termination = synthetic ? synthetic->termination(synthetic->context, *collection,
            nullptr, **public_values, *state.running_build, HostFrozenCellEvidence::hard_limit) :
            HostFrozenTerminationEvidence::create(*collection, nullptr, **public_values, *state.running_build);
        if (!projected_termination) throw HostFrozenSourceLaunchReason::native;
        std::optional<HostFrozenTerminationEvidence> termination{std::move(*projected_termination)};
        if (!termination || !termination->view()) throw HostFrozenSourceLaunchReason::native;
        const auto view = *termination->view();
        auto finish = state.control->finish(view.canonical_json.size(), core::sha256(view.canonical_json).bytes());
        if (!finish || !state.send_control(*finish) || !binding.write_payload(*finish,
                view.canonical_json, work::production_source_launch_stream_operations()))
            throw HostFrozenSourceLaunchReason::io;
        const auto acknowledgment = state.receive_control();
        if (!acknowledgment) throw HostFrozenSourceLaunchReason::association;
        const auto terminal = state.control->finish_acknowledgment(*acknowledgment);
        if (!terminal) throw HostFrozenSourceLaunchReason::association;
        termination.reset();
        preparation_step = 17U;
        if (!state.cleanup() || !state.send_control(*terminal)) throw HostFrozenSourceLaunchReason::uncertain;
        // The owning controller must consume this credential-bound terminal
        // while the genuine collector PID remains alive. Exit/reap follows
        // this acknowledgment; it never provides a completed-phase receipt.
        const auto terminal_ack = state.receive_control();
        if (!terminal_ack || acknowledgment->sequence == UINT32_MAX ||
            *terminal_ack != FrozenProcessRecord{FrozenProcessType::terminal, terminal->phase,
                acknowledgment->sequence + 1U, terminal->ordinal, 0, 0, 0, 0, {}})
            throw HostFrozenSourceLaunchReason::association;
        outcome = 0;
    } catch (HostFrozenSourceLaunchReason reason) {
        refused_reason = reason;
        outcome = 78;
    } catch (...) {
        // Allocation/dependency failure leaves an incomplete producer prefix;
        // it can never mint a successful FINISH or qualify a measurement.
        outcome = 78;
    }
    if (outcome) {
        // Read only published, quiescent failure facts before cleanup. These
        // scalar diagnostics cannot grant phase completion or success.
        if (state.control && state.control->status() != FrozenSourceControlStatus::pending) {
            const auto reason = state.control->failure_reason();
            if (reason) control_reason = static_cast<unsigned>(*reason);
            if (const auto* failure = state.control->collection_failure()) {
                collection_reason = static_cast<unsigned>(failure->reason());
                if (const auto* headroom = failure->headroom_failure())
                    headroom_diagnostic = host_frozen_source_headroom_tag(*headroom, preparation_step,
                        binding.raw_primary().value_or(refused_reason));
                if (const auto* error = failure->lower_error()) retain_core(*error);
                if (const auto admission_reason = failure->admission_reason())
                    admission_diagnostic = host_frozen_source_admission_tag(*admission_reason,
                        failure->binding_checkpoint(), failure->lower_error(), preparation_step,
                        binding.raw_primary().value_or(refused_reason));
                if (const auto* terminal_facts = failure->terminal_facts();
                    terminal_facts && !terminal_facts->failures.empty())
                    std::visit([&](const auto& error) noexcept {
                        pairwise_code = static_cast<unsigned>(error.code());
                        pairwise_reason = static_cast<unsigned>(error.reason());
                        if (const auto* context = std::get_if<runtime::MonitoringChannelContext>(&error.context()))
                            monitoring_channel = static_cast<unsigned>(context->channel);
                    }, terminal_facts->failures.front());
                else if (const auto* projected = failure->projection_failure()) {
                    pairwise_code = static_cast<unsigned>(projected->code());
                    pairwise_reason = static_cast<unsigned>(projected->reason());
                }
            }
        }
        diagnose(); // Preserve the original refusal before cleanup samples/closes.
    }
    const bool state_retired = state.cleanup();
    const auto binding_retired = binding.retire();
    if (!state_retired || !binding_retired) streams.retain_until_outer_termination();
    return outcome;
}
namespace {
auto source_descriptor_argument(const char* value) noexcept -> bool {
    if (!value) return false;
    const std::string_view text{value};
    if (text.empty() || text.size() > 10U) return false;
    int descriptor{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), descriptor);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && descriptor >= 3;
}
}
auto decode_host_frozen_source_governed_handoff(std::string_view bytes,
    std::uint64_t q5_origin, std::uint64_t launch_origin, std::uint64_t launch_end,
    std::uint64_t original_main_entry, std::string_view qualification_id) noexcept
    -> std::expected<HostFrozenSourceGovernedHandoff, HostFrozenSourceLaunchReason> {
    using Reason = HostFrozenSourceLaunchReason;
    try {
        constexpr std::array<std::string_view, 10> keys{"schema", "machine_plan_sha256", "policy_sha256",
            "ready_bytes_hex", "exposure_used_ns", "window_ordinal", "q4_count", "execution_seal_sha256",
            "source", "build_manifest_sha256"};
        const auto value = closed_governed_object(bytes, keys);
        const auto text = [&](std::string_view key) -> const std::string& {
            return value.at(key).get_ref<const std::string&>();
        };
        if (text("schema") != "epycbench.private-campaign-native-handoff/v1" ||
            (text("machine_plan_sha256") != governed_plan_sha &&
             text("machine_plan_sha256") != post_reboot_plan_sha &&
             text("machine_plan_sha256") != governed_reboot_plan_sha &&
             text("machine_plan_sha256") != environment_recovery_plan_sha &&
             text("machine_plan_sha256") != current_source_plan_sha) || text("policy_sha256") != governed_policy_sha ||
            !governed_hex(text("source"), 40U) || !governed_hex(text("execution_seal_sha256"), 64U) ||
            !governed_hex(text("build_manifest_sha256"), 64U)) return std::unexpected{Reason::association};
        const auto used = governed_unsigned(value, "exposure_used_ns");
        const auto ordinal = governed_unsigned(value, "window_ordinal");
        const auto q4 = governed_unsigned(value, "q4_count");
        const bool environment_recovery = text("machine_plan_sha256") == environment_recovery_plan_sha;
        const bool current_source = text("machine_plan_sha256") == current_source_plan_sha;
        const bool governed_reboot = text("machine_plan_sha256") == governed_reboot_plan_sha;
        const bool post_reboot = current_source || environment_recovery || governed_reboot || text("machine_plan_sha256") == post_reboot_plan_sha;
        if ((!qualification_id.empty() && qualification_id != (current_source ? current_source_qid : environment_recovery ? environment_recovery_qid : governed_reboot ? governed_reboot_qid : post_reboot ? post_reboot_qid : governed_qid)) ||
            (current_source ? q4 != 13U : environment_recovery ? q4 != 12U : governed_reboot ? q4 != 11U : post_reboot ? q4 != 10U : q4 == 10U)) return std::unexpected{Reason::association};
        // Historical floors stay fixed. The new current-source Q5 handoff
        // requires a charged Q1-Q4 window before the separately guarded Q5.
        if (used >= policy_exposure || ordinal < 1U || ordinal > 184U || q4 < 6U || q4 > (current_source ? 13U : environment_recovery ? 12U : governed_reboot ? 11U : 10U) ||
            (ordinal == 1U && used != 0) ||
            (post_reboot && (ordinal < (current_source ? 99U : environment_recovery ? 60U : governed_reboot ? 38U : 26U) ||
                used < (current_source ? UINT64_C(34240741310288) : environment_recovery ? UINT64_C(26616961183234) : governed_reboot ? UINT64_C(18076634376338) : UINT64_C(12243829713200)) ||
                (current_source && used == UINT64_C(34240741310288)))))
            return std::unexpected{Reason::capacity};
        const auto& hex = text("ready_bytes_hex");
        if (hex.empty() || hex.size() % 2U || hex.size() > 4096U || !governed_hex(hex, hex.size()))
            return std::unexpected{Reason::malformed};
        std::array<char, 2048> ready_bank{};
        for (std::size_t index{}; index < hex.size() / 2U; ++index)
            ready_bank[index] = static_cast<char>((hexadecimal.find(hex[2U * index]) << 4U) |
                hexadecimal.find(hex[2U * index + 1U]));
        const std::string_view raw_ready{ready_bank.data(), hex.size() / 2U};
        if (!raw_ready.ends_with('\n') || raw_ready.find('\n') != raw_ready.size() - 1U)
            return std::unexpected{Reason::malformed};
        constexpr std::array<std::string_view, 7> ready_keys{"event", "clock", "expected_policy",
            "temporary_policy", "origin_ns", "work_end_ns", "window_end_ns"};
        const auto ready = closed_governed_object(raw_ready, ready_keys);
        const auto origin = governed_unsigned(ready, "origin_ns");
        const auto work_end = governed_unsigned(ready, "work_end_ns");
        const auto window_end = governed_unsigned(ready, "window_end_ns");
        const auto original_policy = governed_unsigned(ready, "expected_policy");
        if (ready.at("event") != "ready" || ready.at("clock") != "CLOCK_MONOTONIC_RAW" ||
            governed_unsigned(ready, "temporary_policy") != 1U || original_policy <= 1U || original_policy > INT32_MAX)
            return std::unexpected{Reason::association};
        if (!origin || origin > q5_origin || work_end <= origin || window_end <= work_end ||
            window_end - work_end != policy_restore ||
            window_end - origin > UINT64_C(10800000000000) ||
            window_end - origin > policy_exposure - used)
            return std::unexpected{Reason::deadline};
        if (!q5_origin || q5_origin != launch_origin || q5_origin > UINT64_MAX - UINT64_C(5000000000) ||
            launch_end != std::min(work_end, q5_origin + UINT64_C(5000000000)) ||
            original_main_entry < q5_origin || original_main_entry >= launch_end)
            return std::unexpected{Reason::deadline};
        const auto seal = core::Sha256Digest::parse(text("execution_seal_sha256"));
        const auto build_digest = core::Sha256Digest::parse(text("build_manifest_sha256"));
        if (!seal || !build_digest) return std::unexpected{Reason::malformed};
        HostFrozenSourceGovernedHandoff result{origin, work_end, window_end, used,
            static_cast<std::uint32_t>(ordinal), static_cast<std::uint32_t>(q4),
            static_cast<std::uint32_t>(original_policy), {}, *seal, *build_digest, raw_ready.size()};
        result.post_reboot_baseline = post_reboot;
        result.governed_reboot_baseline = governed_reboot;
        result.environment_recovery_baseline = environment_recovery;
        result.current_source_baseline = current_source;
        std::copy(text("source").begin(), text("source").end(), result.source.begin());
        return result;
    } catch (...) { return std::unexpected{Reason::malformed}; }
}

auto is_host_frozen_source_invocation(int argc, char* argv[]) noexcept -> bool {
    if (argc < 2 || !argv || !argv[1]) return false;
    const std::string_view name{argv[1]};
    return name == "__host-frozen-source" || name == "__host-frozen-source-collector" ||
        name == "__host-frozen-source-governed";
}
namespace {
// The actual original main owns this graph until every child, descriptor and
// signal/subreaper restoration is confirmed. Ordinary preparation can fail;
// it cannot unwind a live native graph or manufacture a successful stage.
struct SourceInvocationDispatcher final {
    using Role = work::SourceLaunchRole;
    using Type = HostFrozenSourceLaunchType;
    runtime::RunnerCancellation& token;
    HostFrozenSourceLaunchClock& clock;
    std::optional<work::ResourceLedger> ledger;
    platform_linux::detail::ControllerSignalScope signals;
    const platform_linux::detail::TopologySourceOperations files;
    const work::SourceLaunchStreamOperations streams;
    const work::GuardOperations test_clock;
    const work::SourceLaunchChannelOperations channels{work::production_source_launch_channel_operations()};
    std::optional<work::SourceLaunchInvocation> invocation;
    std::optional<work::ResourceLease> baseline;
    std::optional<work::SourceInputFileOwner> inputs;
    std::optional<MonitoringRunningBuildAuthority> build;
    MonitoringRunningBuildAuthority::Operations build_operations{};
    std::string build_revision;
    std::array<int, 16> descriptors{};
    std::array<bool, 16> consumed{};
    std::array<HostFrozenSourceLaunchRecord, 4> role_grants{};
    std::array<bool, 4> role_registered{};
    std::array<char, 65536> scratch{};
    std::array<std::byte, 4096> drain_buffer{};
    std::array<std::size_t, 3> drained{};
    std::array<char, 160> controller_diagnostic{};
    std::size_t controller_diagnostic_size{};
    bool controller_diagnostic_overflow{};
    HostFrozenSourceLaunchRecord grant;
    std::optional<HostFrozenSourceBindingFact> first;
    std::optional<work::SourceLaunchChildFailure> first_child;
    std::array<HostFrozenSourceBindingFact, 64> later{};
    std::size_t later_count{}, messages{}, turns{};
    std::uint64_t original_entry{}, last{}, permit{}, incoming{}, outgoing{};
    bool seen_incoming{}, initial_ready{}, packet_ready{}, bootstrap_ready{};
    bool permit_acknowledged{}, producer_finished{};
    std::uint32_t cells{};
    bool validator_output{}, phase_finished{}, publication_retired{}, terminal_requested{};
    bool uncertain{}, subreaper_owned{}, signal_owned{}, receive_bank{};
    int original_subreaper{};
    // Only the original64MiB packet and one64MiB current read can overlap.
    // Validator bodies end after hashing; their bytes remain in Python's
    // actual completion owner. The192MiB native ordinary charge also covers
    // fixed facts/scratch, Build and bounded argument/path preparation.
    std::string packet, candidate, repository, qid, output_path;
    std::optional<core::Sha256Digest> packet_hash, output_hash;
    std::optional<std::array<std::byte, 32>> adjudication_commitment;
    std::uint64_t adjudication_bytes{};
    std::optional<work::SourceLaunchFact> retired_launch_failure;
    std::optional<work::SourceLaunchFact> first_input_failure;
    std::optional<work::Reason> first_resource_failure;
    std::optional<platform_linux::detail::SignalScopeDiagnostic> first_signal_failure;
    std::array<platform_linux::detail::SignalScopeFailure, 2> signal_failures{};
    std::size_t signal_failure_count{};
    std::array<char, 96> first_core_code{};
    std::size_t first_core_code_size{};
    bool governed{}, workflow_completed{}, policy_channels_ready{}, policy_stopped{}, policy_restored{}, policy_report_eof{},
        policy_mask_owned{}, policy_write_failed{}, cancellation_observed{}, policy_report_overflow{};
    std::uint64_t governed_pid{}, policy_end{};
    std::optional<HostFrozenSourceGovernedHandoff> governed_handoff;
    std::optional<std::uint64_t> policy_restored_at_ns;
    std::size_t governed_read_retries{}, policy_total{}, policy_line_size{}, policy_written{};
    std::array<char, 4096> policy_line_bank{};
    std::array<char, 16> policy_stop_reason{};
    std::size_t policy_stop_reason_size{};
    const char* policy_command{};
    std::array<int, 2> policy_flags{};
    std::array<bool, 2> policy_flags_owned{};
    sigset_t policy_original_mask{};
    std::string machine_plan;
    unsigned preparation_step{};

    SourceInvocationDispatcher(runtime::RunnerCancellation& cancellation,
        HostFrozenSourceLaunchClock& sample, std::uint64_t origin,
        platform_linux::detail::TopologySourceOperations file_operations = platform_linux::detail::production_topology_source_operations(),
        work::SourceLaunchStreamOperations stream_operations = work::production_source_launch_stream_operations(),
        work::GuardOperations clock_operations = {}) noexcept
        : token{cancellation}, clock{sample}, files{file_operations}, streams{stream_operations}, test_clock{clock_operations},
          original_entry{origin}, last{origin} { descriptors.fill(-1); }
    SourceInvocationDispatcher(const SourceInvocationDispatcher&) = delete;
    ~SourceInvocationDispatcher() noexcept {
        if (invocation || inputs || baseline || subreaper_owned || signal_owned || policy_mask_owned ||
            std::ranges::any_of(descriptors, [](int fd) { return fd >= 0; })) std::terminate();
    }
    auto fail(HostFrozenSourceLaunchReason reason, HostFrozenSourceBindingOperation operation,
        Raw raw = {}, int fd = -1) noexcept -> bool {
        const HostFrozenSourceBindingFact fact{reason, operation, raw.value, raw.error_number, fd};
        if (!first) first = fact;
        else if (later_count < later.size()) later[later_count++] = fact;
        else uncertain = true;
        if (governed) request_policy(false);
        return false;
    }
    auto sample_now(std::uint64_t* value) noexcept -> Raw {
        if (!test_clock.now) return work::source_launch_raw_now(clock.current, value, &clock.actual);
        const auto sampled = test_clock.now(test_clock.context, value);
        clock.actual = test_clock.actual_raw_now ? *test_clock.actual_raw_now : sampled;
        return sampled;
    }
    auto guard_operations() noexcept -> work::GuardOperations {
        return {this, +[](void* opaque, std::uint64_t* value) noexcept -> Raw {
            auto& self = *static_cast<SourceInvocationDispatcher*>(opaque);
            return self.sample_now(value);
        }, +[](void* opaque) noexcept {
            auto& self = *static_cast<SourceInvocationDispatcher*>(opaque);
            const auto signal = self.signal_owned ? self.signals.observe() : platform_linux::detail::ControllerSignal::none;
            if (signal != platform_linux::detail::ControllerSignal::none)
                static_cast<void>(self.token.try_request(signal == platform_linux::detail::ControllerSignal::interrupt
                    ? runtime::ipc::CancelReason::user_interrupt : runtime::ipc::CancelReason::controller_shutdown));
            return self.token.requested();
        }, &clock.actual};
    }
    auto guard(bool retirement = false, std::uint64_t cleanup_end = 0) noexcept -> bool {
        if (!retirement && first) return false;
        const auto signal = signal_owned ? signals.observe() : platform_linux::detail::ControllerSignal::none;
        if (signal != platform_linux::detail::ControllerSignal::none)
            static_cast<void>(token.try_request(signal == platform_linux::detail::ControllerSignal::interrupt
                ? runtime::ipc::CancelReason::user_interrupt : runtime::ipc::CancelReason::controller_shutdown));
        if (token.requested()) {
            if (!cancellation_observed) {
                cancellation_observed = true;
                static_cast<void>(fail(HostFrozenSourceLaunchReason::cancelled, HostFrozenSourceBindingOperation::guard));
            }
            if (!retirement) return false; //Known retirement remains available.
        }
        std::uint64_t now{};
        const auto sampled = sample_now(&now);
        if (sampled.value || sampled.error_number || now < last)
            return fail(HostFrozenSourceLaunchReason::clock, HostFrozenSourceBindingOperation::guard, clock.actual);
        last = now;
        if (retirement && governed && policy_command) static_cast<void>(policy_flush());
        if (policy_channels_ready && governed_handoff && !policy_read() && !retirement) return false;
        if (!retirement && first) return false;
        const auto end = retirement ? cleanup_end :
            (initial_ready ? (permit ? std::min(permit, grant.phase_end_ns) : grant.phase_end_ns) :
                std::min(grant.entry_end_ns, grant.phase_end_ns));
        if (last >= end) return fail(HostFrozenSourceLaunchReason::deadline, HostFrozenSourceBindingOperation::guard);
        if (!retirement && token.requested())
            return fail(HostFrozenSourceLaunchReason::cancelled, HostFrozenSourceBindingOperation::guard);
        return true;
    }
    auto adopt(std::size_t slot, Raw raw) noexcept -> bool {
        if (slot >= descriptors.size() || descriptors[slot] >= 0 || consumed[slot])
            return fail(HostFrozenSourceLaunchReason::association, HostFrozenSourceBindingOperation::open, raw);
        if (raw.value >= 0 && raw.value <= INT32_MAX) descriptors[slot] = static_cast<int>(raw.value);
        if (raw.value < 3 || raw.value > INT32_MAX || raw.error_number) {
            if (raw.value > INT32_MAX) uncertain = true;
            return fail(HostFrozenSourceLaunchReason::io, HostFrozenSourceBindingOperation::open, raw,
                descriptors[slot]);
        }
        for (std::size_t index{}; index < descriptors.size(); ++index)
            if (index != slot && descriptors[index] == descriptors[slot]) {
                descriptors[slot] = -1;
                return fail(HostFrozenSourceLaunchReason::association, HostFrozenSourceBindingOperation::open, raw);
            }
        return true;
    }
    auto close(std::size_t slot) noexcept -> bool {
        if (descriptors[slot] < 0 || consumed[slot]) return true;
        const auto descriptor = descriptors[slot];
        consumed[slot] = true;
        const auto result = files.close(files.context, descriptor);
        if (result.value || result.error_number) {
            uncertain = true;
            return fail(HostFrozenSourceLaunchReason::uncertain, HostFrozenSourceBindingOperation::close, result, descriptor);
        }
        descriptors[slot] = -1;
        return true;
    }
    auto metadata(int fd, struct stat& info) noexcept -> bool {
        if (!guard()) return false;
        const auto result = files.stat_fd(files.context, fd, &info);
        if (result.value || result.error_number)
            return fail(HostFrozenSourceLaunchReason::io, HostFrozenSourceBindingOperation::metadata, result, fd);
        return guard();
    }
    void retain_signal(const platform_linux::detail::SignalScopeFailure& failure) noexcept {
        if (!first_signal_failure) first_signal_failure = failure.primary;
        if (signal_failure_count < signal_failures.size()) signal_failures[signal_failure_count++] = failure;
        else uncertain = true;
        static_cast<void>(fail(HostFrozenSourceLaunchReason::native, HostFrozenSourceBindingOperation::guard,
            {0, failure.primary.error_number}));
        for (std::size_t index{}; index < failure.cleanup_count && index < failure.cleanup.size(); ++index)
            static_cast<void>(fail(HostFrozenSourceLaunchReason::uncertain, HostFrozenSourceBindingOperation::guard,
                {0, failure.cleanup[index].error_number}));
        if (failure.facts.residual) uncertain = true;
    }
    auto prepare_ledger() noexcept -> bool {
        auto resources = work::ResourceLedger::prepare();
        if (!resources) {
            first_resource_failure = resources.error();
            return fail(HostFrozenSourceLaunchReason::capacity, HostFrozenSourceBindingOperation::guard);
        }
        ledger.emplace(std::move(*resources));
        return true;
    }
    auto adopt_governed_arguments(char* argv[]) noexcept -> bool {
        governed = true;
        constexpr std::array<std::size_t, 5> slots{1, 2, 10, 11, 12};
        bool accepted = true;
        // All positive actual descriptors enter fixed cells before metadata,
        // heap allocation, other argument parsing, or any failure return.
        for (std::size_t index{}; index < slots.size(); ++index) {
            long descriptor{-1};
            const char* raw = argv[4U + index];
            if (!raw) { accepted = false; continue; }
            const std::string_view text{raw};
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), descriptor);
            if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || descriptor < 3) {
                accepted = false; continue; //0..2 are borrowed standard owners.
            }
            accepted = adopt(slots[index], {descriptor, 0}) && accepted;
        }
        const auto number = [](const char* raw, std::uint64_t& value) noexcept {
            if (!raw) return false;
            const std::string_view text{raw};
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
            return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && !text.empty();
        };
        accepted = number(argv[9], governed_pid) && accepted;
        accepted = number(argv[10], grant.origin_ns) && accepted;
        accepted = number(argv[11], grant.launch_origin_ns) && accepted;
        accepted = number(argv[12], grant.entry_end_ns) && accepted;
        grant.phase_end_ns = grant.entry_end_ns; //No work before the closed handoff.
        grant.invocation_origin_ns = grant.origin_ns;
        if (!accepted || governed_pid != static_cast<std::uint64_t>(streams.process_id()) ||
            !grant.origin_ns || grant.launch_origin_ns != grant.origin_ns ||
            grant.entry_end_ns <= last || last < grant.origin_ns ||
            grant.entry_end_ns - grant.origin_ns > UINT64_C(5000000000))
            return fail(HostFrozenSourceLaunchReason::association, HostFrozenSourceBindingOperation::decode);
        return true;
    }
    auto read_governed_handoff(std::uint64_t original_main_entry, std::string_view qualification) -> bool {
        const auto exec = work::production_exec_operations();
        for (const auto slot : {1U, 2U, 10U, 11U, 12U}) {
            if (!guard()) return false;
            const auto cloexec = exec.cloexec(exec.context, descriptors[slot], true);
            if (cloexec.value || cloexec.error_number)
                return fail(HostFrozenSourceLaunchReason::io, HostFrozenSourceBindingOperation::metadata, cloexec, descriptors[slot]);
        }
        std::array<struct stat, 2> channel_identity{};
        for (std::size_t index{}; index < 2U; ++index) {
            const auto descriptor = descriptors[11U + index];
            struct stat info{};
            if (!metadata(descriptor, info)) return false;
            const auto current = streams.output_flags(descriptor, false, 0);
            if (current.value < 0 || current.error_number)
                return fail(HostFrozenSourceLaunchReason::io, HostFrozenSourceBindingOperation::metadata, current, descriptor);
            const auto mode = current.value & O_ACCMODE;
            if (!S_ISFIFO(info.st_mode) || (info.st_mode & 07777U) != 0600U ||
                info.st_uid != streams.effective_uid() ||
                (index == 0U ? mode != O_WRONLY : mode != O_RDONLY))
                return fail(HostFrozenSourceLaunchReason::association, HostFrozenSourceBindingOperation::metadata, {}, descriptor);
            channel_identity[index] = info;
            if (index && info.st_dev == channel_identity[0].st_dev && info.st_ino == channel_identity[0].st_ino)
                return fail(HostFrozenSourceLaunchReason::association, HostFrozenSourceBindingOperation::metadata, {}, descriptor);
            policy_flags[index] = static_cast<int>(current.value);
            if (!(current.value & O_NONBLOCK)) {
                policy_flags_owned[index] = true; //Before the fallible F_SETFL.
                const auto nonblocking = streams.output_flags(descriptor, true, policy_flags[index] | O_NONBLOCK);
                if (nonblocking.value || nonblocking.error_number)
                    return fail(HostFrozenSourceLaunchReason::io, HostFrozenSourceBindingOperation::metadata, nonblocking, descriptor);
            }
            const auto verified = streams.output_flags(descriptor, false, 0);
            if (verified.value != (policy_flags[index] | O_NONBLOCK) || verified.error_number)
                return fail(HostFrozenSourceLaunchReason::association, HostFrozenSourceBindingOperation::metadata, verified, descriptor);
        }
        policy_channels_ready = true;
        struct stat before{}, after{};
        if (!metadata(descriptors[10], before)) return false;
        const auto flags = exec.file_flags(exec.context, descriptors[10]);
        if (flags.value < 0 || flags.error_number)
            return fail(HostFrozenSourceLaunchReason::io, HostFrozenSourceBindingOperation::metadata, flags, descriptors[10]);
        const auto access = flags.value & O_ACCMODE;
        if (!S_ISREG(before.st_mode) || (before.st_mode & 07777U) != 0600U ||
            before.st_uid != streams.effective_uid() || before.st_nlink != 1 ||
            before.st_size < 1 || before.st_size > 4096 ||
            (access != O_RDONLY && access != O_RDWR) || (flags.value & O_APPEND))
            return fail(HostFrozenSourceLaunchReason::association, HostFrozenSourceBindingOperation::metadata, {}, descriptors[10]);
        for (std::size_t offset{}; offset < static_cast<std::size_t>(before.st_size);) {
            if (!guard()) return false;
            const auto request = std::as_writable_bytes(std::span{scratch}).subspan(offset,
                static_cast<std::size_t>(before.st_size) - offset);
            const auto read = streams.read_at(descriptors[10], request, offset);
            if (read.value < 0 && (read.error_number == EINTR || read.error_number == EAGAIN)) {
                if (++governed_read_retries > 52U) return fail(HostFrozenSourceLaunchReason::capacity, HostFrozenSourceBindingOperation::read);
                continue;
            }
            if (read.value <= 0 || read.error_number || static_cast<std::size_t>(read.value) > request.size())
                return fail(HostFrozenSourceLaunchReason::io, HostFrozenSourceBindingOperation::read, read, descriptors[10]);
            offset += static_cast<std::size_t>(read.value);
        }
        std::array<std::byte, 1> extra{};
        if (!guard()) return false;
        const auto eof = streams.read_at(descriptors[10], extra, static_cast<std::uint64_t>(before.st_size));
        if (eof.value || eof.error_number)
            return fail(HostFrozenSourceLaunchReason::association, HostFrozenSourceBindingOperation::read, eof, descriptors[10]);
        if (!metadata(descriptors[10], after)) return false;
        if (before.st_dev != after.st_dev || before.st_ino != after.st_ino || before.st_mode != after.st_mode ||
            before.st_uid != after.st_uid || before.st_gid != after.st_gid || before.st_nlink != after.st_nlink ||
            before.st_size != after.st_size || before.st_mtim.tv_sec != after.st_mtim.tv_sec ||
            before.st_mtim.tv_nsec != after.st_mtim.tv_nsec || before.st_ctim.tv_sec != after.st_ctim.tv_sec ||
            before.st_ctim.tv_nsec != after.st_ctim.tv_nsec)
            return fail(HostFrozenSourceLaunchReason::association, HostFrozenSourceBindingOperation::metadata, {}, descriptors[10]);
        auto decoded = decode_host_frozen_source_governed_handoff(
            {scratch.data(), static_cast<std::size_t>(before.st_size)}, grant.origin_ns, grant.launch_origin_ns,
            grant.entry_end_ns, original_main_entry, qualification);
        if (!decoded) return fail(decoded.error(), HostFrozenSourceBindingOperation::decode);
        governed_handoff.emplace(std::move(*decoded));
        policy_total = governed_handoff->ready_bytes;
        grant.campaign_end_ns = governed_handoff->work_end;
        grant.phase_end_ns = std::min(governed_handoff->work_end,
            grant.origin_ns > UINT64_MAX - UINT64_C(600000000000) ? UINT64_MAX : grant.origin_ns + UINT64_C(600000000000));
        return guard();
    }
    void request_policy(bool release) noexcept {
        if (!governed || policy_restored) return;
        if (policy_command) {
            //A pending RELEASE can become CANCEL before its first byte. Once
            //a prefix was written, retain that exact command rather than
            //splicing two commands; later cancellation still refuses success.
            if (!release && !policy_written) policy_command = "CANCEL\n";
            return;
        }
        policy_command = release ? "RELEASE\n" : "CANCEL\n";
        //One request fence; child cleanup cannot renew the restoration tail.
        const auto maximum = governed_handoff ? governed_handoff->window_end : grant.entry_end_ns;
        policy_end = std::min(maximum, last > UINT64_MAX - policy_restore ? UINT64_MAX : last + policy_restore);
    }
    auto policy_write(std::span<const std::byte> bytes) noexcept -> Raw {
        sigset_t pipe{}, pending{};
        ::sigemptyset(&pipe); ::sigaddset(&pipe, SIGPIPE);
        const auto blocked = ::pthread_sigmask(SIG_BLOCK, &pipe, &policy_original_mask);
        if (blocked) {
            static_cast<void>(fail(HostFrozenSourceLaunchReason::native, HostFrozenSourceBindingOperation::send, {blocked, 0}, descriptors[11]));
            return {-1, blocked};
        }
        policy_mask_owned = true;
        const bool had_primary = first.has_value();
        const auto prior_secondary_count = later_count;
        const auto before = ::sigpending(&pending);
        const auto before_error = before < 0 ? errno : 0;
        Raw written{-1, EIO};
        bool restore = before == 0 && ::sigismember(&pending, SIGPIPE) == 0;
        if (!restore) static_cast<void>(fail(HostFrozenSourceLaunchReason::native,
            HostFrozenSourceBindingOperation::send, {before, before_error}, descriptors[11]));
        else {
            written = streams.write(descriptors[11], bytes);
            if ((written.value < 0 || written.error_number) && written.error_number != EAGAIN &&
                written.error_number != EWOULDBLOCK && written.error_number != EINTR)
                static_cast<void>(fail(HostFrozenSourceLaunchReason::io, HostFrozenSourceBindingOperation::send, written, descriptors[11]));
            const auto after = ::sigpending(&pending);
            if (after != 0) {
                restore = false;
                static_cast<void>(fail(HostFrozenSourceLaunchReason::native, HostFrozenSourceBindingOperation::send, {after, errno}, descriptors[11]));
            } else if (::sigismember(&pending, SIGPIPE) == 1) {
                timespec immediate{};
                const auto consumed_signal = ::sigtimedwait(&pipe, nullptr, &immediate);
                if (consumed_signal != SIGPIPE) {
                    restore = false;
                    static_cast<void>(fail(HostFrozenSourceLaunchReason::native, HostFrozenSourceBindingOperation::send,
                        {consumed_signal, consumed_signal < 0 ? errno : 0}, descriptors[11]));
                }
            }
        }
        if (!restore) { uncertain = true; return {-1, EIO}; }
        const auto restored = ::pthread_sigmask(SIG_SETMASK, &policy_original_mask, nullptr);
        if (restored) {
            uncertain = true;
            static_cast<void>(fail(HostFrozenSourceLaunchReason::uncertain, HostFrozenSourceBindingOperation::send, {restored, 0}, descriptors[11]));
        } else policy_mask_owned = false;
        if (uncertain || policy_mask_owned || first.has_value() != had_primary || later_count != prior_secondary_count)
            return {-1, EIO};
        return written;
    }
    auto policy_flush() noexcept -> bool {
        if (!policy_channels_ready || policy_mask_owned || uncertain || policy_write_failed) return false;
        if (!policy_command || policy_restored || descriptors[11] < 0) return true;
        const std::string_view command{policy_command};
        if (policy_written >= command.size()) return true;
        const auto bytes = std::as_bytes(std::span{command}).subspan(policy_written);
        const auto written = policy_write(bytes);
        if (written.value < 0 && (written.error_number == EAGAIN || written.error_number == EWOULDBLOCK || written.error_number == EINTR)) return true;
        if (written.value <= 0 || written.error_number || static_cast<std::size_t>(written.value) > bytes.size()) {
            policy_write_failed = true; //Only EAGAIN/EINTR can repeat a write.
            if (written.value > 0) uncertain = true;
            return fail(HostFrozenSourceLaunchReason::io, HostFrozenSourceBindingOperation::send, written, descriptors[11]);
        }
        policy_written += static_cast<std::size_t>(written.value);
        return true;
    }
    auto policy_line(std::string_view line) -> bool {
        if (!governed_handoff || policy_restored) return fail(HostFrozenSourceLaunchReason::sequence, HostFrozenSourceBindingOperation::decode);
        if (!policy_stopped) {
            constexpr std::array<std::string_view, 2> keys{"event", "reason"};
            const auto stop = closed_governed_object(line, keys);
            const auto& reason = stop.at("reason").get_ref<const std::string&>();
            if (stop.at("event") != "stop" || (reason != "release" && reason != "cancel" && reason != "expiry" && reason != "signal" && reason != "eof"))
                return fail(HostFrozenSourceLaunchReason::malformed, HostFrozenSourceBindingOperation::decode);
            policy_stopped = true;
            std::copy(reason.begin(), reason.end(), policy_stop_reason.begin());
            policy_stop_reason_size = reason.size();
            if (!policy_command || reason != (std::string_view{policy_command} == "RELEASE\n" ? "release" : "cancel"))
                return fail(HostFrozenSourceLaunchReason::cancelled, HostFrozenSourceBindingOperation::receive, {}, descriptors[12]);
            return true;
        }
        constexpr std::array<std::string_view, 15> keys{"event", "clock", "expected_policy", "mutation_attempted",
            "stop_reason", "restoration_confirmed", "original_verified", "close_uncertain", "clock_uncertain",
            "origin_ns", "window_end_ns", "restored_at_ns", "primary", "cleanup", "diagnostics_incomplete"};
        const auto result = closed_governed_object(line, keys, true);
        const auto boolean = [&](std::string_view key, bool expected) {
            return result.at(key).is_boolean() && result.at(key).get<bool>() == expected;
        };
        const auto restored = governed_unsigned(result, "restored_at_ns");
        if (result.at("event") != "result" || result.at("clock") != "CLOCK_MONOTONIC_RAW" ||
            governed_unsigned(result, "expected_policy") != governed_handoff->original_policy ||
            governed_unsigned(result, "origin_ns") != governed_handoff->window_origin ||
            governed_unsigned(result, "window_end_ns") != governed_handoff->window_end ||
            restored < governed_handoff->window_origin || restored > last || restored >= governed_handoff->window_end ||
            !boolean("mutation_attempted", true) || !boolean("restoration_confirmed", true) ||
            !boolean("original_verified", true) || !boolean("close_uncertain", false) || !boolean("clock_uncertain", false) ||
            !boolean("diagnostics_incomplete", false) || !result.at("primary").is_null() ||
            !result.at("cleanup").is_array() || !result.at("cleanup").empty())
            return fail(HostFrozenSourceLaunchReason::uncertain, HostFrozenSourceBindingOperation::decode, {}, descriptors[12]);
        const auto& reason = result.at("stop_reason").get_ref<const std::string&>();
        if (reason != std::string_view{policy_stop_reason.data(), policy_stop_reason_size})
            return fail(HostFrozenSourceLaunchReason::association, HostFrozenSourceBindingOperation::decode, {}, descriptors[12]);
        std::uint64_t observed{};
        const auto sampled = sample_now(&observed);
        if (sampled.value || sampled.error_number || observed < last)
            return fail(HostFrozenSourceLaunchReason::clock, HostFrozenSourceBindingOperation::guard, clock.actual);
        last = observed;
        if (observed >= governed_handoff->window_end || (policy_end && observed >= policy_end))
            return fail(HostFrozenSourceLaunchReason::deadline, HostFrozenSourceBindingOperation::guard);
        policy_restored_at_ns = restored;
        policy_restored = true;
        return true;
    }
    auto diagnose_policy_timing(bool retired) const noexcept -> bool {
        if (!policy_restored || !policy_restored_at_ns || !governed_handoff) return false;
        // Only a fully accepted RESULT supplies this timestamp. Projection is
        // optional after cleanup; it neither changes the first cause/status nor
        // turns restoration, report EOF or whole-actor retirement into one fact.
        const auto elapsed = *policy_restored_at_ns - governed_handoff->window_origin;
        const auto exposure = governed_handoff->exposure_used + elapsed;
        std::array<char, 512> bytes{};
        const auto count = std::snprintf(bytes.data(), bytes.size(),
            "source_policy_result window_ordinal=%u origin_ns=%llu window_end_ns=%llu "
            "restored_at_ns=%llu elapsed_ns=%llu exposure_ns=%llu restoration_confirmed=1 "
            "report_eof=%u retirement_confirmed=%u\n",
            governed_handoff->window_ordinal,
            static_cast<unsigned long long>(governed_handoff->window_origin),
            static_cast<unsigned long long>(governed_handoff->window_end),
            static_cast<unsigned long long>(*policy_restored_at_ns),
            static_cast<unsigned long long>(elapsed), static_cast<unsigned long long>(exposure),
            static_cast<unsigned>(policy_report_eof), static_cast<unsigned>(retired));
        if (count <= 0 || static_cast<std::size_t>(count) >= bytes.size()) return false;
        const auto written = streams.write(STDERR_FILENO,
            std::as_bytes(std::span{bytes}).first(static_cast<std::size_t>(count)));
        return written.value == count && !written.error_number;
    }
    auto policy_read() noexcept -> bool {
        if (policy_report_overflow) return false;
        if (!governed_handoff || descriptors[12] < 0 || policy_report_eof) return true;
        try {
            std::array<char, 1024> incoming_bytes{};
            bool accepted_lines = true;
            for (std::size_t attempt{}; attempt < 4U; ++attempt) {
                const auto request = std::span{incoming_bytes}.first(std::min(incoming_bytes.size(), 4096U - policy_total + 1U));
                const auto read = files.read(files.context, descriptors[12], request);
                if (read.value < 0 && (read.error_number == EAGAIN || read.error_number == EWOULDBLOCK || read.error_number == EINTR)) return true;
                if (!read.value && !read.error_number) {
                    if (!policy_restored || policy_line_size)
                        return fail(HostFrozenSourceLaunchReason::eof, HostFrozenSourceBindingOperation::read, read, descriptors[12]);
                    //Result acceptance and report closure are distinct facts.
                    //Actual EOF is checked after its read under the same tail.
                    std::uint64_t observed{};
                    const auto sampled = sample_now(&observed);
                    if (sampled.value || sampled.error_number || observed < last)
                        return fail(HostFrozenSourceLaunchReason::clock, HostFrozenSourceBindingOperation::guard, clock.actual);
                    last = observed;
                    if (observed >= governed_handoff->window_end || (policy_end && observed >= policy_end))
                        return fail(HostFrozenSourceLaunchReason::deadline, HostFrozenSourceBindingOperation::guard);
                    policy_report_eof = true;
                    return accepted_lines;
                }
                if (read.value < 0 || read.error_number || static_cast<std::size_t>(read.value) > request.size())
                    return fail(HostFrozenSourceLaunchReason::io, HostFrozenSourceBindingOperation::read, read, descriptors[12]);
                if (static_cast<std::size_t>(read.value) > 4096U - policy_total) {
                    policy_report_overflow = true;
                    return fail(HostFrozenSourceLaunchReason::capacity, HostFrozenSourceBindingOperation::read, read, descriptors[12]);
                }
                policy_total += static_cast<std::size_t>(read.value);
                if (policy_restored)
                    return fail(HostFrozenSourceLaunchReason::sequence, HostFrozenSourceBindingOperation::read, read, descriptors[12]);
                for (std::size_t index{}; index < static_cast<std::size_t>(read.value); ++index) {
                    const auto character = incoming_bytes[index];
                    if (policy_line_size >= policy_line_bank.size())
                        return fail(HostFrozenSourceLaunchReason::capacity, HostFrozenSourceBindingOperation::read, read, descriptors[12]);
                    policy_line_bank[policy_line_size++] = character;
                    if (character == '\n') {
                        //Sampling after the actual read preserves its original
                        //RAW cause/history and checks the result observation.
                        std::uint64_t now{};
                        const auto sampled = sample_now(&now);
                        if (sampled.value || sampled.error_number || now < last)
                            return fail(HostFrozenSourceLaunchReason::clock, HostFrozenSourceBindingOperation::guard, clock.actual);
                        last = now;
                        if (now >= governed_handoff->window_end)
                            return fail(HostFrozenSourceLaunchReason::deadline, HostFrozenSourceBindingOperation::guard);
                        const std::string_view line{policy_line_bank.data(), policy_line_size};
                        const auto accepted = policy_line(line);
                        policy_line_size = 0;
                        accepted_lines = accepted && accepted_lines;
                        if (policy_restored && index + 1U != static_cast<std::size_t>(read.value))
                            return fail(HostFrozenSourceLaunchReason::sequence, HostFrozenSourceBindingOperation::decode, {}, descriptors[12]);
                    }
                }
            }
            return accepted_lines;
        } catch (...) { return fail(HostFrozenSourceLaunchReason::malformed, HostFrozenSourceBindingOperation::decode, {}, descriptors[12]); }
    }
    auto retire_policy(bool children_confirmed) noexcept -> bool {
        if (!governed) return true;
        if (governed_handoff) static_cast<void>(guard(true, governed_handoff->window_end));
        if (!policy_command) request_policy(children_confirmed && workflow_completed && terminal_requested && !token.requested() && !first &&
            !(invocation && invocation->primary()) && !first_input_failure);
        bool confirmed = children_confirmed;
        if (governed_handoff) {
            for (std::size_t attempt{}; !policy_report_eof && attempt < 302U && guard(true, policy_end); ++attempt) {
                static_cast<void>(policy_flush());
                static_cast<void>(policy_read());
                if (!policy_report_eof) static_cast<void>(streams.pause(100));
            }
            confirmed = confirmed && policy_restored && policy_report_eof && !policy_mask_owned;
        } else {
            //Malformed timing cannot mint a replacement restoration window.
            static_cast<void>(policy_flush());
            confirmed = false;
        }
        for (std::size_t index{}; index < 2U; ++index) {
            if (!policy_flags_owned[index]) continue;
            const auto restored = streams.output_flags(descriptors[11U + index], true, policy_flags[index]);
            if (restored.value || restored.error_number) {
                uncertain = true; confirmed = false;
                static_cast<void>(fail(HostFrozenSourceLaunchReason::uncertain, HostFrozenSourceBindingOperation::metadata, restored, descriptors[11U + index]));
            } else policy_flags_owned[index] = false;
        }
        return confirmed && !uncertain;
    }
    auto synthetic_interpreter_matches(int copied, const std::string& configured) -> bool {
        // This is solely the executable-local synthetic fixture. The normal
        // source path keeps its configured image and unchanged owner predicate.
        if (!adopt(9, files.open_root(files.context, configured.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC)))
            return false;
        std::array<std::optional<core::Sha256Digest>, 2> hashes;
        for (std::size_t index{}; index < hashes.size(); ++index) {
            const auto descriptor = index == 0U ? copied : descriptors[9];
            struct stat before{}, after{};
            if (!metadata(descriptor, before)) return false;
            if (!S_ISREG(before.st_mode) || before.st_nlink != 1 || before.st_size <= 0 ||
                before.st_size > INT64_C(67108864) || !(before.st_mode & 0111U) || (before.st_mode & 06022U))
                return fail(HostFrozenSourceLaunchReason::association, HostFrozenSourceBindingOperation::metadata, {}, descriptor);
            candidate.resize(static_cast<std::size_t>(before.st_size));
            for (std::size_t offset{}; offset < candidate.size();) {
                if (!guard()) return false;
                auto bytes = std::as_writable_bytes(std::span{candidate}).subspan(offset);
                const auto count = std::min(bytes.size(), std::size_t{65536});
                const auto read = streams.read_at(descriptor, bytes.first(count), offset);
                if (read.value <= 0 || read.error_number || static_cast<std::size_t>(read.value) > count)
                    return fail(HostFrozenSourceLaunchReason::io, HostFrozenSourceBindingOperation::read, read, descriptor);
                offset += static_cast<std::size_t>(read.value);
            }
            if (!metadata(descriptor, after)) return false;
            if (before.st_dev != after.st_dev || before.st_ino != after.st_ino || before.st_mode != after.st_mode ||
                before.st_uid != after.st_uid || before.st_gid != after.st_gid || before.st_nlink != after.st_nlink ||
                before.st_size != after.st_size || before.st_mtim.tv_sec != after.st_mtim.tv_sec ||
                before.st_mtim.tv_nsec != after.st_mtim.tv_nsec || before.st_ctim.tv_sec != after.st_ctim.tv_sec ||
                before.st_ctim.tv_nsec != after.st_ctim.tv_nsec)
                return fail(HostFrozenSourceLaunchReason::association, HostFrozenSourceBindingOperation::metadata, {}, descriptor);
            hashes[index] = core::sha256(candidate);
        }
        candidate.clear(); // Its checked capacity remains in the native buffer charge.
        if (hashes[0] != hashes[1])
            return fail(HostFrozenSourceLaunchReason::association, HostFrozenSourceBindingOperation::metadata);
        return close(9);
    }
    auto associated(const HostFrozenSourceLaunchRecord& value) noexcept -> bool {
        return value.invocation == grant.invocation && value.build == grant.build && value.source == grant.source &&
            value.campaign == grant.campaign && value.invocation_origin_ns == grant.invocation_origin_ns &&
            value.campaign_end_ns == grant.campaign_end_ns && value.origin_ns == grant.origin_ns &&
            value.phase_end_ns == grant.phase_end_ns && value.phase == grant.phase &&
            value.phase_generation == grant.phase_generation &&
            (value.role != Role::controller || (value.generation == grant.generation && value.resources == grant.resources &&
                value.launch_origin_ns == grant.launch_origin_ns && value.entry_end_ns == grant.entry_end_ns));
    }
    auto send(HostFrozenSourceLaunchRecord value) -> bool {
        if (outgoing == UINT64_MAX || !guard()) return false;
        value.sequence = outgoing++;
        const auto bytes = encode_host_frozen_source_launch_record(value);
        if (!bytes) return fail(bytes.error(), HostFrozenSourceBindingOperation::decode);
        for (std::size_t attempts{}; attempts < 52U; ++attempts) {
            if (!guard()) return false;
            const auto result = channels.send(channels.context, descriptors[6], *bytes, {});
            if (result.value == static_cast<long>(bytes->size()) && !result.error_number) return guard();
            if (result.value < 0 && (result.error_number == EAGAIN || result.error_number == EWOULDBLOCK ||
                                     result.error_number == EINTR)) {
                static_cast<void>(streams.pause(100));
                continue;
            }
            return fail(HostFrozenSourceLaunchReason::io, HostFrozenSourceBindingOperation::send, result, descriptors[6]);
        }
        return fail(HostFrozenSourceLaunchReason::deadline, HostFrozenSourceBindingOperation::send);
    }
    static auto charge(Role role, work::SourceLaunchPhase phase) noexcept -> work::Resources {
        work::Resources value{};
        value.processes = 1; value.threads = 1; value.descriptors = 16;
        value.stack_bytes = core::ByteCount::from_base_units(UINT64_C(8388608));
        value.ordinary_bytes = core::ByteCount::from_base_units(UINT64_C(6291456));
        if (role == Role::controller) {
            value.descriptors = 128;
            value.ordinary_bytes = core::ByteCount::from_base_units(gib / 2U);
        } else if (role == Role::collector) {
            value.descriptors = 256;
            value.ordinary_bytes = core::ByteCount::from_base_units(3U * gib);
            if (phase != work::SourceLaunchPhase::q7) {
                value.threads = 11; value.measurement_workers = 8; value.service_roles = 2;
                value.stack_bytes = core::ByteCount::from_base_units(UINT64_C(134217728));
                value.mapped_bytes = core::ByteCount::from_base_units(9U * gib);
            }
        } else if (role == Role::validator) {
            value.descriptors = 32;
            value.ordinary_bytes = core::ByteCount::from_base_units(gib / 4U);
        }
        value.address_space_bytes = core::ByteCount::from_base_units(
            value.stack_bytes.value() + value.mapped_bytes.value() + value.ordinary_bytes.value());
        return value;
    }
    auto read_producer(const HostFrozenSourceLaunchRecord& value) -> bool {
        if (value.rights_count != 1U || value.path_bytes != 64U || value.stdout_cap == 0 ||
            value.stdout_cap > UINT64_C(67108864) || invocation->received_descriptors().size() != 1U)
            return fail(HostFrozenSourceLaunchReason::association, HostFrozenSourceBindingOperation::decode);
        const auto descriptor = invocation->received_descriptors().front();
        struct stat before{}, after{};
        if (!metadata(descriptor, before)) return false;
        if (!S_ISREG(before.st_mode) || before.st_uid != streams.effective_uid() ||
            (before.st_mode & 0777U) != 0600U || before.st_nlink != 1 || before.st_size <= 0 ||
            static_cast<std::uint64_t>(before.st_size) != value.stdout_cap)
            return fail(HostFrozenSourceLaunchReason::association, HostFrozenSourceBindingOperation::metadata, {}, descriptor);
        candidate.resize(static_cast<std::size_t>(value.stdout_cap));
        for (std::size_t position{}; position < candidate.size();) {
            if (!guard()) return false;
            auto bytes = std::as_writable_bytes(std::span{candidate.data(), candidate.size()}).subspan(position);
            const auto result = streams.read_at(descriptor, bytes.first(std::min(bytes.size(), std::size_t{65536})), position);
            if (result.value <= 0 || result.error_number || static_cast<std::size_t>(result.value) > bytes.size())
                return fail(HostFrozenSourceLaunchReason::io, HostFrozenSourceBindingOperation::read, result, descriptor);
            position += static_cast<std::size_t>(result.value);
        }
        if (!metadata(descriptor, after) || before.st_dev != after.st_dev || before.st_ino != after.st_ino ||
            before.st_size != after.st_size || before.st_mtim.tv_sec != after.st_mtim.tv_sec ||
            before.st_mtim.tv_nsec != after.st_mtim.tv_nsec)
            return fail(HostFrozenSourceLaunchReason::association, HostFrozenSourceBindingOperation::metadata, {}, descriptor);
        const auto hash = core::sha256(candidate);
        if (hash.to_hex() != std::string_view{value.path.data(), value.path_bytes})
            return fail(HostFrozenSourceLaunchReason::association, HostFrozenSourceBindingOperation::read, {}, descriptor);
        if (value.selector == host_frozen_source_input_selector) {
            if (value.role != Role::collector || packet_ready || grant.phase == work::SourceLaunchPhase::q7)
                return fail(HostFrozenSourceLaunchReason::association, HostFrozenSourceBindingOperation::decode);
            if (grant.phase == work::SourceLaunchPhase::q5) {
                packet.swap(candidate); packet_hash = hash;
            } else if (!packet_hash || candidate != packet)
                return fail(HostFrozenSourceLaunchReason::association, HostFrozenSourceBindingOperation::read);
            packet_ready = true;
        } else {
            if (value.role != Role::validator || validator_output)
                return fail(HostFrozenSourceLaunchReason::association, HostFrozenSourceBindingOperation::decode);
            if (grant.phase == work::SourceLaunchPhase::q7 &&
                (!adjudication_commitment || hash.bytes() != *adjudication_commitment ||
                 value.stdout_cap != adjudication_bytes))
                return fail(HostFrozenSourceLaunchReason::association, HostFrozenSourceBindingOperation::read);
            output_hash = hash; validator_output = true;
        }
        std::string{}.swap(candidate);
        return invocation->close_received().has_value();
    }
    auto reobserve_build() -> bool {
        if (!guard() || !build) return false;
        // The retained initial Build is the association anchor. Each fallible
        // publication boundary gets a fresh one-use recheck owner; reusing a
        // consumed recheck would violate that lower owner's contract.
        auto current = MonitoringRunningBuildAuthority::retain_for_composition(build_revision, build_operations);
        core::detail::ExpectedErrorCleanup current_cleanup{current};
        if (!current) return fail(HostFrozenSourceLaunchReason::association, HostFrozenSourceBindingOperation::guard);
        bool accepted = current->identity() == build->identity() && current->canonical() == build->canonical();
        if (accepted) {
            auto checked = current->recheck(); core::detail::ExpectedErrorCleanup checked_cleanup{checked};
            accepted = checked && *checked;
        }
        if (!accepted) static_cast<void>(fail(HostFrozenSourceLaunchReason::association, HostFrozenSourceBindingOperation::guard));
        // Preserve the original refusal before consuming the current owner.
        auto retired = current->retire(); core::detail::ExpectedErrorCleanup retired_cleanup{retired};
        if (!retired) static_cast<void>(fail(HostFrozenSourceLaunchReason::contained, HostFrozenSourceBindingOperation::guard));
        return accepted && retired && guard();
    }
    auto handle(HostFrozenSourceLaunchRecord value) -> bool {
        const auto index = static_cast<std::size_t>(value.role);
        if (!associated(value) || (seen_incoming && value.sequence <= incoming) ||
            value.rights_count != invocation->received_descriptors().size() || ++messages > 4096U)
            return fail(HostFrozenSourceLaunchReason::association, HostFrozenSourceBindingOperation::decode);
        seen_incoming = true; incoming = value.sequence;
        if (value.type == Type::launch) {
            if (value.rights_count || value.pid || value.path_bytes || value.selector || value.stdout_cap ||
                value.role == Role::controller || role_registered[index])
                return fail(HostFrozenSourceLaunchReason::association, HostFrozenSourceBindingOperation::decode);
            const auto scope = invocation->begin_launch(value.role);
            if (!scope) return false;
            const auto demand = charge(value.role, grant.phase);
            const auto bootstrap = value.role == Role::collector ? std::optional{charge(Role::git, grant.phase)} : std::nullopt;
            if (!invocation->reserve_external(value.role, demand, bootstrap)) return false;
            value.type = Type::launched;
            value.generation = invocation->slot(value.role).generation;
            value.resources = demand;
            value.launch_origin_ns = scope->origin_ns; value.entry_end_ns = scope->end_ns;
            value.permit_end_ns = permit;
            role_grants[index] = value; role_registered[index] = true;
            return send(value);
        }
        if (value.type == Type::launched) {
            if (value.role == Role::controller) {
                if (initial_ready || value.pid != static_cast<std::uint64_t>(invocation->slot(Role::controller).pid) ||
                    value.rights_count || value.selector || value.path_bytes || value.stdout_cap)
                    return fail(HostFrozenSourceLaunchReason::association, HostFrozenSourceBindingOperation::decode);
                initial_ready = true;
                if (!invocation->set_stage(work::SourceLaunchStage::bootstrap)) return false;
            } else {
                if (!role_registered[index] || value.generation != role_grants[index].generation ||
                    value.resources != role_grants[index].resources || value.launch_origin_ns != role_grants[index].launch_origin_ns ||
                    value.entry_end_ns != role_grants[index].entry_end_ns || value.rights_count || value.path_bytes ||
                    value.selector || value.stdout_cap || !invocation->adopt_external(value.role, static_cast<long>(value.pid)))
                    return fail(HostFrozenSourceLaunchReason::association, HostFrozenSourceBindingOperation::decode);
                role_grants[index].pid = value.pid;
            }
            return send(value);
        }
        if (value.role != Role::controller &&
            (!role_registered[index] || value.pid != role_grants[index].pid || value.generation != role_grants[index].generation ||
             value.resources != role_grants[index].resources || value.launch_origin_ns != role_grants[index].launch_origin_ns ||
             value.entry_end_ns != role_grants[index].entry_end_ns))
            return fail(HostFrozenSourceLaunchReason::association, HostFrozenSourceBindingOperation::decode);
        if (value.type == Type::ready && (value.selector == host_frozen_source_input_selector ||
                                         value.selector == host_frozen_source_output_selector)) {
            if (!read_producer(value)) return false;
            value.rights_count = 0;
            return send(value);
        }
        if (value.selector == host_frozen_source_control_selector && value.role == Role::collector && !value.rights_count) {
            std::array<std::byte, 64> bytes{};
            if (value.path_bytes != 128U) return fail(HostFrozenSourceLaunchReason::malformed, HostFrozenSourceBindingOperation::decode);
            for (std::size_t i{}; i < bytes.size(); ++i) {
                const auto high = hexadecimal.find(value.path[2U * i]), low = hexadecimal.find(value.path[2U * i + 1U]);
                if (high == std::string_view::npos || low == std::string_view::npos)
                    return fail(HostFrozenSourceLaunchReason::malformed, HostFrozenSourceBindingOperation::decode);
                bytes[i] = static_cast<std::byte>((high << 4U) | low);
            }
            bool actual_ready{};
            // Q7 has no Run. Its sole zero-payload READY still closes the actual
            // bootstrap role before the validator starts.
            if (grant.phase == work::SourceLaunchPhase::q7) {
                actual_ready = load(bytes, 1, 1) == 1U;
                if (actual_ready) {
                    if (load(bytes, 0, 1) != 1U || load(bytes, 2, 1) != 7U ||
                        !std::ranges::all_of(std::span{bytes}.subspan(3), [](std::byte b) { return b == std::byte{}; }))
                        return fail(HostFrozenSourceLaunchReason::association, HostFrozenSourceBindingOperation::decode);
                } else if (load(bytes, 0, 1) == 1U && load(bytes, 1, 1) == 6U && load(bytes, 2, 1) == 7U &&
                    load(bytes, 3, 1) == 0 && load(bytes, 4, 4) == 1U &&
                    std::ranges::all_of(std::span{bytes}.subspan(8, 16), [](std::byte b) { return b == std::byte{}; }) &&
                    load(bytes, 24, 8) > 0 && load(bytes, 24, 8) <= UINT64_C(67108864) &&
                    bootstrap_ready && !permit && !cells && !value.permit_end_ns && !producer_finished) {
                    // Q7 has no Run/PERMIT. Its one FINISH ACK commits the
                    // later retained validator output; replay cannot renew it.
                    adjudication_commitment.emplace();
                    std::copy_n(bytes.begin() + 32, 32U, adjudication_commitment->begin());
                    adjudication_bytes = load(bytes, 24, 8);
                    producer_finished = true;
                }
                else
                    return fail(HostFrozenSourceLaunchReason::association, HostFrozenSourceBindingOperation::decode);
            } else {
                const auto control = decode_frozen_process_record(bytes);
                if (!control || control->phase != static_cast<unsigned>(grant.phase))
                    return fail(HostFrozenSourceLaunchReason::association, HostFrozenSourceBindingOperation::decode);
                if (control->type == FrozenProcessType::permit) {
                    const auto maximum = grant.phase == work::SourceLaunchPhase::q5 ? 8U : 384U;
                    if (!bootstrap_ready || (permit && !permit_acknowledged) || control->ordinal != cells || cells >= maximum ||
                        control->deadline_ns <= last || control->deadline_ns > grant.phase_end_ns ||
                        value.permit_end_ns != control->deadline_ns)
                        return fail(HostFrozenSourceLaunchReason::association, HostFrozenSourceBindingOperation::decode);
                    permit = control->deadline_ns; permit_acknowledged = false;
                } else if (control->type == FrozenProcessType::ack) {
                    const auto maximum = grant.phase == work::SourceLaunchPhase::q5 ? 8U : 384U;
                    if (!bootstrap_ready || !permit || control->ordinal != cells || value.permit_end_ns != permit)
                        return fail(HostFrozenSourceLaunchReason::association, HostFrozenSourceBindingOperation::decode);
                    if (cells < maximum && !permit_acknowledged) { ++cells; permit_acknowledged = true; }
                    else if (cells == maximum && permit_acknowledged && !producer_finished) producer_finished = true;
                    else return fail(HostFrozenSourceLaunchReason::sequence, HostFrozenSourceBindingOperation::decode);
                }
                else if (control->type != FrozenProcessType::ready)
                    return fail(HostFrozenSourceLaunchReason::association, HostFrozenSourceBindingOperation::decode);
                actual_ready = control->type == FrozenProcessType::ready;
            }
            if (!bootstrap_ready) {
                if (!actual_ready || (!packet_ready && grant.phase != work::SourceLaunchPhase::q7) ||
                    !invocation->retire_serial_bootstrap() || !invocation->set_stage(work::SourceLaunchStage::collection))
                    return fail(HostFrozenSourceLaunchReason::association, HostFrozenSourceBindingOperation::decode);
                bootstrap_ready = true;
            }
            return send(value);
        }
        if (value.type == Type::terminal && value.role != Role::controller) {
            if (value.rights_count || value.selector || value.wait_status || value.error_number ||
                (value.role == Role::validator && (!validator_output || !output_hash || value.path_bytes != 64U ||
                    output_hash->to_hex() != std::string_view{value.path.data(), value.path_bytes})) ||
                !invocation->confirm_external_terminal(value.role, static_cast<int>(value.pid), 0) ||
                !invocation->retire(value.role))
                return fail(HostFrozenSourceLaunchReason::association, HostFrozenSourceBindingOperation::decode);
            role_registered[index] = false;
            return send(value);
        }
        const auto children_retired = [&]() noexcept {
            return !invocation->slot(Role::collector).occupied && !invocation->slot(Role::validator).occupied &&
                !invocation->slot(Role::git).occupied;
        };
        if (value.type == Type::finish && value.role == Role::controller) {
            if (phase_finished || !children_retired() || !bootstrap_ready || !producer_finished || !validator_output || !output_hash ||
                value.rights_count || value.selector || value.pid || value.path_bytes != 64U ||
                std::string_view{value.path.data(), value.path_bytes} != output_hash->to_hex())
                return fail(HostFrozenSourceLaunchReason::association, HostFrozenSourceBindingOperation::decode);
            permit = 0; phase_finished = true; value.permit_end_ns = 0;
            if (!invocation->set_stage(work::SourceLaunchStage::publication)) return false;
            return send(value);
        }
        if (value.type == Type::ready && value.role == Role::controller && phase_finished && children_retired()) {
            if (value.selector == host_frozen_source_reobserve_selector && !value.rights_count &&
                !value.path_bytes && packet_hash) {
                if (!reobserve_build()) return false;
                const auto hash = packet_hash->to_hex();
                value.path_bytes = static_cast<std::uint16_t>(hash.size());
                std::copy(hash.begin(), hash.end(), value.path.begin());
                return send(value);
            }
            if (value.selector == host_frozen_source_publication_retired_selector && !value.rights_count &&
                !value.path_bytes && !publication_retired) {
                publication_retired = true;
                return send(value);
            }
            if (value.selector == host_frozen_source_root_directory_selector ||
                value.selector == host_frozen_source_leaf_directory_selector) {
                if (value.rights_count != 1U || value.path_bytes) return false;
                const auto observed = invocation->publication_directory(value.selector == host_frozen_source_root_directory_selector, files);
                if (!observed) return false;
                value.rights_count = 0; value.path_bytes = 16;
                for (std::size_t i{}; i < 16; ++i) value.path[15U - i] = hexadecimal[(*observed >> (4U * i)) & 15U];
                return send(value);
            }
        }
        if (value.type == Type::grant && value.role == Role::controller && publication_retired && children_retired() &&
            !value.rights_count && !value.path_bytes && !value.pid && grant.phase != work::SourceLaunchPhase::q7) {
            const auto next = static_cast<work::SourceLaunchPhase>(static_cast<unsigned>(grant.phase) + 1U);
            const auto deadline = invocation->advance_phase(next, grant.phase_generation);
            if (!deadline) return false;
            grant.phase = next; grant.phase_generation = deadline->generation;
            grant.origin_ns = deadline->origin_ns; grant.phase_end_ns = deadline->end_ns; grant.permit_end_ns = 0;
            value = grant; value.type = Type::grant; value.path = {}; value.path_bytes = 0;
            packet_ready = false; bootstrap_ready = false; validator_output = false;
            phase_finished = false; publication_retired = false; output_hash.reset(); permit = 0;
            cells = 0; permit_acknowledged = false; producer_finished = false;
            adjudication_commitment.reset(); adjudication_bytes = 0;
            return send(value);
        }
        if (value.type == Type::terminal && value.role == Role::controller && grant.phase == work::SourceLaunchPhase::q7 &&
            publication_retired && children_retired() && value.pid == static_cast<std::uint64_t>(invocation->slot(Role::controller).pid) &&
            !value.rights_count && !value.path_bytes && !value.selector && !terminal_requested) {
            terminal_requested = true;
            return send(value);
        }
        return fail(HostFrozenSourceLaunchReason::association, HostFrozenSourceBindingOperation::decode);
    }

    auto drain() noexcept -> bool {
        const auto view = invocation->slot(Role::controller);
        for (std::size_t index{}; index < view.streams.size(); ++index) {
            if (view.streams[index] < 0) continue;
            const auto owned = invocation->descriptors(Role::controller);
            if (std::ranges::none_of(owned, [&](const auto& fd) { return fd.descriptor == view.streams[index] && !fd.consumed; })) continue;
            const auto bytes = std::span{reinterpret_cast<char*>(drain_buffer.data()), drain_buffer.size()};
            const auto result = files.read(files.context, view.streams[index], bytes);
            if (result.value > 0 && !result.error_number) {
                if (static_cast<std::size_t>(result.value) > bytes.size() ||
                    drained[index] > 1048576U - static_cast<std::size_t>(result.value))
                    return fail(HostFrozenSourceLaunchReason::capacity, HostFrozenSourceBindingOperation::read, result, view.streams[index]);
                drained[index] += static_cast<std::size_t>(result.value);
                if (index == 1U) {
                    const auto count = static_cast<std::size_t>(result.value);
                    if (count > controller_diagnostic.size() - controller_diagnostic_size)
                        controller_diagnostic_overflow = true;
                    else {
                        std::copy_n(bytes.begin(), count,
                            controller_diagnostic.begin() + static_cast<std::ptrdiff_t>(controller_diagnostic_size));
                        controller_diagnostic_size += count;
                    }
                }
                // The source controller has no successful textual output. Any
                // interpreter/setup stderr is a real failed/incomplete attempt.
                return fail(HostFrozenSourceLaunchReason::contained, HostFrozenSourceBindingOperation::read, result, view.streams[index]);
            }
            if (!result.value && !result.error_number) {
                if (!invocation->close_descriptor(Role::controller, view.streams[index])) return false;
            } else if (result.error_number != EAGAIN && result.error_number != EWOULDBLOCK && result.error_number != EINTR)
                return fail(HostFrozenSourceLaunchReason::io, HostFrozenSourceBindingOperation::read, result, view.streams[index]);
        }
        return true;
    }
    auto prepare(const char* root, const char* qualification, const char* entry,
        std::uint64_t origin, const HostFrozenSourceSyntheticDispatcher* synthetic,
        const char* fixture_setup, const work::SourceLaunchOperations* observed_launch = nullptr) -> bool {
        preparation_step = 1U; //The fixed owner precedes ledger allocation.
        if (!ledger && !prepare_ledger()) return false;
        grant.type = Type::grant; grant.role = Role::controller; grant.generation = 1;
        if (!governed) {
            invocation.emplace(*ledger, guard_operations(), observed_launch ? *observed_launch : work::production_source_launch_operations(), origin,
                origin + UINT64_C(10800000000000), &clock.current);
            const auto first_launch = invocation->begin_launch(Role::controller);
            if (!first_launch) return false;
            grant.origin_ns = origin; grant.invocation_origin_ns = origin;
            grant.campaign_end_ns = origin + UINT64_C(10800000000000);
            grant.phase_end_ns = origin + UINT64_C(600000000000);
            grant.launch_origin_ns = first_launch->origin_ns; grant.entry_end_ns = first_launch->end_ns;
        }
        auto native = charge(Role::git, work::SourceLaunchPhase::q5);
        native.descriptors = 16;
        native.ordinary_bytes = core::ByteCount::from_base_units(UINT64_C(201326592));
        native.address_space_bytes = core::ByteCount::from_base_units(native.stack_bytes.value() + native.ordinary_bytes.value());
        preparation_step = 2U; // Enclosing native stock.
        auto reserved = ledger->reserve(native);
        if (!reserved) {
            first_resource_failure = reserved.error();
            return fail(HostFrozenSourceLaunchReason::capacity, HostFrozenSourceBindingOperation::guard);
        }
        baseline.emplace(std::move(*reserved));
        if (governed) {
            if (!read_governed_handoff(original_entry, qualification)) return false;
            invocation.emplace(*ledger, guard_operations(), observed_launch ? *observed_launch : work::production_source_launch_operations(), origin,
                governed_handoff->work_end, &clock.current, work::SourceLaunchPhase::q5,
                work::SourceLaunchDeadline{grant.launch_origin_ns, grant.entry_end_ns});
            const auto inherited = invocation->begin_launch(Role::controller);
            if (!inherited || inherited->origin_ns != grant.launch_origin_ns || inherited->end_ns != grant.entry_end_ns)
                return fail(HostFrozenSourceLaunchReason::association, HostFrozenSourceBindingOperation::guard);
        }
        const work::SourceLaunchDeadline launch{grant.launch_origin_ns, grant.entry_end_ns};

        // This dedicated source invocation applies the already-approved
        // per-process ceilings. Lowering NOFILE does not retire an inherited
        // descriptor; the explicit actual inventory below refuses unknowns.
        auto limits = work::production_limit_operations();
        preparation_step = 3U; // Identity and inherited limits.
        limits.guard = guard_operations();
        int actual_pid{}, actual_parent{};
        std::uint64_t actual_uid{};
        const auto identity = limits.identity(limits.context, &actual_pid, &actual_parent, &actual_uid);
        if (identity.value || identity.error_number || actual_pid != streams.process_id() ||
            actual_parent <= 0 || actual_uid == 0 || actual_uid != streams.effective_uid())
            return fail(HostFrozenSourceLaunchReason::association, HostFrozenSourceBindingOperation::guard, identity);
        work::SourceLaunchSpec own_limits{};
        own_limits.expected_parent = actual_parent;
        own_limits.origin_ns = launch.origin_ns; own_limits.entry_end_ns = launch.end_ns;
        own_limits.limits = {core::ByteCount::from_base_units(20U * gib),
            core::ByteCount::from_base_units(UINT64_C(8388608)), 1024};
        const auto admitted_limits = work::apply_source_launch_child_limits(own_limits, limits, &clock.current, &last);
        if (!admitted_limits) {
            first_child = admitted_limits.error();
            return fail(HostFrozenSourceLaunchReason::native, HostFrozenSourceBindingOperation::guard,
                {static_cast<long>(first_child->native_result), first_child->error_number});
        }
        preparation_step = 4U; // Actual inherited descriptor inventory.
        for (const auto standard : {0, 1, 2}) {
            struct stat standard_info{};
            if (!metadata(standard, standard_info)) return false;
        }
        if (!adopt(8, files.open_root(files.context, "/proc/self/fd", O_RDONLY | O_DIRECTORY | O_CLOEXEC))) return false;
        std::array<int, 19> known_inherited{0, 1, 2};
        std::size_t known_count = 3U;
        for (const auto descriptor : descriptors) if (descriptor >= 3) known_inherited[known_count++] = descriptor;
        std::size_t visited{};
        bool inventory_end{};
        for (std::size_t refill{}; refill < 8U; ++refill) {
            if (!guard()) return false;
            const auto buffer = std::span{scratch}.first(4096U);
            const auto result = files.entries(files.context, descriptors[8], buffer);
            if (result.value == 0 && !result.error_number) { inventory_end = true; break; }
            if (result.value <= 0 || result.error_number || static_cast<std::size_t>(result.value) > buffer.size())
                return fail(HostFrozenSourceLaunchReason::io, HostFrozenSourceBindingOperation::read, result, descriptors[8]);
            const auto parsed = work::source_launch_descriptor_names(buffer.first(static_cast<std::size_t>(result.value)), std::span{known_inherited}.first(known_count), visited);
            if (!parsed) return fail(HostFrozenSourceLaunchReason::association, HostFrozenSourceBindingOperation::decode, result, descriptors[8]);
        }
        if (!inventory_end || !close(8)) return false;
        repository = root; qid = qualification;
        if (repository.front() != '/' || std::filesystem::canonical(repository) != std::filesystem::path{repository})
            return fail(HostFrozenSourceLaunchReason::association, HostFrozenSourceBindingOperation::open);
        preparation_step = 5U; // Signal owner.
        const auto signal_result = signals.acquire();
        if (!signal_result) {
            retain_signal(signal_result.error());
            return false;
        }
        signal_owned = true;
        // The controller envelope describes the collector subquota it may
        // delegate. Its own actual interpreter reservation is the smaller
        // controller charge below; these caps are never summed as two owners.
        grant.resources = charge(Role::collector, work::SourceLaunchPhase::q5);
        grant.path_bytes = static_cast<std::uint16_t>(qid.size());
        std::copy(qid.begin(), qid.end(), grant.path.begin());
        preparation_step = 6U; // Governed repository owner.
        if (!guard() || !adopt(0, files.open_root(files.context, root, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW))) return false;
        struct stat root_stat{};
        if (!metadata(descriptors[0], root_stat) || !S_ISDIR(root_stat.st_mode)) return false;
        preparation_step = 7U; // Genuine running Build.
        build_operations = synthetic ? synthetic->build : MonitoringRunningBuildAuthority::Operations{
            nullptr, +[](void*) { return epycbench::build_manifest_json(); }, +[](void*, unsigned) {}};
        const auto manifest = synthetic ? synthetic->build.manifest(synthetic->build.context) : epycbench::build_manifest_json();
        const auto& revision = manifest.at("git").at("revision").get_ref<const std::string&>();
        auto running = synthetic ? MonitoringRunningBuildAuthority::retain_for_composition(revision, synthetic->build) :
                                   MonitoringRunningBuildAuthority::retain(revision);
        core::detail::ExpectedErrorCleanup running_cleanup{running};
        if (!running) {
            const auto code = running.error().code.value();
            first_core_code_size = std::min(code.size(), first_core_code.size());
            std::copy_n(code.begin(), first_core_code_size, first_core_code.begin());
            return fail(HostFrozenSourceLaunchReason::association, HostFrozenSourceBindingOperation::guard);
        }
        build.emplace(std::move(*running)); grant.build = build->identity().bytes();
        build_revision = revision;
        std::array<int, 16> root_borrow{};
        std::size_t root_borrow_count{};
        for (const auto descriptor : descriptors) if (descriptor >= 3) root_borrow[root_borrow_count++] = descriptor;
        inputs.emplace(*ledger, guard_operations(), files, descriptors[0],
            work::SourceInputRootIdentity{static_cast<std::uint64_t>(root_stat.st_dev), static_cast<std::uint64_t>(root_stat.st_ino)},
            scratch, origin, grant.phase_end_ns, last, &clock.current, std::span{root_borrow}.first(root_borrow_count));
        preparation_step = 8U; // Exact policy and input-manifest bytes.
        const auto policy = inputs->read(work::SourceInputPath::policy);
        if (!policy) { first_input_failure = inputs->primary(); return false; }
        grant.source = core::sha256(*policy).bytes();
        if (governed && core::sha256(*policy).to_hex() != governed_policy_sha)
            return fail(HostFrozenSourceLaunchReason::association, HostFrozenSourceBindingOperation::decode);
        const auto campaign = inputs->read(work::SourceInputPath::manifest);
        if (!campaign) { first_input_failure = inputs->primary(); return false; }
        grant.campaign = core::sha256(*campaign).bytes();
        if (governed) {
            const auto approved = inputs->read(governed_handoff->current_source_baseline
                ? work::SourceInputPath::current_source_campaign : governed_handoff->environment_recovery_baseline
                ? work::SourceInputPath::environment_recovery_campaign : governed_handoff->governed_reboot_baseline
                ? work::SourceInputPath::governed_reboot_campaign : governed_handoff->post_reboot_baseline
                ? work::SourceInputPath::post_reboot_campaign : work::SourceInputPath::campaign);
            if (!approved) { first_input_failure = inputs->primary(); return false; }
            machine_plan.assign(*approved);
            if (core::sha256(machine_plan).to_hex() !=
                (governed_handoff->current_source_baseline ? current_source_plan_sha :
                    governed_handoff->environment_recovery_baseline ? environment_recovery_plan_sha :
                    governed_handoff->governed_reboot_baseline ? governed_reboot_plan_sha :
                    governed_handoff->post_reboot_baseline ? post_reboot_plan_sha : governed_plan_sha))
                return fail(HostFrozenSourceLaunchReason::association, HostFrozenSourceBindingOperation::decode);
            grant.campaign = core::sha256(machine_plan).bytes();
            std::string current_body{build->canonical()}; current_body.push_back('\n');
            if (core::sha256(current_body) != governed_handoff->build_manifest ||
                revision != std::string_view{governed_handoff->source.data(), governed_handoff->source.size()})
                return fail(HostFrozenSourceLaunchReason::association, HostFrozenSourceBindingOperation::decode);
        }
        if (!inputs->retire()) { first_input_failure = inputs->primary(); uncertain = true; return false; }
        inputs.reset();
        const auto entropy = streams.entropy(grant.invocation);
        if (entropy.value != static_cast<long>(grant.invocation.size()) || entropy.error_number)
            return fail(HostFrozenSourceLaunchReason::native, HostFrozenSourceBindingOperation::read, entropy);
        preparation_step = 9U; // Actual held executable images.
        const auto configured_python = std::filesystem::canonical(EPYCBENCH_SOURCE_PYTHON_EXECUTABLE).string();
        const auto python = synthetic && fixture_setup
            ? (std::filesystem::path{fixture_setup}.parent_path() / "python-interpreter").string()
            : configured_python;
        if (!governed) {
            const auto entry_path = std::filesystem::canonical(entry).string();
            if (!adopt(1, files.open_root(files.context, "/proc/self/exe", O_RDONLY | O_CLOEXEC)) ||
                !adopt(2, files.open_root(files.context, entry_path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC))) return false;
        }
        if (!adopt(3, files.open_root(files.context, python.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC))) return false;
        for (const auto slot : {1U, 2U, 3U}) {
            struct stat image{};
            if (!metadata(descriptors[slot], image) || !S_ISREG(image.st_mode) || !(image.st_mode & 0111U) ||
                (image.st_mode & 06022U)) return fail(HostFrozenSourceLaunchReason::association, HostFrozenSourceBindingOperation::metadata);
            const auto exec = work::production_exec_operations();
            const auto capability = exec.capability_size(exec.context, descriptors[slot]);
            if (capability.value != -1 || capability.error_number != ENODATA)
                return fail(HostFrozenSourceLaunchReason::association, HostFrozenSourceBindingOperation::metadata, capability, descriptors[slot]);
        }
        if (governed) {
            const auto exec = work::production_exec_operations();
            for (const auto slot : {1U, 2U}) {
                struct stat info{};
                if (!metadata(descriptors[slot], info)) return false;
                const auto flags = exec.file_flags(exec.context, descriptors[slot]);
                if (flags.value < 0 || flags.error_number)
                    return fail(HostFrozenSourceLaunchReason::io, HostFrozenSourceBindingOperation::metadata, flags, descriptors[slot]);
                if ((flags.value & O_ACCMODE) != O_RDONLY || (flags.value & O_APPEND) || info.st_nlink != 1 ||
                    (info.st_uid != 0 && info.st_uid != streams.effective_uid()))
                    return fail(HostFrozenSourceLaunchReason::association, HostFrozenSourceBindingOperation::metadata, {}, descriptors[slot]);
            }
            platform_linux::detail::ImageMetadata held{}, running_image{};
            const auto held_result = exec.metadata(exec.context, descriptors[1], &held);
            const auto running_result = exec.running_metadata(exec.context, &running_image);
            if (held_result.value || held_result.error_number || running_result.value || running_result.error_number)
                return fail(HostFrozenSourceLaunchReason::io, HostFrozenSourceBindingOperation::metadata,
                    held_result.value || held_result.error_number ? held_result : running_result, descriptors[1]);
            if (held.device != running_image.device || held.inode != running_image.inode)
                return fail(HostFrozenSourceLaunchReason::association, HostFrozenSourceBindingOperation::metadata, {}, descriptors[1]);
        }
        if (synthetic && !synthetic_interpreter_matches(descriptors[3], configured_python)) return false;
        preparation_step = 10U; // Exclusive output-directory owner.
        if (!adopt(4, files.open_at(files.context, descriptors[0], ".local", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC))) return false;
        std::array<std::byte, 16> nonce{};
        const auto random = streams.entropy(nonce);
        if (random.value != static_cast<long>(nonce.size()) || random.error_number) return false;
        std::string directory{"source-launch-"};
        for (const auto byte : nonce) { directory.push_back(hexadecimal[std::to_integer<unsigned>(byte) >> 4U]); directory.push_back(hexadecimal[std::to_integer<unsigned>(byte) & 15U]); }
        const auto made = streams.make_directory(descriptors[4], directory.c_str());
        if (made.value || made.error_number) return fail(HostFrozenSourceLaunchReason::io, HostFrozenSourceBindingOperation::open, made);
        if (!adopt(5, files.open_at(files.context, descriptors[4], directory.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC))) return false;
        output_path = repository + "/.local/" + directory;
        preparation_step = 11U; // Native transport owner.
        int pair[2]{-1, -1};
        const auto paired = channels.socket_pair(channels.context, pair);
        // Both returned cells are registered before any result/clock check.
        bool pair_owned = true;
        for (std::size_t i{}; i < 2; ++i) if (pair[i] >= 0) pair_owned = adopt(6U + i, {pair[i], 0}) && pair_owned;
        if (paired.value || paired.error_number)
            return fail(HostFrozenSourceLaunchReason::io, HostFrozenSourceBindingOperation::open, paired);
        if (!pair_owned || pair[0] < 3 || pair[1] < 3)
            return fail(HostFrozenSourceLaunchReason::association, HostFrozenSourceBindingOperation::open, paired);
        for (const auto fd : pair) {
            const auto credentials = channels.enable_credentials(channels.context, fd);
            if (credentials.value || credentials.error_number)
                return fail(HostFrozenSourceLaunchReason::credentials,
                    HostFrozenSourceBindingOperation::credentials, credentials, fd);
        }
        preparation_step = 12U; // Subreaper owner.
        const auto queried = streams.subreaper(false, &original_subreaper);
        if (queried.value || queried.error_number) return false;
        int enabled = 1;
        const auto installed = streams.subreaper(true, &enabled);
        if (installed.value || installed.error_number) return false;
        subreaper_owned = true;
        std::array<int, 19> inherited{0, 1, 2};
        std::size_t inherited_count = 3U;
        for (const auto descriptor : descriptors) if (descriptor >= 3) inherited[inherited_count++] = descriptor;
        preparation_step = 13U; // Closed controller record.
        const auto encoded = encode_host_frozen_source_launch_record(grant);
        if (!encoded) return fail(encoded.error(), HostFrozenSourceBindingOperation::decode);
        std::string hex; hex.reserve(2048);
        for (const auto byte : *encoded) { hex.push_back(hexadecimal[std::to_integer<unsigned>(byte) >> 4U]); hex.push_back(hexadecimal[std::to_integer<unsigned>(byte) & 15U]); }
        const auto parent = std::to_string(streams.process_id());
        // The executable-local synthetic fixture authenticates its isolated
        // governed repository. Its test tools remain the compiled source
        // inputs; they do not become that repository's source authority.
        const std::string script = synthetic ? EPYCBENCH_SOURCE_TEST_PROCESS_SCRIPT :
            repository + "/tools/monitoring_host_frozen_process.py";
        std::array<std::string, 14> text{"python3", "-I", "-B", script,
            synthetic ? "--synthetic-source-controller" : "--source-controller", "24", parent, hex,
            repository, "25", "26", "27", output_path, {}};
        std::array<char*, 14> arguments{};
        for (std::size_t i{}; i < text.size() - 1U; ++i) arguments[i] = text[i].data();
        std::string fixture_environment;
        if (synthetic) {
            if (!fixture_setup || !*fixture_setup) return false;
            fixture_environment = "EPYCBENCH_FROZEN_WORKFLOW_FIXTURE_OUTPUT=";
            fixture_environment += fixture_setup;
        }
        std::array<char*, 4> environment{const_cast<char*>("LC_ALL=C"), const_cast<char*>("PATH=/usr/bin:/bin"),
            synthetic ? fixture_environment.data() : nullptr, nullptr};
        const std::array mappings{work::SourceLaunchDescriptorMapping{descriptors[7], 24},
            work::SourceLaunchDescriptorMapping{descriptors[1], 25}, work::SourceLaunchDescriptorMapping{descriptors[2], 26},
            work::SourceLaunchDescriptorMapping{descriptors[3], 27}};
        work::SourceLaunchSpec spec{descriptors[3], arguments.data(), environment.data(), mappings, std::span{inherited}.first(inherited_count),
            {core::ByteCount::from_base_units(20U * gib), core::ByteCount::from_base_units(UINT64_C(8388608)), 1024},
            streams.process_id(), launch.origin_ns, launch.end_ns, work::SourceLaunchPhase::q5, 1, descriptors[0]};
        preparation_step = 14U; // Actual controller launch.
        if (!invocation->launch(Role::controller, charge(Role::controller, work::SourceLaunchPhase::q5), spec)) return false;
        if (!close(7)) return false;
        std::array<int, 19> borrowed{0, 1, 2};
        std::size_t borrowed_count = 3U;
        for (const auto descriptor : descriptors) if (descriptor >= 3) borrowed[borrowed_count++] = descriptor;
        if (!invocation->prepare_receive_bank(std::span{borrowed}.first(borrowed_count), channels)) return false;
        receive_bank = true;
        preparation_step = 15U; // Registered controller delivery.
        return send(grant);
    }
    auto run() -> bool {
        while (++turns <= 108000U + 4096U) {
            if (!guard() || !drain()) return false;
            HostFrozenSourceLaunchBytes bytes{}; work::SourceLaunchAncillary ancillary{};
            const auto result = invocation->receive_channel(descriptors[6], bytes, ancillary);
            if (!result) return false;
            if (result->value == static_cast<long>(bytes.size()) && !result->error_number) {
                if (!ancillary.credentials || ancillary.peer.pid != invocation->slot(Role::controller).pid ||
                    ancillary.peer.uid != streams.effective_uid())
                    return fail(HostFrozenSourceLaunchReason::credentials, HostFrozenSourceBindingOperation::credentials);
                const auto value = decode_host_frozen_source_launch_record(bytes);
                if (!value || !handle(*value)) return false;
            } else if (!(result->value < 0 && (result->error_number == EAGAIN || result->error_number == EWOULDBLOCK || result->error_number == EINTR))) {
                if (!(terminal_requested && result->value == 0 && !result->error_number))
                    return fail(HostFrozenSourceLaunchReason::eof, HostFrozenSourceBindingOperation::receive, *result, descriptors[6]);
            }
            const auto terminal = invocation->observe_terminal(Role::controller);
            if (!terminal) return false;
            if (*terminal) {
                const auto status = invocation->slot(Role::controller).native_wait_status;
                return terminal_requested && status && WIFEXITED(*status) && WEXITSTATUS(*status) == 0;
            }
            if (result->value < 0) static_cast<void>(streams.pause(100));
        }
        return fail(HostFrozenSourceLaunchReason::capacity, HostFrozenSourceBindingOperation::guard);
    }
    auto cleanup() noexcept -> bool {
        bool confirmed = !uncertain;
        if (governed && (!workflow_completed || !terminal_requested || token.requested() || first || (invocation && invocation->primary()) || first_input_failure)) {
            request_policy(false);
            if (!policy_channels_ready && descriptors[11] >= 3) {
                struct stat info{};
                const auto checked = files.stat_fd(files.context, descriptors[11], &info);
                const auto flags = streams.output_flags(descriptors[11], false, 0);
                const auto access = flags.value & O_ACCMODE;
                if (!checked.value && !checked.error_number && S_ISFIFO(info.st_mode) &&
                    (info.st_mode & 07777U) == 0600U && info.st_uid == streams.effective_uid() &&
                    flags.value >= 0 && !flags.error_number && (access == O_WRONLY || access == O_RDWR)) {
                    policy_flags[0] = static_cast<int>(flags.value);
                    if (!(flags.value & O_NONBLOCK)) {
                        policy_flags_owned[0] = true;
                        const auto changed = streams.output_flags(descriptors[11], true, policy_flags[0] | O_NONBLOCK);
                        if (changed.value || changed.error_number) uncertain = true;
                    }
                    const auto verified = streams.output_flags(descriptors[11], false, 0);
                    policy_channels_ready = verified.value == (policy_flags[0] | O_NONBLOCK) && !verified.error_number;
                }
            }
            //A fixed nonblocking turn sends cancellation before ordinary
            //process cleanup. EAGAIN/partial command retains its same cell.
            static_cast<void>(policy_flush());
        }
        if (inputs) { if (!inputs->retire()) confirmed = false; else inputs.reset(); }
        if (invocation && invocation->slot(Role::controller).occupied) {
            std::uint64_t now{};
            const auto sampled = sample_now(&now);
            if (sampled.value || sampled.error_number || now < last || now > UINT64_MAX - UINT64_C(15000000000)) confirmed = false;
            else {
                last = now;
                const auto phase_cleanup = grant.phase_end_ns > UINT64_MAX - policy_restore ? UINT64_MAX : grant.phase_end_ns + policy_restore;
                const auto end = std::min({phase_cleanup, now + UINT64_C(15000000000),
                    governed_handoff ? governed_handoff->window_end : UINT64_MAX});
                // A dead leader can leave its collector/validator alive. A
                // normal terminal handshake already retired every external
                // role; every other outcome must stop the actual whole group.
                const bool descendants_possible = !terminal_requested ||
                    std::ranges::any_of(std::array{Role::collector, Role::validator, Role::git},
                        [&](Role role) noexcept { return invocation->slot(role).occupied; });
                const auto stop = [&](platform_linux::ProcessSignal signal) noexcept {
                    if (invocation->slot(Role::controller).group_confirmed)
                        static_cast<void>(invocation->request_group_stop(Role::controller, signal));
                    else if (!invocation->slot(Role::controller).terminal)
                        static_cast<void>(invocation->request_stop(Role::controller, signal));
                };
                if (!invocation->slot(Role::controller).terminal || descendants_possible)
                    stop(platform_linux::ProcessSignal::terminate);
                bool killed{};
                for (std::size_t attempts{}; attempts < 152U && guard(true, end); ++attempts) {
                    static_cast<void>(drain());
                    const auto terminal = invocation->observe_terminal(Role::controller);
                    if (!terminal) { confirmed = false; break; }
                    if (*terminal) break;
                    if (!killed && last >= now + UINT64_C(5000000000)) {
                        stop(platform_linux::ProcessSignal::kill); killed = true;
                    }
                    static_cast<void>(streams.pause(100));
                }
                if (!invocation->slot(Role::controller).terminal) confirmed = false;
                else {
                    bool no_children{};
                    for (std::size_t attempts{}; attempts < 152U && guard(true, end); ++attempts) {
                        const auto waited = invocation->reap_orphans();
                        if (!waited) { confirmed = false; break; }
                        if (*waited) { no_children = true; break; }
                        if (!killed && last >= now + UINT64_C(5000000000)) {
                            stop(platform_linux::ProcessSignal::kill); killed = true;
                        }
                        static_cast<void>(streams.pause(100));
                    }
                    confirmed = confirmed && no_children;
                }
            }
        }
        if (invocation) {
            static_cast<void>(invocation->close_received());
            for (const auto role : {Role::collector, Role::validator, Role::git, Role::controller}) {
                if (invocation->slot(role).occupied && !invocation->retire(role)) confirmed = false;
            }
            if (receive_bank && !invocation->retire_receive_bank()) confirmed = false;
            else receive_bank = false;
            if (invocation->has_owned_resources() || invocation->uncertain()) confirmed = false;
        }
        //Only confirmed ordinary retirement can authorize RELEASE. The
        //administrator is external: result parsing never waits/reaps it.
        if (!retire_policy(confirmed)) confirmed = false;
        for (std::size_t index{}; index < descriptors.size(); ++index) if (!close(index)) confirmed = false;
        if (!confirmed) return false;
        if (subreaper_owned) {
            auto original = original_subreaper;
            const auto restored = streams.subreaper(true, &original);
            if (restored.value || restored.error_number) return fail(HostFrozenSourceLaunchReason::uncertain, HostFrozenSourceBindingOperation::guard, restored);
            subreaper_owned = false;
        }
        if (signal_owned) {
            const auto restored = signals.close();
            if (!restored) { retain_signal(restored.error()); return false; }
            if (restored->residual) {
                uncertain = true;
                return fail(HostFrozenSourceLaunchReason::uncertain, HostFrozenSourceBindingOperation::guard);
            }
            signal_owned = false;
            if (restored->observed != platform_linux::detail::ControllerSignal::none) {
                static_cast<void>(token.try_request(restored->observed == platform_linux::detail::ControllerSignal::interrupt
                    ? runtime::ipc::CancelReason::user_interrupt : runtime::ipc::CancelReason::controller_shutdown));
                if (!cancellation_observed) {
                    cancellation_observed = true;
                    static_cast<void>(fail(HostFrozenSourceLaunchReason::cancelled, HostFrozenSourceBindingOperation::guard));
                }
            }
        }
        if (build) {
            auto retired = build->retire(); core::detail::ExpectedErrorCleanup build_cleanup{retired};
            if (!retired) return false;
            build.reset();
        }
        std::string{}.swap(packet); std::string{}.swap(candidate);
        std::string{}.swap(repository); std::string{}.swap(qid); std::string{}.swap(output_path);
        std::string{}.swap(machine_plan);
        if (invocation && invocation->primary()) retired_launch_failure = *invocation->primary();
        invocation.reset();
        if (baseline && !baseline->retire()) return false;
        baseline.reset();
        return true;
    }
};

auto run_source_invocation(int argc, char* argv[],
    runtime::ControllerProcessProductionAuthority& authority, runtime::RunnerCancellation& cancellation,
    HostFrozenSourceLaunchClock& clock, std::uint64_t original_main_entry_ns,
    const HostFrozenSourceSyntheticDispatcher* synthetic, const char* fixture_setup = nullptr,
    const work::SourceLaunchOperations* observed_launch = nullptr) noexcept -> int {
    // Borrowing the genuine main authority never resets/creates its ID source.
    static_cast<void>(authority);
    const bool governed_entry = argc >= 2 && argv && argv[1] &&
        std::string_view{argv[1]} == "__host-frozen-source-governed";
    const bool invalid_clock = clock.cancellation != &cancellation || cancellation.requested() ||
        clock.actual.value != 0 || clock.actual.error_number ||
        (clock.current.raw_present && clock.current.reason == work::SourceLaunchReason::clock) ||
        original_main_entry_ns > UINT64_MAX - UINT64_C(10810000000000);
    if (!governed_entry && invalid_clock) return 78;
    if (!is_host_frozen_source_invocation(argc, argv)) return 64;
    const bool collector = std::string_view{argv[1]} == "__host-frozen-source-collector";
    if ((governed_entry && argc != 13) || (!governed_entry && !collector && argc != 5) ||
        (collector && argc != 8 && argc != 10)) return 64;
    if (collector && (!source_descriptor_argument(argv[2]) || !argv[3] || !argv[4] ||
                      std::string_view{argv[4]}.size() != 2048U)) return 64;
    const auto root = collector ? argv[5] : argv[2];
    const auto qid = collector ? argv[6] : argv[3];
    const bool invalid_root = !root || !qid || std::string_view{root}.empty() || std::string_view{root}.size() > 511U ||
        std::string_view{qid}.empty() || std::string_view{qid}.size() > 128U ||
        std::string_view{qid} == "." || std::string_view{qid} == ".." || std::string_view{qid}.find('/') != std::string_view::npos;
    if (!governed_entry && invalid_root) return 64;
    if (collector) {
        int channel{}, parent{}, output{}, retained{-1}, qualification{-1};
        const auto parse = [](const char* text, int& value) noexcept {
            if (!text) return false;
            const std::string_view bytes{text};
            const auto result = std::from_chars(bytes.data(), bytes.data() + bytes.size(), value);
            return result.ec == std::errc{} && result.ptr == bytes.data() + bytes.size();
        };
        if (!parse(argv[2], channel) || !parse(argv[3], parent) || !parse(argv[7], output) ||
            parent <= 0 || output < 3 || channel == output) return 64;
        if (argc == 10 && (!parse(argv[8], retained) || !parse(argv[9], qualification) ||
            retained < 3 || qualification < 3 || retained == qualification || retained == channel ||
            retained == output || qualification == channel || qualification == output)) return 64;
        HostFrozenSourceLaunchBytes bytes{};
        for (std::size_t index{}; index < bytes.size(); ++index) {
            const auto high = hexadecimal.find(argv[4][index * 2U]);
            const auto low = hexadecimal.find(argv[4][index * 2U + 1U]);
            if (high == std::string_view::npos || low == std::string_view::npos) return 64;
            bytes[index] = static_cast<std::byte>((high << 4U) | low);
        }
        const auto grant = decode_host_frozen_source_launch_record(bytes);
        if (!grant || grant->type != HostFrozenSourceLaunchType::grant ||
            grant->role != work::SourceLaunchRole::collector ||
            (grant->phase == work::SourceLaunchPhase::q5 ? argc != 8 : argc != 10)) return 64;
        return HostFrozenSourceLaunchState::collect(channel, parent, output, retained, qualification, *grant,
            root, qid, authority, cancellation, clock, synthetic);
    }
    SourceInvocationDispatcher dispatcher{cancellation, clock, original_main_entry_ns};
    bool completed{};
    const bool arguments_owned = !governed_entry || dispatcher.adopt_governed_arguments(argv);
    try {
        const auto proceed = [&]() -> bool {
            return dispatcher.prepare(root, qid, argv[4],
                governed_entry ? dispatcher.grant.origin_ns : original_main_entry_ns,
                synthetic, fixture_setup, observed_launch) && dispatcher.run();
        };
        if (governed_entry)
            completed = dispatch_host_frozen_source_governed_entry(invalid_clock, invalid_root,
                arguments_owned, qid ? std::string_view{qid} : std::string_view{},
                [&](HostFrozenSourceLaunchReason reason) noexcept {
                    static_cast<void>(dispatcher.fail(reason, HostFrozenSourceBindingOperation::decode, clock.actual));
                }, proceed);
        else completed = proceed();
    } catch (...) {
        static_cast<void>(dispatcher.fail(HostFrozenSourceLaunchReason::contained, HostFrozenSourceBindingOperation::guard));
    }
    dispatcher.workflow_completed = completed;
    const bool retired = dispatcher.cleanup();
    if (dispatcher.invocation && dispatcher.invocation->primary() && !dispatcher.retired_launch_failure)
        dispatcher.retired_launch_failure = *dispatcher.invocation->primary();
    const bool successful = retired && completed && !dispatcher.first && !dispatcher.retired_launch_failure &&
        !dispatcher.first_input_failure && !cancellation.requested();
    if (!successful) {
        std::fprintf(stderr, "source_invocation_refusal preparation_step=%u\n", dispatcher.preparation_step);
        if (dispatcher.first)
            std::fprintf(stderr, "source_primary reason=%u operation=%u result=%ld errno=%d fd=%d\n",
                static_cast<unsigned>(dispatcher.first->reason), static_cast<unsigned>(dispatcher.first->operation),
                dispatcher.first->native_result, dispatcher.first->error_number, dispatcher.first->descriptor);
        for (const auto* fact : {&dispatcher.first_input_failure, &dispatcher.retired_launch_failure})
            if (*fact)
                std::fprintf(stderr, "source_native_primary reason=%u operation=%u result=%ld errno=%d fd=%d\n",
                    static_cast<unsigned>((*fact)->reason), static_cast<unsigned>((*fact)->operation),
                    (*fact)->native_result, (*fact)->error_number, (*fact)->descriptor);
        if (dispatcher.first_resource_failure)
            std::fprintf(stderr, "source_resource_refusal reason=%u\n",
                static_cast<unsigned>(*dispatcher.first_resource_failure));
        if (dispatcher.first_signal_failure)
            std::fprintf(stderr, "source_signal_refusal stage=%u reason=%u errno=%d\n",
                static_cast<unsigned>(dispatcher.first_signal_failure->stage),
                static_cast<unsigned>(dispatcher.first_signal_failure->reason),
                dispatcher.first_signal_failure->error_number);
        if (dispatcher.first_core_code_size)
            std::fprintf(stderr, "source_core_refusal code=%.*s\n", static_cast<int>(dispatcher.first_core_code_size),
                dispatcher.first_core_code.data());
        // Forward only the controller's fixed protocol and bounded internal
        // operation token. Arbitrary stderr/exception text remains private and
        // cannot become a diagnostic pathname or artifact dump.
        const std::string_view diagnostic{dispatcher.controller_diagnostic.data(), dispatcher.controller_diagnostic_size};
        constexpr std::string_view prefix = "source_controller_refusal operation=";
        if (!dispatcher.controller_diagnostic_overflow && diagnostic.starts_with(prefix) &&
            diagnostic.ends_with('\n') && diagnostic.size() > prefix.size() + 1U &&
            diagnostic.size() <= prefix.size() + 97U) {
            const auto operation = diagnostic.substr(prefix.size(), diagnostic.size() - prefix.size() - 1U);
            if (std::ranges::all_of(operation, [](char value) noexcept {
                return (value >= 'a' && value <= 'z') || (value >= '0' && value <= '9') || value == '_';
            })) std::fprintf(stderr, "%.*s", static_cast<int>(diagnostic.size()), diagnostic.data());
        }
    }
    static_cast<void>(dispatcher.diagnose_policy_timing(retired));
    if (!retired) dispatcher.streams.retain_until_outer_termination();
    return successful && !cancellation.requested() ? 0 : 78;
}
}
auto observe_host_frozen_source_governed_report_for_test(
    const HostFrozenSourceGovernedHandoff& handoff, runtime::RunnerCancellation& cancellation,
    HostFrozenSourceLaunchClock& clock, platform_linux::detail::TopologySourceOperations files,
    work::SourceLaunchStreamOperations streams, work::GuardOperations observed_clock,
    std::uint64_t original_observation) noexcept -> HostFrozenSourceGovernedReportTestObservation {
    SourceInvocationDispatcher owner{cancellation, clock, original_observation, files, streams, observed_clock};
    //The fixed cells precede all injected IO. This path has no ledger/child/
    //main-authority or readiness grant and cannot mint a source capability.
    static_cast<void>(owner.adopt(11U, {81, 0}));
    static_cast<void>(owner.adopt(12U, {82, 0}));
    owner.governed = true; owner.workflow_completed = true; owner.terminal_requested = true;
    owner.governed_handoff.emplace(handoff); owner.policy_channels_ready = true;
    owner.policy_total = handoff.ready_bytes;
    owner.grant.phase_end_ns = handoff.work_end; owner.grant.entry_end_ns = handoff.work_end;
    const bool retired = owner.retire_policy(true);
    const bool control_closed = owner.close(11U);
    const bool report_closed = owner.close(12U);
    //Tests supply truthful successful consuming-close leaves. A failed close
    //retains the whole actual actor rather than clearing an uncertain slot.
    if (!control_closed || !report_closed || owner.uncertain) streams.retain_until_outer_termination();
    const bool confirmed = retired && control_closed && report_closed;
    const bool projected = owner.diagnose_policy_timing(confirmed);
    std::optional<std::uint64_t> elapsed, exposure;
    if (owner.policy_restored_at_ns) {
        elapsed = *owner.policy_restored_at_ns - handoff.window_origin;
        exposure = handoff.exposure_used + *elapsed;
    }
    return {owner.policy_restored, owner.policy_report_eof, retired && control_closed && report_closed,
        cancellation.requested(), owner.policy_command && std::string_view{owner.policy_command} == "CANCEL\n",
        owner.first, owner.later_count, owner.policy_restored_at_ns, elapsed, exposure, projected};
}
auto run_host_frozen_source_invocation(int argc, char* argv[],
    runtime::ControllerProcessProductionAuthority& authority, runtime::RunnerCancellation& cancellation,
    HostFrozenSourceLaunchClock& clock, std::uint64_t original_main_entry_ns) noexcept -> int {
    return run_source_invocation(argc, argv, authority, cancellation, clock, original_main_entry_ns, nullptr);
}
auto run_host_frozen_synthetic_collector(int argc, char* argv[],
    runtime::ControllerProcessProductionAuthority& authority, runtime::RunnerCancellation& cancellation,
    HostFrozenSourceLaunchClock& clock, std::uint64_t original_main_entry_ns,
    const HostFrozenSourceSyntheticDispatcher& synthetic) noexcept -> int {
    if (argc < 2 || !argv || !argv[1] || std::string_view{argv[1]} != "__host-frozen-source-collector") return 64;
    return run_source_invocation(argc, argv, authority, cancellation, clock, original_main_entry_ns, &synthetic);
}
auto run_host_frozen_synthetic_outer(int argc, char* argv[],
    runtime::ControllerProcessProductionAuthority& authority, runtime::RunnerCancellation& cancellation,
    HostFrozenSourceLaunchClock& clock, std::uint64_t original_main_entry_ns,
    const HostFrozenSourceSyntheticDispatcher& synthetic, const char* fixture_setup,
    bool inject_cleanup_wait_failure) noexcept -> int {
    if (!argv || !argv[1] || !((argc == 5 && std::string_view{argv[1]} == "__host-frozen-source") ||
        (argc == 13 && std::string_view{argv[1]} == "__host-frozen-source-governed")) ||
        !synthetic.build.manifest || !synthetic.build.fault || !fixture_setup || !*fixture_setup) return 64;
    // Test containment observes the real nested group before its startup gate
    // can release. This changes no production launch leaf or process role.
    struct ObservedLaunch final {
        work::SourceLaunchOperations production{work::production_source_launch_operations()};
        long raw_pid{};
        Raw notification{};
        bool notified{}, cleanup_injection_pending{};
    } observed;
    observed.cleanup_injection_pending = inject_cleanup_wait_failure;
    auto operations = observed.production;
    operations.context = &observed;
    operations.fork = +[](void* opaque) noexcept -> Raw {
        auto& owner = *static_cast<ObservedLaunch*>(opaque);
        const auto actual = owner.production.fork(owner.production.context);
        if (actual.value > 0) owner.raw_pid = actual.value;
        return actual;
    };
    operations.gate = +[](void* opaque, int descriptor) noexcept -> Raw {
        auto& owner = *static_cast<ObservedLaunch*>(opaque);
        if (owner.notified || owner.raw_pid <= 0 || owner.raw_pid > INT32_MAX) return {-1, EINVAL};
        owner.notified = true; // One consuming notification, before release.
        std::array<std::byte, 8> bytes{};
        store(bytes, 0, static_cast<std::uint64_t>(owner.raw_pid), bytes.size());
        const auto written = work::production_source_launch_stream_operations().write(1, bytes);
        owner.notification = written;
        if (written.value != static_cast<long>(bytes.size()) || written.error_number)
            return {-1, written.error_number ? written.error_number : EIO};
        return owner.production.gate(owner.production.context, descriptor);
    };
    operations.process.context = &observed;
    operations.process.wait_pid = +[](void* opaque, int pid, int* status, int options) noexcept -> Raw {
        auto& owner = *static_cast<ObservedLaunch*>(opaque);
        const auto actual = owner.production.process.wait_pid(owner.production.process.context, pid, status, options);
        if (owner.cleanup_injection_pending && pid == -1 && actual.value == -1 && actual.error_number == ECHILD) {
            owner.cleanup_injection_pending = false;
            *status = 0;
            // Target-only injected association failure after the real process
            // graph is empty. The next call still observes genuine ECHILD.
            return {99, 0};
        }
        return actual;
    };
    operations.process.send_signal = +[](void* opaque, int pid, int signal) noexcept -> Raw {
        auto& owner = *static_cast<ObservedLaunch*>(opaque);
        return owner.production.process.send_signal(owner.production.process.context, pid, signal);
    };
    const auto result = run_source_invocation(argc, argv, authority, cancellation, clock, original_main_entry_ns,
        &synthetic, fixture_setup, &operations);
    // A refusal test must demonstrate that its intended fault was reached.
    return inject_cleanup_wait_failure && observed.cleanup_injection_pending ? 64 : result;
}
} // namespace epycbench::monitoring::qualification::detail
