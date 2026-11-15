#include "results/kernel_approval_owner.hpp"
#include "results/kernel_report_review_literals.hpp"
#include <algorithm>
#include <limits>

namespace epycbench::results::kernel_approval {
namespace {
struct Rejected {
    std::string_view code;
};
void need(bool b, std::string_view code = "invalid_approval") {
    if (!b)
        throw Rejected{code};
}
void add(std::size_t& n, std::size_t amount, std::size_t limit) {
    need(n <= limit && amount <= limit - n, "capacity");
    n += amount;
}
auto error(std::string_view code) -> core::Error {
    return {.category = core::ErrorCategory::results,
            .code = core::StableErrorCode{"results.kernel_approval." + std::string{code}},
            .message = "Kernel approval admission rejected",
            .operation = "retain_kernel_approval",
            .os_error = std::nullopt,
            .context = nlohmann::json::object()};
}
constexpr std::array<std::string_view, 46> keys{"schema",
                                                "framework_version",
                                                "policy_version",
                                                "report_schema",
                                                "implementation_id",
                                                "issue",
                                                "source_commit",
                                                "title",
                                                "approval",
                                                "artifacts",
                                                "actor",
                                                "recorded_at_utc",
                                                "disposition",
                                                "statement",
                                                "semantic_items",
                                                "id",
                                                "automated_disposition",
                                                "compiler_family",
                                                "function_sha256",
                                                "generated_human_review_status",
                                                "human_report",
                                                "machine_report",
                                                "object_sha256",
                                                "qualification_preset",
                                                "path",
                                                "sha256",
                                                "qualification",
                                                "route",
                                                "implementer_agent_id",
                                                "author_agent_ids",
                                                "input_sha256",
                                                "reviewers",
                                                "agent_id",
                                                "run_id",
                                                "model",
                                                "started_at_utc",
                                                "completed_at_utc",
                                                "assessments",
                                                "compiler_version",
                                                "compiler_target",
                                                "machine_report_sha256",
                                                "human_report_sha256",
                                                "conclusion",
                                                "instruction_locations",
                                                "explanation",
                                                "uncertainty"};
constexpr std::size_t absent = 1024;
enum class Kind { string, object, array };
struct Node {
    Kind kind{};
    std::size_t key{keys.size()}, first{absent}, next{absent};
    std::string_view raw;
    std::size_t length{};
};
// One scalar at a time; raw UTF-8 and escaped codepoints produce the same bytes.
struct Decoder {
    std::string_view input;
    std::size_t pos{};
    auto take() -> unsigned char {
        need(pos < input.size(), "malformed_json");
        return static_cast<unsigned char>(input[pos++]);
    }
    auto hex4() -> std::uint32_t {
        std::uint32_t n{};
        for (int i = 0; i != 4; ++i) {
            auto c = take();
            unsigned d = c >= '0' && c <= '9'   ? static_cast<unsigned>(c - '0')
                         : c >= 'a' && c <= 'f' ? static_cast<unsigned>(c - 'a' + 10)
                         : c >= 'A' && c <= 'F' ? static_cast<unsigned>(c - 'A' + 10)
                                                : 16U;
            need(d < 16, "malformed_json");
            n = n * 16 + d;
        }
        return n;
    }
    auto scalar() -> std::uint32_t {
        auto c = take();
        need(c >= 32, "malformed_json");
        if (c == '\\') {
            switch (take()) {
            case '"':
                return '"';
            case '\\':
                return '\\';
            case '/':
                return '/';
            case 'b':
                return '\b';
            case 'f':
                return '\f';
            case 'n':
                return '\n';
            case 'r':
                return '\r';
            case 't':
                return '\t';
            case 'u': {
                auto n = hex4();
                if (n >= 0xd800 && n <= 0xdbff) {
                    need(take() == '\\' && take() == 'u', "malformed_json");
                    auto low = hex4();
                    need(low >= 0xdc00 && low <= 0xdfff, "malformed_json");
                    n = 0x10000 + (n - 0xd800) * 1024 + low - 0xdc00;
                } else
                    need(n < 0xdc00 || n > 0xdfff, "malformed_json");
                return n;
            }
            default:
                throw Rejected{"malformed_json"};
            }
        }
        if (c < 128)
            return c;
        unsigned count{};
        std::uint32_t n{}, minimum{};
        if (c >= 0xc2 && c <= 0xdf) {
            count = 1;
            n = c & 31U;
            minimum = 0x80;
        } else if (c >= 0xe0 && c <= 0xef) {
            count = 2;
            n = c & 15U;
            minimum = 0x800;
        } else if (c >= 0xf0 && c <= 0xf4) {
            count = 3;
            n = c & 7U;
            minimum = 0x10000;
        } else
            throw Rejected{"malformed_json"};
        for (unsigned i = 0; i != count; ++i) {
            auto b = take();
            need((b & 0xc0U) == 0x80, "malformed_json");
            n = (n << 6) | (b & 63U);
        }
        need(n >= minimum && n <= 0x10ffff && (n < 0xd800 || n > 0xdfff), "malformed_json");
        return n;
    }
};
template <class Emit> void decoded(std::string_view raw, Emit emit) {
    Decoder d{raw};
    while (d.pos != raw.size()) {
        auto n = d.scalar();
        if (n < 0x80)
            emit(static_cast<char>(n));
        else {
            if (n < 0x800)
                emit(static_cast<char>(0xc0 | (n >> 6)));
            else {
                if (n < 0x10000)
                    emit(static_cast<char>(0xe0 | (n >> 12)));
                else {
                    emit(static_cast<char>(0xf0 | (n >> 18)));
                    emit(static_cast<char>(0x80 | ((n >> 12) & 63)));
                }
                emit(static_cast<char>(0x80 | ((n >> 6) & 63)));
            }
            emit(static_cast<char>(0x80 | (n & 63)));
        }
    }
}
template <std::size_t Capacity, std::size_t Depth> struct ScannerStock {
    std::string_view input;
    Limits limits;
    Usage usage;
    std::size_t pos{}, count{};
    std::array<Node, Capacity> nodes{};
    std::array<char, 32> key_buffer{};
    void space() {
        while (pos < input.size() && (input[pos] == ' ' || input[pos] == '\r' ||
                                      input[pos] == '\n' || input[pos] == '\t'))
            ++pos;
    }
    auto take() -> char {
        need(pos < input.size(), "malformed_json");
        return input[pos++];
    }
    auto string(bool key) -> Node {
        need(take() == '"', "malformed_json");
        auto start = pos;
        Decoder d{input, pos};
        std::size_t length{};
        while (true) {
            need(d.pos < input.size(), "malformed_json");
            if (input[d.pos] == '"')
                break;
            auto n = d.scalar();
            auto bytes = n < 0x80 ? 1U : n < 0x800 ? 2U : n < 0x10000 ? 3U : 4U;
            add(length, bytes, limits.string_bytes);
            add(usage.string_bytes, bytes, limits.string_bytes);
            if (!key)
                add(usage.retained_bytes, bytes, limits.retained_bytes);
        }
        pos = d.pos + 1;
        return {Kind::string, keys.size(), absent, absent, input.substr(start, d.pos - start),
                length};
    }
    auto value(std::size_t depth, std::size_t key = keys.size()) -> std::size_t {
        need(depth <= limits.depth, "capacity");
        need(depth <= Depth); // Each closed shape has its own finite scanner.
        usage.depth = std::max(usage.depth, depth);
        add(usage.nodes, 1, limits.nodes);
        need(count < nodes.size());
        auto index = count++;
        space();
        need(pos < input.size(), "malformed_json");
        if (input[pos] == '"')
            nodes[index] = string(false);
        else {
            char first = take();
            need(first == '{' || first == '[', "malformed_json");
            nodes[index].kind = first == '{' ? Kind::object : Kind::array;
            char end = first == '{' ? '}' : ']';
            space();
            std::uint64_t mask{};
            std::size_t last = absent;
            if (pos < input.size() && input[pos] == end)
                ++pos;
            else
                while (true) {
                    add(usage.entries, 1, limits.entries);
                    std::size_t member = keys.size();
                    if (first == '{') {
                        space();
                        auto name = string(true);
                        need(name.length <= key_buffer.size());
                        std::size_t n{};
                        decoded(name.raw, [&](char c) { key_buffer[n++] = c; });
                        auto found =
                            std::ranges::find(keys, std::string_view{key_buffer.data(), n});
                        need(found != keys.end());
                        member = static_cast<std::size_t>(found - keys.begin());
                        if constexpr (Capacity == 128)
                            need(member < 26); // Preserve the legacy closed key vocabulary.
                        need((mask & (std::uint64_t{1} << member)) == 0, "duplicate_member");
                        mask |= std::uint64_t{1} << member;
                        space();
                        need(take() == ':', "malformed_json");
                    }
                    auto child = value(depth + 1, member);
                    if (last == absent)
                        nodes[index].first = child;
                    else
                        nodes[last].next = child;
                    last = child;
                    space();
                    auto next = take();
                    if (next == end)
                        break;
                    need(next == ',', "malformed_json");
                }
        }
        nodes[index].key = key;
        return index;
    }
    void run() {
        (void)value(0);
        space();
        need(pos == input.size(), "malformed_json");
    }
    auto child(const Node& n, std::string_view key) const -> const Node& {
        for (auto i = n.first; i != absent; i = nodes[i].next)
            if (keys[nodes[i].key] == key)
                return nodes[i];
        throw Rejected{"invalid_approval"};
    }
    void object(const Node& n, std::initializer_list<std::string_view> names) const {
        need(n.kind == Kind::object);
        std::size_t count_members{};
        for (auto i = n.first; i != absent; i = nodes[i].next) {
            // Keep each closed scanner's physical bound at the indexed use.
            need(i < nodes.size() && i < count);
            const auto& member = nodes[i];
            need(member.key < keys.size());
            need(std::ranges::find(names, keys[member.key]) != names.end());
            ++count_members;
        }
        need(count_members == names.size());
    }
    auto equal(const Node& n, std::string_view text) const -> bool {
        if (n.kind != Kind::string || n.length != text.size())
            return false;
        std::size_t i{};
        bool same = true;
        decoded(n.raw, [&](char c) {
            same = same && c == text[i];
            ++i;
        });
        return same;
    }
    void eq(const Node& n, std::string_view text) const { need(equal(n, text)); }
    void nonempty(const Node& n) const { need(n.kind == Kind::string && n.length != 0); }
    void hex(const Node& n, std::size_t width) const {
        need(n.kind == Kind::string && n.length == width);
        decoded(n.raw, [](char c) { need((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')); });
    }
    auto copy(const Node& n) const -> std::string {
        std::string result;
        result.reserve(n.length);
        decoded(n.raw, [&](char c) { result.push_back(c); });
        return result;
    }
};
using Scanner = ScannerStock<128, 4>;
using AgentScanner = ScannerStock<1024, 8>;
void calendar(const Node& n) {
    need(n.kind == Kind::string && n.length == 20);
    std::array<char, 20> s{};
    std::size_t i{};
    decoded(n.raw, [&](char c) { s[i++] = c; });
    for (std::size_t j = 0; j < s.size(); ++j) {
        char literal = j == 4 || j == 7     ? '-'
                       : j == 10            ? 'T'
                       : j == 13 || j == 16 ? ':'
                       : j == 19            ? 'Z'
                                            : 0;
        need(literal ? s[j] == literal : s[j] >= '0' && s[j] <= '9');
    }
    auto number = [&](std::size_t start, std::size_t length) {
        unsigned result{};
        for (std::size_t j = start; j < start + length; ++j)
            result = result * 10 + static_cast<unsigned>(s[j] - '0');
        return result;
    };
    auto year = number(0, 4), month = number(5, 2), day = number(8, 2);
    constexpr std::array<unsigned, 12> days{31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    need(year != 0);
    const auto month_index = month - 1U;
    need(month_index < days.size());
    auto maximum = days[month_index] +
                   (month == 2 && year % 4 == 0 && (year % 100 != 0 || year % 400 == 0) ? 1U : 0U);
    need(day >= 1 && day <= maximum && number(11, 2) < 24 && number(14, 2) < 60 &&
         number(17, 2) < 60);
}
auto validate(const auto& s, bool agent) -> kernel_report::Family {
    const auto& root = s.nodes[0];
    s.object(root, {"schema", "framework_version", "policy_version", "report_schema",
                    "implementation_id", "issue", "source_commit", "title",
                    agent ? "qualification" : "approval", "artifacts"});
    auto get = [&](std::string_view k) -> const Node& { return s.child(root, k); };
    s.eq(get("schema"), agent ? "epycbench.kernel-object-agent-qualification/v1"
                              : "epycbench.kernel-object-qualification-approval/v1");
    s.eq(get("framework_version"), "epycbench.kernel-object-audit/v1");
    s.eq(get("report_schema"), "epycbench.kernel-object-audit-report/v1");
    bool bw = s.equal(get("issue"), "EB-KRN-BW-006");
    s.eq(get("issue"), bw ? "EB-KRN-BW-006" : "EB-KRN-LAT-007");
    s.eq(get("policy_version"), bw ? "epycbench.kernel-object-audit.bandwidth/v1"
                                   : "epycbench.kernel-object-audit.latency/v1");
    s.eq(get("implementation_id"), bw ? "scalar-u64-sequential/v1" : "scalar-u64-random-cycle/v1");
    s.hex(get("source_commit"), 40);
    s.nonempty(get("title"));
    if (!agent) {
        const auto& a = get("approval");
        s.object(a, {"actor", "recorded_at_utc", "disposition", "statement", "semantic_items"});
        s.nonempty(s.child(a, "actor"));
        s.nonempty(s.child(a, "statement"));
        calendar(s.child(a, "recorded_at_utc"));
        s.eq(s.child(a, "disposition"), "approved");
        const auto& items = s.child(a, "semantic_items");
        need(items.kind == Kind::array);
        std::span<const kernel_report::detail::ReviewLiteral> reviews =
            bw ? std::span<const kernel_report::detail::ReviewLiteral>{kernel_report::detail::
                                                                           bandwidth_reviews}
               : std::span<const kernel_report::detail::ReviewLiteral>{
                     kernel_report::detail::latency_reviews};
        auto item = items.first;
        for (auto review : reviews) {
            need(item != absent);
            s.object(s.nodes[item], {"id", "disposition"});
            s.eq(s.child(s.nodes[item], "id"), review.id);
            s.eq(s.child(s.nodes[item], "disposition"), "supported");
            item = s.nodes[item].next;
        }
        need(item == absent);
    }
    const auto& artifacts = get("artifacts");
    need(artifacts.kind == Kind::array);
    auto artifact = artifacts.first;
    for (std::size_t slot = 0; slot < 2; ++slot) {
        need(artifact != absent);
        const auto& n = s.nodes[artifact];
        s.object(n, {"automated_disposition", "compiler_family", "function_sha256",
                     "generated_human_review_status", "human_report", "machine_report",
                     "object_sha256", "qualification_preset"});
        s.eq(s.child(n, "automated_disposition"), "pass");
        s.eq(s.child(n, "compiler_family"), slot == 0 ? "GCC" : "Clang");
        s.eq(s.child(n, "generated_human_review_status"), "pending");
        s.eq(s.child(n, "qualification_preset"), slot == 0 ? "release" : "release-clang");
        s.hex(s.child(n, "function_sha256"), 64);
        s.hex(s.child(n, "object_sha256"), 64);
        for (auto k : {"human_report", "machine_report"}) {
            const auto& r = s.child(n, k);
            s.object(r, {"path", "sha256"});
            bool human = std::string_view{k} == "human_report";
            s.eq(s.child(r, "path"),
                 slot == 0
                     ? (human ? "gcc/object-code-audit.txt" : "gcc/object-code-audit.json")
                     : (human ? "clang/object-code-audit.txt" : "clang/object-code-audit.json"));
            s.hex(s.child(r, "sha256"), 64);
        }
        artifact = n.next;
    }
    need(artifact == absent);
    return bw ? kernel_report::Family::bandwidth : kernel_report::Family::latency;
}
auto digest_matches(const auto& s, const Node& n, const core::Sha256Digest& digest) -> bool {
    constexpr std::string_view digits = "0123456789abcdef";
    std::array<char, 64> hex{};
    std::size_t i{};
    for (auto byte : digest.bytes()) {
        auto b = std::to_integer<unsigned>(byte);
        hex[i++] = digits[b >> 4];
        hex[i++] = digits[b & 15];
    }
    return s.equal(n, {hex.data(), hex.size()});
}
void associate(const auto& s, kernel_report::Family family, const kernel_report::Owner& report,
               std::string_view human, std::size_t slot, const Node& artifact, bool agent) {
    auto view = report.report();
    need(view.has_value(), "missing_report");
    need(view->family == family && view->disposition == kernel_report::Disposition::pass,
         "association_mismatch");
    const auto& root = s.nodes[0];
    const auto& r = view->content;
    auto match = [&](const Node& n, const kernel_report::Value* value) {
        need(value && s.equal(n, value->text()), "association_mismatch");
    };
    match(s.child(root, "framework_version"), r.member("framework_version"));
    match(s.child(root, "report_schema"), r.member("schema"));
    match(s.child(root, "policy_version"), r.member("family_policy_version"));
    match(s.child(root, "implementation_id"), r.member("implementation_id"));
    match(s.child(root, "source_commit"), r.member("source")->member("commit"));
    need(r.member("source")->member("dirty")->text() == "false", "association_mismatch");
    match(s.child(artifact, "compiler_family"), r.member("compiler")->member("family"));
    match(s.child(artifact, "qualification_preset"), r.member("qualification")->member("preset"));
    match(s.child(root, "policy_version"), r.member("qualification")->member("policy_identifier"));
    need(r.member("qualification")->member("configuration")->text() == "Release" &&
             r.member("human_review")->member("status")->text() == "pending" &&
             r.member("qualification_status")->text() == "pending_human_review",
         "association_mismatch");
    match(s.child(artifact, "function_sha256"), r.member("function")->member("sha256"));
    match(s.child(artifact, "object_sha256"), r.member("object")->member("sha256"));
    need(digest_matches(s, s.child(s.child(artifact, "machine_report"), "sha256"),
                        *report.digest()) &&
             !human.empty() &&
             digest_matches(s, s.child(s.child(artifact, "human_report"), "sha256"),
                            core::sha256(human)),
         "association_mismatch");
    if (!agent) {
        auto item = s.child(s.child(root, "approval"), "semantic_items").first;
        for (const auto& review : r.member("human_review")->member("items")->children()) {
            need(item != absent, "association_mismatch");
            match(s.child(s.nodes[item], "id"), review.member("id"));
            item = s.nodes[item].next;
        }
        need(item == absent && slot < 2, "association_mismatch");
    }
}
auto materialize(const auto& s, bool agent) -> Manifest {
    Manifest m;
    const auto& root = s.nodes[0];
    auto get = [&](const Node& n, std::string_view k) { return s.copy(s.child(n, k)); };
    m.schema = get(root, "schema");
    m.framework_version = get(root, "framework_version");
    m.policy_version = get(root, "policy_version");
    m.report_schema = get(root, "report_schema");
    m.implementation_id = get(root, "implementation_id");
    m.issue = get(root, "issue");
    m.source_commit = get(root, "source_commit");
    m.title = get(root, "title");
    if (!agent) {
        const auto& a = s.child(root, "approval");
        m.approval.actor = get(a, "actor");
        m.approval.recorded_at_utc = get(a, "recorded_at_utc");
        m.approval.disposition = get(a, "disposition");
        m.approval.statement = get(a, "statement");
        for (auto i = s.child(a, "semantic_items").first; i != absent; i = s.nodes[i].next)
            m.approval.semantic_items.push_back(
                {get(s.nodes[i], "id"), get(s.nodes[i], "disposition")});
    }
    auto i = s.child(root, "artifacts").first;
    for (auto& artifact : m.artifacts) {
        const auto& n = s.nodes[i];
        artifact.automated_disposition = get(n, "automated_disposition");
        artifact.compiler_family = get(n, "compiler_family");
        artifact.function_sha256 = get(n, "function_sha256");
        artifact.generated_human_review_status = get(n, "generated_human_review_status");
        artifact.object_sha256 = get(n, "object_sha256");
        artifact.qualification_preset = get(n, "qualification_preset");
        auto reference = [&](std::string_view k) {
            const auto& ref = s.child(n, k);
            return ReportReference{get(ref, "path"), get(ref, "sha256")};
        };
        artifact.human_report = reference("human_report");
        artifact.machine_report = reference("machine_report");
        i = n.next;
    }
    m.route = agent ? ReviewRoute::independent_agents : ReviewRoute::human_approval;
    return m;
}
void validate_agents(auto& s, kernel_report::Family family, const kernel_report::Owner& gcc,
                     const kernel_report::Owner& clang) {
    const auto& q = s.child(s.nodes[0], "qualification");
    s.object(q, {"route", "disposition", "implementer_agent_id", "author_agent_ids", "input_sha256",
                 "reviewers"});
    s.eq(s.child(q, "route"), "two-independent-agents/v1");
    s.eq(s.child(q, "disposition"), "qualified");
    s.nonempty(s.child(q, "implementer_agent_id"));
    s.hex(s.child(q, "input_sha256"), 64);
    const auto order = [](const Node& left, const Node& right) {
        Decoder a{left.raw}, b{right.raw};
        while (a.pos != a.input.size() && b.pos != b.input.size()) {
            const auto x = a.scalar(), y = b.scalar();
            if (x != y)
                return x < y ? -1 : 1;
        }
        return a.pos == a.input.size() ? (b.pos == b.input.size() ? 0 : -1) : 1;
    };
    const auto same = [&](const Node& left, const Node& right) {
        return left.kind == Kind::string && right.kind == Kind::string && order(left, right) == 0;
    };
    const auto& authors = s.child(q, "author_agent_ids");
    need(authors.kind == Kind::array);
    std::size_t author_count{};
    for (auto i = authors.first; i != absent; i = s.nodes[i].next) {
        s.nonempty(s.nodes[i]);
        need(++author_count <= 8);
        for (auto j = authors.first; j != i; j = s.nodes[j].next)
            need(!same(s.nodes[i], s.nodes[j]));
    }
    need(author_count != 0);
    add(s.usage.retained_bytes, author_count * sizeof(std::string), s.limits.retained_bytes);
    const auto& reviewers = s.child(q, "reviewers");
    need(reviewers.kind == Kind::array);
    auto reviewer = reviewers.first;
    const Node* previous{};
    const std::array reports{&gcc, &clang};
    const auto reviews =
        family == kernel_report::Family::bandwidth
            ? std::span<const kernel_report::detail::ReviewLiteral>{kernel_report::detail::
                                                                        bandwidth_reviews}
            : std::span<const kernel_report::detail::ReviewLiteral>{
                  kernel_report::detail::latency_reviews};
    for (std::size_t index = 0; index < 2; ++index) {
        need(reviewer != absent);
        const auto& r = s.nodes[reviewer];
        s.object(r, {"agent_id", "run_id", "model", "started_at_utc", "completed_at_utc",
                     "input_sha256", "assessments"});
        for (auto key : {"agent_id", "run_id", "model"})
            s.nonempty(s.child(r, key));
        need(!same(s.child(r, "agent_id"), s.child(q, "implementer_agent_id")));
        for (auto i = authors.first; i != absent; i = s.nodes[i].next)
            need(!same(s.child(r, "agent_id"), s.nodes[i]));
        if (previous) {
            need(!same(s.child(r, "agent_id"), s.child(*previous, "agent_id")));
            need(!same(s.child(r, "run_id"), s.child(*previous, "run_id")));
        }
        calendar(s.child(r, "started_at_utc"));
        calendar(s.child(r, "completed_at_utc"));
        need(order(s.child(r, "started_at_utc"), s.child(r, "completed_at_utc")) <= 0);
        need(same(s.child(r, "input_sha256"), s.child(q, "input_sha256")), "association_mismatch");
        const auto& assessments = s.child(r, "assessments");
        need(assessments.kind == Kind::array);
        auto assessment = assessments.first;
        auto artifact = s.child(s.nodes[0], "artifacts").first;
        for (std::size_t slot = 0; slot < 2; ++slot) {
            need(assessment != absent && artifact != absent);
            const auto& a = s.nodes[assessment];
            s.object(a, {"compiler_family", "qualification_preset", "compiler_version",
                         "compiler_target", "object_sha256", "function_sha256",
                         "machine_report_sha256", "human_report_sha256", "semantic_items"});
            const auto& art = s.nodes[artifact];
            for (auto key :
                 {"compiler_family", "qualification_preset", "object_sha256", "function_sha256"})
                need(same(s.child(a, key), s.child(art, key)), "association_mismatch");
            for (auto key : {"machine", "human"}) {
                const bool machine = std::string_view{key} == "machine";
                need(same(s.child(a, machine ? "machine_report_sha256" : "human_report_sha256"),
                          s.child(s.child(art, machine ? "machine_report" : "human_report"),
                                  "sha256")),
                     "association_mismatch");
            }
            const auto* compiler = reports[slot]->report()->content.member("compiler");
            need(s.equal(s.child(a, "compiler_version"), compiler->member("version")->text()) &&
                     s.equal(s.child(a, "compiler_target"), compiler->member("target")->text()),
                 "association_mismatch");
            const auto& items = s.child(a, "semantic_items");
            need(items.kind == Kind::array);
            auto item = items.first;
            for (auto review : reviews) {
                need(item != absent);
                const auto& n = s.nodes[item];
                s.object(
                    n, {"id", "conclusion", "instruction_locations", "explanation", "uncertainty"});
                s.eq(s.child(n, "id"), review.id);
                s.eq(s.child(n, "conclusion"), "supported");
                s.nonempty(s.child(n, "instruction_locations"));
                s.nonempty(s.child(n, "explanation"));
                s.eq(s.child(n, "uncertainty"), "");
                item = n.next;
            }
            need(item == absent);
            add(s.usage.retained_bytes, reviews.size() * sizeof(AgentSemanticItem),
                s.limits.retained_bytes);
            assessment = a.next;
            artifact = art.next;
        }
        need(assessment == absent && artifact == absent);
        previous = &r;
        reviewer = r.next;
    }
    need(reviewer == absent);
}
auto materialize_agents(const auto& s) -> AgentQualification {
    AgentQualification q;
    const auto& root = s.child(s.nodes[0], "qualification");
    const auto get = [&](const Node& n, std::string_view key) { return s.copy(s.child(n, key)); };
    q.route = get(root, "route");
    q.disposition = get(root, "disposition");
    q.implementer_agent_id = get(root, "implementer_agent_id");
    q.input_sha256 = get(root, "input_sha256");
    std::size_t authors{};
    for (auto i = s.child(root, "author_agent_ids").first; i != absent; i = s.nodes[i].next)
        ++authors;
    q.author_agent_ids.reserve(authors);
    for (auto i = s.child(root, "author_agent_ids").first; i != absent; i = s.nodes[i].next)
        q.author_agent_ids.push_back(s.copy(s.nodes[i]));
    auto reviewer = s.child(root, "reviewers").first;
    for (auto& r : q.reviewers) {
        const auto& n = s.nodes[reviewer];
        r.agent_id = get(n, "agent_id");
        r.run_id = get(n, "run_id");
        r.model = get(n, "model");
        r.started_at_utc = get(n, "started_at_utc");
        r.completed_at_utc = get(n, "completed_at_utc");
        r.input_sha256 = get(n, "input_sha256");
        auto assessment = s.child(n, "assessments").first;
        for (auto& a : r.assessments) {
            const auto& v = s.nodes[assessment];
            a.compiler_family = get(v, "compiler_family");
            a.qualification_preset = get(v, "qualification_preset");
            a.compiler_version = get(v, "compiler_version");
            a.compiler_target = get(v, "compiler_target");
            a.object_sha256 = get(v, "object_sha256");
            a.function_sha256 = get(v, "function_sha256");
            a.machine_report_sha256 = get(v, "machine_report_sha256");
            a.human_report_sha256 = get(v, "human_report_sha256");
            std::size_t item_count{};
            for (auto item = s.child(v, "semantic_items").first; item != absent;
                 item = s.nodes[item].next)
                ++item_count;
            a.semantic_items.reserve(item_count);
            for (auto item = s.child(v, "semantic_items").first; item != absent;
                 item = s.nodes[item].next) {
                const auto& i = s.nodes[item];
                a.semantic_items.push_back({get(i, "id"), get(i, "conclusion"),
                                            get(i, "instruction_locations"), get(i, "explanation"),
                                            get(i, "uncertainty")});
            }
            assessment = v.next;
        }
        reviewer = n.next;
    }
    return q;
}
} // namespace
struct Owner::Storage {
    Manifest manifest;
    std::string bytes;
    std::array<std::string, 2> human;
    std::array<std::optional<kernel_report::Owner>, 2> reports;
    core::Sha256Digest digest{core::sha256(std::string_view{})};
    Usage usage;
};
Owner::Owner(std::unique_ptr<Storage> s) noexcept : storage_(std::move(s)) {}
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
auto Owner::manifest() const noexcept -> const Manifest* {
    return storage_ ? &storage_->manifest : nullptr;
}
auto Owner::bytes() const noexcept -> std::string_view {
    return storage_ ? std::string_view{storage_->bytes} : std::string_view{};
}
auto Owner::digest() const noexcept -> const core::Sha256Digest* {
    return storage_ ? &storage_->digest : nullptr;
}
auto Owner::report(std::size_t slot) const noexcept -> const kernel_report::Owner* {
    return storage_ && slot < 2 && storage_->reports[slot] ? &*storage_->reports[slot] : nullptr;
}
auto Owner::human_bytes(std::size_t slot) const noexcept -> std::string_view {
    return storage_ && slot < 2 ? std::string_view{storage_->human[slot]} : std::string_view{};
}
auto Owner::usage() const noexcept -> Usage { return storage_ ? storage_->usage : Usage{}; }
auto Owner::retain_impl(std::string_view bytes, kernel_report::Owner&& gcc,
                        kernel_report::Owner&& clang, std::array<std::string_view, 2> human,
                        Limits limits, bool agent) -> std::expected<Owner, core::Error> {
    try {
        need(limits.depth <= 64, "invalid_limits");
        for (auto n : {limits.input_bytes, limits.string_bytes, limits.integer_digits, limits.nodes,
                       limits.entries, limits.retained_bytes, limits.scanner_bytes})
            need(n <= std::string{}.max_size(), "invalid_limits");
        need(&gcc != &clang, "aliased_reports");
        need(gcc.report().has_value() && clang.report().has_value(), "missing_report");
        auto prepare = [&]<class Scan>() -> std::expected<Owner, core::Error> {
            Usage usage{};
            usage.scanner_bytes = sizeof(Scan);
            need(usage.scanner_bytes <= limits.scanner_bytes, "capacity");
            add(usage.retained_bytes, sizeof(Owner::Storage), limits.retained_bytes);
            for (const auto* report : {&gcc, &clang}) {
                auto u = report->usage();
                add(usage.input_bytes, u.input_bytes, limits.input_bytes);
                add(usage.string_bytes, u.string_bytes, limits.string_bytes);
                add(usage.integer_digits, u.integer_digits, limits.integer_digits);
                add(usage.nodes, u.nodes, limits.nodes);
                add(usage.entries, u.entries, limits.entries);
                add(usage.retained_bytes, u.retained_bytes, limits.retained_bytes);
                usage.depth = std::max(usage.depth, u.depth);
            }
            need(usage.depth <= limits.depth, "capacity");
            for (auto raw : {bytes, human[0], human[1]}) {
                add(usage.input_bytes, raw.size(), limits.input_bytes);
                add(usage.retained_bytes, raw.size(), limits.retained_bytes);
            }
            Scan scanner{bytes, limits, usage};
            scanner.run();
            auto family = validate(scanner, agent);
            auto item_count = family == kernel_report::Family::bandwidth ? 17U : 12U;
            if (!agent)
                add(scanner.usage.retained_bytes, item_count * sizeof(SemanticItem),
                    limits.retained_bytes);
            auto artifact = scanner.child(scanner.nodes[0], "artifacts").first;
            associate(scanner, family, gcc, human[0], 0, scanner.nodes[artifact], agent);
            artifact = scanner.nodes[artifact].next;
            associate(scanner, family, clang, human[1], 1, scanner.nodes[artifact], agent);
            if (agent)
                validate_agents(scanner, family, gcc, clang);
            auto storage = std::make_unique<Owner::Storage>();
            storage->manifest = materialize(scanner, agent);
            if (agent)
                storage->manifest.agent_qualification = materialize_agents(scanner);
            storage->bytes = bytes;
            storage->human[0] = human[0];
            storage->human[1] = human[1];
            storage->digest = core::sha256(bytes);
            storage->usage = scanner.usage;
            // No throwing operations after the first transfer.
            storage->reports[0].emplace(std::move(gcc));
            storage->reports[1].emplace(std::move(clang));
            return Owner{std::move(storage)};
        };
        return agent ? prepare.template operator()<AgentScanner>()
                     : prepare.template operator()<Scanner>();
    } catch (const Rejected& r) {
        return std::unexpected(error(r.code));
    }
}
auto retain(std::string_view bytes, kernel_report::Owner&& gcc, kernel_report::Owner&& clang,
            std::array<std::string_view, 2> human, Limits limits)
    -> std::expected<Owner, core::Error> {
    return Owner::retain_impl(bytes, std::move(gcc), std::move(clang), human, limits, false);
}
auto retain_agent(std::string_view bytes, kernel_report::Owner&& gcc, kernel_report::Owner&& clang,
                  std::array<std::string_view, 2> readable, Limits limits)
    -> std::expected<Owner, core::Error> {
    return Owner::retain_impl(bytes, std::move(gcc), std::move(clang), readable, limits, true);
}
} // namespace epycbench::results::kernel_approval
