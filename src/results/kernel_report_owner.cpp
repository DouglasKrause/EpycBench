#include "results/kernel_report_owner.hpp"
#include "results/kernel_report_review_literals.hpp"
#include <algorithm>
#include <array>
#include <charconv>
#include <limits>
#include <set>

namespace epycbench::results::kernel_report {
namespace {
struct Rejected {
    std::string_view code;
};
void need(bool value, std::string_view code = "invalid_report") {
    if (!value)
        throw Rejected{code};
}
void add(std::size_t& value, std::size_t amount, std::size_t limit) {
    need(value <= limit && amount <= limit - value, "capacity");
    value += amount;
}
auto error(std::string_view code) -> core::Error {
    return {.category = core::ErrorCategory::results,
            .code = core::StableErrorCode{"results.kernel_report." + std::string{code}},
            .message = "Kernel report admission rejected",
            .operation = "retain_kernel_report",
            .os_error = std::nullopt,
            .context = nlohmann::json::object()};
}
} // namespace
// Two passes: a nonallocating syntax/resource preflight, then an owning tree.
// Ordinary string/vector destruction never allocates, including partial construction.
struct Parser {
    std::string_view input;
    Limits limits;
    Usage usage{};
    std::size_t pos{};
    bool own{};
    void space() {
        while (pos < input.size() && (input[pos] == ' ' || input[pos] == '\n' ||
                                      input[pos] == '\r' || input[pos] == '\t'))
            ++pos;
    }
    auto take() -> char {
        need(pos < input.size(), "malformed_json");
        return input[pos++];
    }
    auto hex4() -> std::uint32_t {
        std::uint32_t n = 0;
        for (int i = 0; i < 4; ++i) {
            char c = take();
            unsigned d = c >= '0' && c <= '9'   ? static_cast<unsigned>(c - '0')
                         : c >= 'a' && c <= 'f' ? static_cast<unsigned>(c - 'a' + 10)
                         : c >= 'A' && c <= 'F' ? static_cast<unsigned>(c - 'A' + 10)
                                                : 16U;
            need(d < 16, "malformed_json");
            n = n * 16 + d;
        }
        return n;
    }
    auto string() -> std::string {
        need(take() == '"', "malformed_json");
        std::string out;
        auto emit = [&](char c) {
            add(usage.string_bytes, 1, limits.string_bytes);
            add(usage.retained_bytes, 1, limits.retained_bytes);
            if (own)
                out.push_back(c);
        };
        while (true) {
            const auto c = static_cast<unsigned char>(take());
            if (c == '"')
                break;
            need(c >= 0x20, "malformed_json");
            if (c == '\\') {
                char e = take();
                if (e == '"' || e == '\\' || e == '/')
                    emit(e);
                else if (e == 'b')
                    emit('\b');
                else if (e == 'f')
                    emit('\f');
                else if (e == 'n')
                    emit('\n');
                else if (e == 'r')
                    emit('\r');
                else if (e == 't')
                    emit('\t');
                else {
                    need(e == 'u', "malformed_json");
                    auto n = hex4();
                    if (n >= 0xd800 && n <= 0xdbff) {
                        need(take() == '\\' && take() == 'u', "malformed_json");
                        auto low = hex4();
                        need(low >= 0xdc00 && low <= 0xdfff, "malformed_json");
                        n = 0x10000 + (n - 0xd800) * 1024 + (low - 0xdc00);
                    } else
                        need(n < 0xdc00 || n > 0xdfff, "malformed_json");
                    if (n < 0x80)
                        emit(static_cast<char>(n));
                    else if (n < 0x800) {
                        emit(static_cast<char>(0xc0 | (n >> 6)));
                        emit(static_cast<char>(0x80 | (n & 63)));
                    } else if (n < 0x10000) {
                        emit(static_cast<char>(0xe0 | (n >> 12)));
                        emit(static_cast<char>(0x80 | ((n >> 6) & 63)));
                        emit(static_cast<char>(0x80 | (n & 63)));
                    } else {
                        emit(static_cast<char>(0xf0 | (n >> 18)));
                        emit(static_cast<char>(0x80 | ((n >> 12) & 63)));
                        emit(static_cast<char>(0x80 | ((n >> 6) & 63)));
                        emit(static_cast<char>(0x80 | (n & 63)));
                    }
                }
            } else if (c < 0x80)
                emit(static_cast<char>(c));
            else {
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
                emit(static_cast<char>(c));
                for (unsigned i = 0; i < count; ++i) {
                    auto b = static_cast<unsigned char>(take());
                    need((b & 0xc0U) == 0x80, "malformed_json");
                    n = (n << 6) | (b & 63U);
                    emit(static_cast<char>(b));
                }
                need(n >= minimum && n <= 0x10ffff && (n < 0xd800 || n > 0xdfff), "malformed_json");
            }
        }
        return out;
    }
    auto value(std::size_t depth) -> Value {
        need(depth <= limits.depth, "capacity");
        usage.depth = std::max(usage.depth, depth);
        add(usage.nodes, 1, limits.nodes);
        add(usage.retained_bytes, sizeof(Value), limits.retained_bytes);
        space();
        need(pos < input.size(), "malformed_json");
        Value result;
        char c = input[pos];
        if (c == '{' || c == '[') {
            result.kind_ = c == '{' ? Value::Kind::object : Value::Kind::array;
            ++pos;
            space();
            char end = c == '{' ? '}' : ']';
            if (pos < input.size() && input[pos] == end) {
                ++pos;
                return result;
            }
            std::set<std::string, std::less<>> names;
            while (true) {
                add(usage.entries, 1, limits.entries);
                std::string name;
                if (c == '{') {
                    space();
                    name = string();
                    space();
                    need(take() == ':', "malformed_json");
                    if (own)
                        need(names.insert(name).second, "duplicate_member");
                }
                auto child = value(depth + 1);
                if (own) {
                    child.name_ = std::move(name);
                    result.children_.push_back(std::move(child));
                }
                space();
                char next = take();
                if (next == end)
                    break;
                need(next == ',', "malformed_json");
            }
        } else if (c == '"') {
            result.kind_ = Value::Kind::string;
            result.text_ = string();
        } else if (c == '-' || (c >= '0' && c <= '9')) {
            result.kind_ = Value::Kind::integer;
            auto start = pos;
            if (c == '-')
                ++pos;
            need(pos < input.size() && input[pos] >= '0' && input[pos] <= '9', "malformed_json");
            if (input[pos] == '0')
                ++pos;
            else
                while (pos < input.size() && input[pos] >= '0' && input[pos] <= '9')
                    ++pos;
            need(pos == input.size() ||
                     (input[pos] != '.' && input[pos] != 'e' && input[pos] != 'E'),
                 "integer_required");
            auto token = input.substr(start, pos - start);
            add(usage.integer_digits, token.size() - (c == '-' ? 1U : 0U), limits.integer_digits);
            add(usage.retained_bytes, token.size(), limits.retained_bytes);
            if (own)
                result.text_ = token == "-0" ? "0" : token;
        } else {
            auto literal = c == 't'   ? std::string_view{"true"}
                           : c == 'f' ? std::string_view{"false"}
                                      : std::string_view{"null"};
            need(input.substr(pos, literal.size()) == literal, "malformed_json");
            pos += literal.size();
            result.kind_ = c == 'n' ? Value::Kind::null : Value::Kind::boolean;
            if (c != 'n') {
                add(usage.retained_bytes, literal.size(), limits.retained_bytes);
                if (own)
                    result.text_ = literal;
            }
        }
        return result;
    }
    auto run() -> Value {
        usage.input_bytes = input.size();
        need(input.size() <= limits.input_bytes, "capacity");
        usage.retained_bytes = input.size();
        need(usage.retained_bytes <= limits.retained_bytes, "capacity");
        auto root = value(0);
        space();
        need(pos == input.size(), "malformed_json");
        return root;
    }
};
auto Value::member(std::string_view key) const noexcept -> const Value* {
    for (const auto& child : children_)
        if (child.name_ == key)
            return &child;
    return nullptr;
}
namespace {
using V = Value;
using K = Value::Kind;
auto get(const V& v, std::string_view key) -> const V& {
    auto p = v.member(key);
    need(p != nullptr);
    return *p;
}
void object(const V& v, std::initializer_list<std::string_view> keys) {
    need(v.kind() == K::object && v.children().size() == keys.size());
    for (auto key : keys)
        need(v.member(key) != nullptr);
}
void kind(const V& v, K k) { need(v.kind() == k); }
void str(const V& v) { kind(v, K::string); }
void integer(const V& v) { kind(v, K::integer); }
auto u(const V& v) -> std::uint64_t {
    integer(v);
    std::uint64_t n{};
    auto t = v.text();
    auto [end, ec] = std::from_chars(t.data(), t.data() + t.size(), n);
    need(ec == std::errc{} && end == t.data() + t.size());
    return n;
}
auto boolean(const V& v) -> bool {
    kind(v, K::boolean);
    return v.text() == "true";
}
void eq(const V& v, std::string_view s) {
    str(v);
    need(v.text() == s);
}
void enumeration(const V& v, std::initializer_list<std::string_view> names) {
    str(v);
    need(std::find(names.begin(), names.end(), v.text()) != names.end());
}
template <class F> void array(const V& v, F f) {
    kind(v, K::array);
    for (const auto& child : v.children())
        f(child);
}
template <class F> void nullable(const V& v, F f) {
    if (v.kind() != K::null)
        f(v);
}
void strings(const V& v) { array(v, str); }
void uints(const V& v) {
    array(v, [](const V& n) { (void)u(n); });
}
void integers(const V& v) { array(v, integer); }
void optional_strings(const V& v) {
    array(v, [](const V& n) { nullable(n, str); });
}
void hex(const V& v, std::size_t length) {
    str(v);
    need(v.text().size() == length);
    for (char c : v.text())
        need((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
}
auto equal(const V& a, const V& b) -> bool {
    if (a.kind() != b.kind() || a.text() != b.text() || a.children().size() != b.children().size())
        return false;
    if (a.kind() == K::object) {
        for (const auto& x : a.children()) {
            auto p = b.member(x.name());
            if (!p || !equal(x, *p))
                return false;
        }
    } else
        for (std::size_t i = 0; i < a.children().size(); ++i)
            if (!equal(a.children()[i], b.children()[i]))
                return false;
    return true;
}
void memory(const V& v) {
    object(v,
           {"text", "base", "index", "scale", "displacement", "displacement_value",
            "displacement_resolved", "width_bits", "width_bytes", "stack_derived", "access_role"});
    str(get(v, "text"));
    for (auto k : {"base", "index", "displacement"})
        nullable(get(v, k), str);
    nullable(get(v, "scale"), [](const V& n) {
        auto x = u(n);
        need(x == 1 || x == 2 || x == 4 || x == 8);
    });
    nullable(get(v, "displacement_value"), integer);
    if (!boolean(get(v, "displacement_resolved")))
        need(get(v, "displacement_value").kind() == K::null);
    auto& bits = get(v, "width_bits");
    auto& bytes = get(v, "width_bytes");
    if (bits.kind() == K::null)
        need(bytes.kind() == K::null);
    else {
        auto n = u(bits);
        need(n == 8 || n == 16 || n == 32 || n == 64 || n == 128 || n == 256 || n == 512);
        need(u(bytes) == n / 8);
    }
    (void)boolean(get(v, "stack_derived"));
    enumeration(get(v, "access_role"), {"read", "write", "read_write", "unknown"});
}
} // namespace
namespace {
constexpr std::array<std::string_view, 14> inventory_keys{"calls",
                                                          "returns",
                                                          "prefetch",
                                                          "vector_candidates",
                                                          "memory",
                                                          "fences",
                                                          "clocks",
                                                          "syscalls",
                                                          "atomics",
                                                          "conditional_branches",
                                                          "unconditional_branches",
                                                          "back_edges",
                                                          "external_tail_transfers",
                                                          "copy_helper_relocations"};
constexpr std::array<std::string_view, 8> generic_keys{"calls",
                                                       "prefetch",
                                                       "fences",
                                                       "clocks",
                                                       "syscalls",
                                                       "atomics",
                                                       "external_tail_transfers",
                                                       "copy_helper_relocations"};
constexpr std::array<std::string_view, 8> generic_ids{"generic.no_calls",
                                                      "generic.no_prefetch",
                                                      "generic.no_fences",
                                                      "generic.no_clocks",
                                                      "generic.no_syscalls",
                                                      "generic.no_atomics",
                                                      "generic.no_external_tail_transfers",
                                                      "generic.no_copy_helpers"};
struct Validator {
    const V& root;
    Family family{};
    bool failed{};
    auto instructions() const -> std::span<const V> { return get(root, "instructions").children(); }
    auto instruction(std::uint64_t address) const -> const V& {
        auto list = instructions();
        auto it =
            std::lower_bound(list.begin(), list.end(), address,
                             [](const V& v, std::uint64_t n) { return u(get(v, "address")) < n; });
        need(it != list.end() && u(get(*it, "address")) == address);
        return *it;
    }
    void address(const V& v) const { (void)instruction(u(v)); }
    void addresses(const V& v) const {
        array(v, [&](const V& n) { address(n); });
    }
    void loop(std::uint64_t start, std::uint64_t stop) const {
        const auto& i = instruction(stop);
        need(boolean(get(i, "back_edge")) && get(i, "branch_target").kind() != K::null &&
             u(get(i, "branch_target")) == start);
    }
    void range(const V& v) const {
        object(v, {"start", "stop"});
        loop(u(get(v, "start")), u(get(v, "stop")));
    }
    void pair(const V& v) const {
        kind(v, K::array);
        need(v.children().size() == 2);
        loop(u(v.children()[0]), u(v.children()[1]));
    }
    void load(const V& v) const {
        if (family == Family::bandwidth)
            object(v, {"address", "mnemonic", "destination", "operands", "memory", "definitions",
                       "uses"});
        else
            object(v, {"address", "mnemonic", "destination", "memory", "definitions", "uses"});
        const auto& i = instruction(u(get(v, "address")));
        str(get(v, "mnemonic"));
        nullable(get(v, "destination"), str);
        array(get(v, "memory"), memory);
        strings(get(v, "definitions"));
        strings(get(v, "uses"));
        need(equal(get(v, "mnemonic"), get(i, "mnemonic")) &&
             equal(get(v, "memory"), get(i, "memory_operands")) &&
             equal(get(v, "definitions"), get(i, "register_def_candidates")) &&
             equal(get(v, "uses"), get(i, "register_use_candidates")));
        const auto defs = get(i, "register_def_candidates").children();
        if (defs.empty())
            need(get(v, "destination").kind() == K::null);
        else
            need(equal(get(v, "destination"), defs.front()));
        if (family == Family::bandwidth)
            need(equal(get(v, "operands"), get(i, "operands")));
    }
    void loads(const V& v) const {
        array(v, [&](const V& n) { load(n); });
    }
    void logical(const V& v) const {
        object(v, {"resolved", "offsets", "loads", "updates", "final_deltas", "cursor_registers"});
        (void)boolean(get(v, "resolved"));
        integers(get(v, "offsets"));
        strings(get(v, "cursor_registers"));
        array(get(v, "loads"), [&](const V& n) {
            object(n, {"address", "base", "cursor_delta", "displacement", "logical_offset"});
            address(get(n, "address"));
            nullable(get(n, "base"), str);
            for (auto k : {"cursor_delta", "displacement", "logical_offset"})
                nullable(get(n, k), integer);
        });
        array(get(v, "updates"), [&](const V& n) {
            const auto& op = get(n, "operation");
            enumeration(op, {"add", "sub", "copy", "lea"});
            if (op.text() == "add" || op.text() == "sub") {
                object(n, {"address", "operation", "destination", "before", "after"});
                integer(get(n, "before"));
            } else {
                object(n, {"address", "operation", "source", "destination", "after"});
                str(get(n, "source"));
            }
            address(get(n, "address"));
            str(get(n, "destination"));
            integer(get(n, "after"));
        });
        kind(get(v, "final_deltas"), K::object);
        for (const auto& n : get(v, "final_deltas").children())
            integer(n);
    }
    void restart(const V& v) const {
        object(v, {"model", "outer_back_edge", "outer_target", "first_load", "pointer_reloads",
                   "index_restart", "accumulator_resets"});
        enumeration(get(v, "model"), {"accumulator_reset", "pointer_reload", "scaled_index_reset",
                                      "pointer_restart_missing", "unresolved"});
        loop(u(get(v, "outer_target")), u(get(v, "outer_back_edge")));
        (void)u(get(v, "first_load")); // Conditional instruction/target domain is checked with
                                       // review candidates.
        array(get(v, "pointer_reloads"), [&](const V& n) {
            object(n, {"address", "cursor", "stable_base"});
            address(get(n, "address"));
            str(get(n, "cursor"));
            str(get(n, "stable_base"));
        });
        addresses(get(v, "accumulator_resets"));
        nullable(get(v, "index_restart"), [&](const V& n) {
            object(n,
                   {"model", "stable_base", "index", "scale", "zeroing", "header_definitions",
                    "base_changes", "stride_words", "stride_bytes", "first_load", "late_zeroing"});
            eq(get(n, "model"), "scaled_index_reset");
            nullable(get(n, "stable_base"), str);
            nullable(get(n, "index"), str);
            need(u(get(n, "scale")) == 8);
            for (auto k : {"zeroing", "header_definitions", "base_changes", "late_zeroing"})
                addresses(get(n, k));
            for (auto k : {"stride_words", "stride_bytes"})
                nullable(get(n, k), integer);
            address(get(n, "first_load"));
            need(equal(get(n, "first_load"), get(v, "first_load")));
        });
    }
    void common() {
        object(root, {"schema",
                      "framework_version",
                      "family_policy_version",
                      "source",
                      "implementation_id",
                      "qualification",
                      "compiler",
                      "compile_command",
                      "effective_flags",
                      "object",
                      "symbol",
                      "function",
                      "tools",
                      "instructions",
                      "relocations",
                      "inventory",
                      "generic_findings",
                      "family_findings",
                      "automated_disposition",
                      "human_review",
                      "qualification_status"});
        eq(get(root, "schema"), "epycbench.kernel-object-audit-report/v1");
        eq(get(root, "framework_version"), "epycbench.kernel-object-audit/v1");
        enumeration(get(root, "family_policy_version"),
                    {"epycbench.kernel-object-audit.latency/v1",
                     "epycbench.kernel-object-audit.bandwidth/v1"});
        family = get(root, "family_policy_version").text().ends_with("latency/v1")
                     ? Family::latency
                     : Family::bandwidth;
        const auto& src = get(root, "source");
        object(src, {"commit", "dirty"});
        hex(get(src, "commit"), 40);
        (void)boolean(get(src, "dirty"));
        str(get(root, "implementation_id"));
        const auto& q = get(root, "qualification");
        object(q, {"preset", "configuration", "policy_identifier"});
        str(get(q, "preset"));
        eq(get(q, "configuration"), "Release");
        need(equal(get(q, "policy_identifier"), get(root, "family_policy_version")));
        const auto& c = get(root, "compiler");
        object(c, {"family", "version", "target", "identity"});
        enumeration(get(c, "family"), {"GCC", "Clang"});
        for (auto k : {"version", "target", "identity"})
            str(get(c, k));
        strings(get(root, "compile_command"));
        const auto& f = get(root, "effective_flags");
        object(f,
               {"optimization", "lto", "loop_vectorization", "slp_vectorization", "unrolling",
                "native_arch", "native_tune", "sanitizers", "coverage", "profiling", "ambiguous"});
        nullable(get(f, "optimization"), str);
        for (auto k : {"lto", "loop_vectorization", "slp_vectorization", "unrolling"})
            nullable(get(f, k), [](const V& n) { (void)boolean(n); });
        for (auto k : {"native_arch", "native_tune", "sanitizers", "coverage", "profiling"})
            (void)boolean(get(f, k));
        strings(get(f, "ambiguous"));
        const auto& obj = get(root, "object");
        object(obj, {"identity", "sha256"});
        str(get(obj, "identity"));
        hex(get(obj, "sha256"), 64);
        const auto& sym = get(root, "symbol");
        object(sym, {"mangled", "demangled", "binding", "type", "section", "value", "size"});
        for (auto k : {"mangled", "demangled", "binding", "type", "section"})
            str(get(sym, k));
        auto start = u(get(sym, "value")), size = u(get(sym, "size"));
        need(size > 0 && size <= std::numeric_limits<std::uint64_t>::max() - start);
        const auto& fn = get(root, "function");
        object(fn, {"sha256", "start", "stop"});
        hex(get(fn, "sha256"), 64);
        need(u(get(fn, "start")) == start && u(get(fn, "stop")) == start + size);
        const auto& tools = get(root, "tools");
        object(tools, {"readelf", "objdump", "digest"});
        str(get(tools, "readelf"));
        str(get(tools, "objdump"));
        eq(get(tools, "digest"), "Python hashlib.sha256");
        std::uint64_t next = start;
        array(get(root, "instructions"), [&](const V& n) {
            object(n, {"address", "raw_bytes", "mnemonic", "operands", "memory_operands",
                       "branch_target", "conditional_branch", "back_edge",
                       "register_def_candidates", "register_use_candidates"});
            need(u(get(n, "address")) == next);
            str(get(n, "raw_bytes"));
            auto raw = get(n, "raw_bytes").text();
            need(!raw.empty() && raw.size() % 3 == 2);
            for (std::size_t i = 0; i < raw.size(); ++i) {
                char x = raw[i];
                need(i % 3 == 2 ? x == ' '
                                : ((x >= '0' && x <= '9') || (x >= 'a' && x <= 'f') ||
                                   (x >= 'A' && x <= 'F')));
            }
            auto bytes = (raw.size() + 1) / 3;
            need(bytes <= start + size - next);
            next += bytes;
            str(get(n, "mnemonic"));
            str(get(n, "operands"));
            array(get(n, "memory_operands"), memory);
            nullable(get(n, "branch_target"), [](const V& v) { (void)u(v); });
            (void)boolean(get(n, "conditional_branch"));
            (void)boolean(get(n, "back_edge"));
            strings(get(n, "register_def_candidates"));
            strings(get(n, "register_use_candidates"));
        });
        need(next == start + size);
        array(get(root, "relocations"), [](const V& n) {
            object(n, {"offset", "relocation_type", "target"});
            (void)u(get(n, "offset"));
            str(get(n, "relocation_type"));
            str(get(n, "target"));
        });
        const auto& inv = get(root, "inventory");
        kind(inv, K::object);
        need(inv.children().size() == inventory_keys.size());
        for (auto key : inventory_keys) {
            const auto& a = get(inv, key);
            if (key == "copy_helper_relocations")
                array(a, [&](const V& n) {
                    auto offset = u(n);
                    bool found = false;
                    for (const auto& r : get(root, "relocations").children())
                        found |= u(get(r, "offset")) == offset;
                    need(found);
                });
            else
                addresses(a);
        }
        const auto& generics = get(root, "generic_findings");
        kind(generics, K::array);
        need(generics.children().size() == generic_ids.size());
        for (std::size_t i = 0; i < generic_ids.size(); ++i) {
            const auto& n = generics.children()[i];
            object(n, {"id", "status", "addresses"});
            eq(get(n, "id"), generic_ids[i]);
            need(equal(get(n, "addresses"), get(inv, generic_keys[i])));
            bool fail = !get(n, "addresses").children().empty();
            eq(get(n, "status"), fail ? "fail" : "pass");
            failed |= fail;
        }
    }
    auto findings() const -> std::span<const V> { return get(root, "family_findings").children(); }
    void framework() {
        kind(get(root, "family_findings"), K::array);
        need(findings().size() > 16);
        const auto& compiler = get(root, "compiler");
        const auto& qualification = get(root, "qualification");
        for (std::size_t i = 0; i < 6; ++i) {
            const auto& n = findings()[i];
            object(n, {"id", "status", "expected", "actual"});
            const auto& exp = get(n, "expected");
            const auto& actual = get(n, "actual");
            bool pass = true;
            switch (i) {
            case 0:
                eq(get(n, "id"), "qualification.compiler_family");
                need(equal(exp, get(compiler, "family")) && equal(actual, exp));
                break;
            case 1:
                eq(get(n, "id"), "qualification.target");
                eq(exp, "Linux ELF x86-64 SysV");
                need(equal(actual, get(compiler, "target")));
                break;
            case 2:
                eq(get(n, "id"), "qualification.binding.implementation_id");
                eq(exp, family == Family::latency ? "scalar-u64-random-cycle/v1"
                                                  : "scalar-u64-sequential/v1");
                need(equal(actual, get(root, "implementation_id")));
                pass = equal(exp, actual);
                break;
            case 3:
                eq(get(n, "id"), "qualification.binding.semantic_symbol");
                eq(exp, family == Family::latency
                            ? "epycbench::kernel::execute_dependent_load_latency(epycbench::kernel:"
                              ":PreparedDependentLoadLatency const&, "
                              "epycbench::kernel::DependentLoadLatencySchedule)"
                            : "epycbench::kernel::execute_sequential_read_bandwidth(epycbench::"
                              "kernel::PreparedSequentialReadBandwidth const&, "
                              "epycbench::kernel::SequentialReadBandwidthPassCount)");
                str(actual);
                pass = equal(exp, actual);
                break;
            case 4:
                eq(get(n, "id"), "qualification.binding.compiler_family");
                strings(exp);
                need(exp.children().size() == 2);
                eq(exp.children()[0], "Clang");
                eq(exp.children()[1], "GCC");
                need(equal(actual, get(compiler, "family")));
                break;
            case 5:
                eq(get(n, "id"), "qualification.binding.preset");
                eq(exp, get(compiler, "family").text() == "GCC" ? "release" : "release-clang");
                need(equal(actual, get(qualification, "preset")));
                pass = equal(exp, actual);
                break;
            default:
                need(false);
            }
            eq(get(n, "status"), pass ? "pass" : "fail");
            failed |= !pass;
        }
        constexpr std::array<std::string_view, 10> flags{
            "optimization", "lto",         "loop_vectorization", "slp_vectorization", "unrolling",
            "native_arch",  "native_tune", "sanitizers",         "coverage",          "profiling"};
        const auto& values = get(root, "effective_flags");
        bool ambiguous = !get(values, "ambiguous").children().empty();
        for (std::size_t i = 0; i < flags.size(); ++i) {
            const auto& n = findings()[6 + i];
            object(n, {"id", "status", "expected", "actual", "reason"});
            str(get(n, "id"));
            auto id = get(n, "id").text();
            need(id.starts_with("qualification.flag.") && id.substr(19) == flags[i]);
            const auto& expected = get(n, "expected");
            if (i == 0)
                eq(expected, "-O3");
            else
                need(!boolean(expected));
            const auto& actual = get(n, "actual");
            need(equal(actual, get(values, flags[i])));
            bool pass = !ambiguous && actual.kind() != K::null && equal(actual, expected);
            eq(get(n, "status"), pass ? "pass" : "fail");
            eq(get(n, "reason"), ambiguous                  ? "ambiguous"
                                 : actual.kind() == K::null ? "unresolved"
                                                            : "evaluated");
            failed |= !pass;
        }
    }
    // The cursor grammar freezes producer order/cardinality, without replaying its algorithm.
    std::size_t cursor{16};
    auto peek(std::string_view suffix) const -> bool {
        if (cursor >= findings().size()) {
            return false;
        }
        const auto& id = get(findings()[cursor], "id");
        str(id);
        auto prefix = family == Family::latency ? std::string_view{"latency.v1."}
                                                : std::string_view{"bandwidth.v1."};
        return id.text().starts_with(prefix) && id.text().substr(prefix.size()) == suffix;
    }
    template <class F>
    auto consume(std::string_view suffix, std::initializer_list<std::string_view> statuses,
                 F validate) -> const V& {
        need(peek(suffix));
        const auto& n = findings()[cursor++];
        object(n, {"id", "status", "evidence"});
        enumeration(get(n, "status"), statuses);
        failed |= get(n, "status").text() == "fail";
        validate(get(n, "evidence"));
        return n;
    }
    void latency() {
        auto addr = [&](const V& n) { addresses(n); };
        auto ls = [&](const V& n) { loads(n); };
        if (peek("candidate_loop.missing")) {
            consume("candidate_loop.missing", {"fail"}, [&](const V& n) {
                object(n, {"back_edges"});
                addresses(get(n, "back_edges"));
            });
            return;
        }
        if (peek("candidate_loop.ambiguous")) {
            const auto& n =
                consume("candidate_loop.ambiguous", {"fail", "review"}, [&](const V& e) {
                    array(e, [&](const V& r) {
                        if (r.member("loads")) {
                            object(r, {"start", "stop", "loads"});
                            addresses(get(r, "loads"));
                            loop(u(get(r, "start")), u(get(r, "stop")));
                        } else
                            range(r);
                    });
                });
            for (const auto& r : get(n, "evidence").children())
                need((r.member("loads") != nullptr) == (get(n, "status").text() == "fail"));
            return;
        }
        consume("candidate_loop", {"pass"}, [&](const V& n) {
            object(n, {"start", "stop", "back_edge", "conditional"});
            loop(u(get(n, "start")), u(get(n, "stop")));
            need(equal(get(n, "stop"), get(n, "back_edge")));
            need(equal(get(n, "conditional"),
                       get(instruction(u(get(n, "stop"))), "conditional_branch")));
        });
        consume("vector_gather", {"pass", "fail"}, ls);
        if (peek("successor_load.missing"))
            consume("successor_load.missing", {"fail"}, ls);
        consume("successor_width", {"pass", "fail"}, ls);
        if (peek("successor_load.multiple")) {
            consume("successor_load.multiple", {"fail"}, ls);
            consume("second_chain", {"fail"}, ls);
            consume("multi_step_unroll", {"fail"}, addr);
        } else if (peek("successor_load")) {
            consume("successor_load", {"pass"}, [&](const V& n) { load(n); });
            consume("second_chain", {"pass"}, addr);
            consume("multi_step_unroll", {"pass"}, addr);
        } else
            consume("benchmark_provenance", {"review"}, ls);
        if (peek("unresolved_memory_provenance"))
            consume("unresolved_memory_provenance", {"review"}, ls);
        consume("payload_access", {"pass", "fail"}, [](const V& n) { array(n, memory); });
        if (peek("payload_provenance"))
            consume("payload_provenance", {"review"}, [](const V& n) { array(n, memory); });
        consume("bounds_validation", {"pass", "fail"}, addr);
        consume("loop_count", {"pass", "review"}, [&](const V& n) {
            object(n, {"branch", "mnemonic"});
            need(equal(get(n, "mnemonic"), get(instruction(u(get(n, "branch"))), "mnemonic")));
        });
        consume("benchmark_store", {"pass", "fail"}, addr);
        if (peek("unresolved_store_provenance"))
            consume("unresolved_store_provenance", {"review"}, addr);
        const auto& terminal =
            consume("terminal_return", {"pass", "fail", "review"}, [&](const V& n) {
                if (n.member("return_register")) {
                    object(n, {"successor", "return_register"});
                    nullable(get(n, "successor"), str);
                    eq(get(n, "return_register"), "rax");
                } else {
                    object(n, {"successor", "live_at_return", "load"});
                    str(get(n, "successor"));
                    strings(get(n, "live_at_return"));
                    address(get(n, "load"));
                }
            });
        need((get(terminal, "evidence").member("return_register") != nullptr) ==
             (get(terminal, "status").text() == "review"));
        consume("dependency_chain", {"review"}, [&](const V& n) {
            object(n, {"successor_load", "address"});
            if (get(n, "successor_load").kind() == K::null)
                need(get(n, "address").kind() == K::null);
            else {
                load(get(n, "address"));
                need(equal(get(n, "successor_load"), get(get(n, "address"), "address")));
            }
        });
    }
    void bandwidth() {
        auto addr = [&](const V& n) { addresses(n); };
        auto ls = [&](const V& n) { loads(n); };
        auto log = [&](const V& n) { logical(n); };
        auto counts = [](const V& n) {
            object(n, {"expected", "actual"});
            need(u(get(n, "expected")) == 8);
            (void)u(get(n, "actual"));
        };
        if (peek("traversal.missing")) {
            consume("traversal.missing", {"fail"}, [&](const V& n) {
                object(n, {"back_edges"});
                addresses(get(n, "back_edges"));
            });
            return;
        }
        if (peek("traversal.ambiguous")) {
            consume("traversal.ambiguous", {"fail"},
                    [&](const V& n) { array(n, [&](const V& r) { range(r); }); });
            return;
        }
        consume("traversal", {"pass"}, [&](const V& n) {
            object(n, {"block", "pass"});
            pair(get(n, "block"));
            nullable(get(n, "pass"), [&](const V& v) { pair(v); });
        });
        consume("block_loop", {"pass"}, [&](const V& n) { range(n); });
        const auto& pass = consume("pass_structure", {"pass", "review"},
                                   [&](const V& n) { nullable(n, [&](const V& v) { range(v); }); });
        bool has_pass = get(pass, "evidence").kind() != K::null;
        need((get(pass, "status").text() == "pass") == has_pass);
        consume("vector_gather", {"pass", "fail"}, ls);
        consume("load_width", {"pass", "fail"}, ls);
        std::uint64_t load_count = 8;
        if (peek("benchmark_load.missing")) {
            const auto& n = consume("benchmark_load.missing", {"fail"}, counts);
            load_count = u(get(get(n, "evidence"), "actual"));
            need(load_count < 8);
        } else if (peek("benchmark_load.extra")) {
            const auto& n = consume("benchmark_load.extra", {"fail"}, counts);
            load_count = u(get(get(n, "evidence"), "actual"));
            need(load_count > 8);
        } else {
            const auto& n = consume("benchmark_load.count", {"pass"}, ls);
            need(get(n, "evidence").children().size() == 8);
        }
        if (peek("word_offset.duplicate")) {
            need(load_count == 8);
            consume("word_offset.duplicate", {"pass", "fail"}, log);
            consume("word_offset.set", {"pass", "fail"}, [&](const V& n) {
                object(n, {"expected", "actual", "affine"});
                uints(get(n, "expected"));
                need(get(n, "expected").children().size() == 8);
                for (std::size_t i = 0; i < 8; ++i)
                    need(u(get(n, "expected").children()[i]) == i * 8);
                integers(get(n, "actual"));
                logical(get(n, "affine"));
            });
            consume("word_offset.order", {"pass", "fail"}, log);
        } else
            consume("unresolved_logical_offset", {"review"}, log);
        if (peek("lane_association")) {
            need(load_count == 8);
            const auto& n = consume("lane_association", {"fail", "review"}, optional_strings);
            bool fail = get(n, "status").text() == "fail";
            consume("accumulator_count",
                    fail ? std::initializer_list<std::string_view>{"fail"}
                         : std::initializer_list<std::string_view>{"pass"},
                    [&](const V& e) {
                        if (fail)
                            counts(e);
                        else
                            strings(e);
                    });
            consume("accumulator_collapse",
                    fail ? std::initializer_list<std::string_view>{"fail"}
                         : std::initializer_list<std::string_view>{"pass"},
                    [&](const V& e) {
                        if (fail)
                            optional_strings(e);
                        else
                            strings(e);
                    });
        } else {
            need(load_count != 8);
            consume("unresolved_lane_proof", {"review"}, optional_strings);
        }
        consume("block_stride", {"pass", "fail", "review"}, [&](const V& n) {
            object(n, {"accepted_registers", "affine", "updates"});
            strings(get(n, "accepted_registers"));
            logical(get(n, "affine"));
            addresses(get(n, "updates"));
        });
        const auto& unroll = consume("multi_block_unroll", {"pass", "fail"}, [&](const V& n) {
            object(n, {"loads"});
            need(u(get(n, "loads")) == load_count);
        });
        eq(get(unroll, "status"), load_count > 8 ? "fail" : "pass");
        if (has_pass) {
            consume("pass_order", {"pass"}, [&](const V& n) {
                object(n, {"block_back_edge", "pass_back_edge"});
                address(get(n, "block_back_edge"));
                address(get(n, "pass_back_edge"));
            });
            consume("pass_restart", {"pass", "fail", "review"}, [&](const V& n) { restart(n); });
            consume("accumulator_persistence", {"fail", "review"}, addr);
        } else {
            consume("unresolved_pass_proof", {"review"}, [](const V& n) { kind(n, K::null); });
            consume("pass_restart", {"review"}, [](const V& n) { kind(n, K::null); });
            consume("accumulator_persistence", {"review"}, strings);
        }
        consume("benchmark_store", {"pass", "fail"}, addr);
        if (peek("unresolved_store_provenance"))
            consume("unresolved_store_provenance", {"review"}, addr);
        auto reduction = [](const V& n) {
            object(n, {"lanes", "reaching_rax"});
            strings(get(n, "lanes"));
            strings(get(n, "reaching_rax"));
        };
        const auto& r = consume("terminal_reduction", {"pass", "fail", "review"}, reduction);
        const auto& ret = consume("terminal_return", {"pass", "fail", "review"}, [](const V& n) {
            object(n, {"abi_return", "reaching_rax"});
            eq(get(n, "abi_return"), "rax");
            strings(get(n, "reaching_rax"));
        });
        need(equal(get(r, "status"), get(ret, "status")) &&
             equal(get(get(r, "evidence"), "reaching_rax"),
                   get(get(ret, "evidence"), "reaching_rax")));
        if (get(r, "status").text() == "review")
            consume("unresolved_reduction_proof", {"review"}, reduction);
    }
    auto finding(std::string_view suffix) const -> const V* {
        auto prefix = family == Family::latency ? std::string_view{"latency.v1."}
                                                : std::string_view{"bandwidth.v1."};
        for (const auto& n : findings().subspan(16)) {
            auto id = get(n, "id").text();
            if (id.starts_with(prefix) && id.substr(prefix.size()) == suffix)
                return &get(n, "evidence");
        }
        return nullptr;
    }
    auto finding_record(std::string_view suffix) const -> const V* {
        const auto* evidence = finding(suffix);
        if (!evidence)
            return nullptr;
        for (const auto& record : findings().subspan(16))
            if (&get(record, "evidence") == evidence)
                return &record;
        return nullptr;
    }
    void status(std::string_view suffix, std::string_view expected) const {
        const auto* record = finding_record(suffix);
        need(record != nullptr);
        eq(get(*record, "status"), expected);
    }
    void empty_status(std::string_view suffix) const {
        if (auto evidence = finding(suffix))
            status(suffix, evidence->children().empty() ? "pass" : "fail");
    }
    static auto has_string(const V& values, std::string_view text) -> bool {
        return std::ranges::any_of(values.children(), [&](const V& value) {
            return value.kind() == K::string && value.text() == text;
        });
    }
    static void ordered_strings(const V& values) {
        strings(values);
        auto list = values.children();
        for (std::size_t i = 1; i < list.size(); ++i)
            need(list[i - 1].text() < list[i].text());
    }
    static void load_addresses_equal(const V& loads_view, const V& addresses_view) {
        auto list = loads_view.children(), addresses_list = addresses_view.children();
        need(list.size() == addresses_list.size());
        for (std::size_t i = 0; i < list.size(); ++i)
            need(equal(get(list[i], "address"), addresses_list[i]));
    }
    static auto canonical_gpr(std::string_view text) -> std::string_view {
        constexpr std::array<std::string_view, 17> names{"rax", "rbx", "rcx", "rdx", "rsi", "rdi",
                                                         "rsp", "rbp", "rip", "r8",  "r9",  "r10",
                                                         "r11", "r12", "r13", "r14", "r15"};
        constexpr std::array<std::string_view, 17> aliases{
            "eax", "ebx", "ecx",  "edx",  "esi",  "edi",  "esp",  "ebp", "eip",
            "r8d", "r9d", "r10d", "r11d", "r12d", "r13d", "r14d", "r15d"};
        auto lower_equal = [&](std::string_view candidate) {
            if (text.size() != candidate.size())
                return false;
            for (std::size_t i = 0; i < text.size(); ++i) {
                char c = text[i];
                if (c >= 'A' && c <= 'Z')
                    c = static_cast<char>(c - 'A' + 'a');
                if (c != candidate[i])
                    return false;
            }
            return true;
        };
        for (std::size_t i = 0; i < names.size(); ++i)
            if (lower_equal(names[i]) || lower_equal(aliases[i]))
                return names[i];
        return {};
    }
    static auto times_eight(const V& input, const V& output) -> bool {
        if (input.kind() == K::null)
            return output.kind() == K::null;
        if (output.kind() != K::integer)
            return false;
        auto a = input.text(), b = output.text();
        bool negative = a.starts_with('-');
        if (negative != b.starts_with('-'))
            return false;
        if (negative) {
            a.remove_prefix(1);
            b.remove_prefix(1);
        }
        std::size_t ai = a.size(), bi = b.size();
        unsigned carry = 0;
        while (ai) {
            unsigned value = static_cast<unsigned>(a[--ai] - '0') * 8U + carry;
            if (!bi || b[--bi] != static_cast<char>('0' + value % 10U))
                return false;
            carry = value / 10U;
        }
        if (carry) {
            if (!bi || b[--bi] != static_cast<char>('0' + carry))
                return false;
        }
        return bi == 0;
    }
    void latency_predicates(const V& review) const {
        if (auto missing = finding("candidate_loop.missing")) {
            need(equal(get(*missing, "back_edges"), get(review, "back_edges")));
            need(get(review, "memory_reads").children().empty() &&
                 get(review, "proven_successor_candidates").children().empty());
            return;
        }
        if (auto ambiguous = finding("candidate_loop.ambiguous")) {
            need(ambiguous->children().size() > 1);
            for (const auto& range_view : ambiguous->children())
                if (auto values = range_view.member("loads"))
                    need(!values->children().empty());
            need(get(review, "memory_reads").children().empty() &&
                 get(review, "proven_successor_candidates").children().empty());
            return;
        }
        const auto& proven = get(review, "proven_successor_candidates");
        auto count = proven.children().size();
        const auto* single = finding("successor_load");
        const auto* multiple = finding("successor_load.multiple");
        need((single != nullptr) == (count == 1));
        need((multiple != nullptr) == (count > 1));
        if (single) {
            need(equal(*single, proven.children()[0]));
            for (auto suffix : {"second_chain", "multi_step_unroll"}) {
                const auto* addresses_view = finding(suffix);
                need(addresses_view && addresses_view->children().size() == 1 &&
                     equal(addresses_view->children()[0], get(*single, "address")));
            }
        } else if (multiple) {
            need(equal(*multiple, proven));
            need(equal(*finding("second_chain"), proven));
            load_addresses_equal(proven, *finding("multi_step_unroll"));
        } else
            need(finding("benchmark_provenance") != nullptr);
        const auto& dependency = *finding("dependency_chain");
        if (single)
            need(equal(get(dependency, "address"), *single) &&
                 equal(get(dependency, "successor_load"), get(*single, "address")));
        else
            need(get(dependency, "address").kind() == K::null &&
                 get(dependency, "successor_load").kind() == K::null);
        const auto& terminal = *finding("terminal_return");
        if (single && get(*single, "destination").kind() != K::null) {
            need(terminal.member("live_at_return") != nullptr);
            need(equal(get(terminal, "load"), get(*single, "address")) &&
                 equal(get(terminal, "successor"), get(*single, "destination")));
            ordered_strings(get(terminal, "live_at_return"));
            status("terminal_return",
                   has_string(get(terminal, "live_at_return"), "rax") ? "pass" : "fail");
        } else {
            need(terminal.member("return_register") != nullptr &&
                 get(terminal, "successor").kind() == K::null);
            status("terminal_return", "review");
        }
        const auto& loop_view = *finding("candidate_loop");
        const auto& count_view = *finding("loop_count");
        need(equal(get(count_view, "branch"), get(loop_view, "stop")));
        status("loop_count", boolean(get(loop_view, "conditional")) ? "pass" : "review");
        for (auto suffix : {"successor_width", "payload_access", "bounds_validation"})
            empty_status(suffix);
        for (auto suffix : {"unresolved_memory_provenance", "payload_provenance"})
            if (auto evidence = finding(suffix))
                need(!evidence->children().empty());
    }
    void bandwidth_predicates(const V& review) const {
        if (auto missing = finding("traversal.missing")) {
            need(equal(get(*missing, "back_edges"), get(review, "back_edges")));
            need(get(review, "memory_reads").children().empty() &&
                 get(review, "benchmark_candidates").children().empty());
            return;
        }
        if (auto ambiguous = finding("traversal.ambiguous")) {
            need(ambiguous->children().size() > 1);
            need(get(review, "memory_reads").children().empty() &&
                 get(review, "benchmark_candidates").children().empty());
            return;
        }
        empty_status("load_width");
        const auto& stride = *finding("block_stride");
        const auto& affine = get(stride, "affine");
        const auto& offsets = get(affine, "offsets");
        auto load_count = get(review, "benchmark_candidates").children().size();
        bool resolved = load_count == 8 && boolean(get(affine, "resolved"));
        need((finding("word_offset.duplicate") != nullptr) == resolved);
        if (resolved) {
            need(equal(*finding("word_offset.duplicate"), affine) &&
                 equal(*finding("word_offset.order"), affine));
            const auto& set = *finding("word_offset.set");
            need(equal(get(set, "affine"), affine) && equal(get(set, "actual"), offsets));
            need(offsets.children().size() == 8);
            bool duplicate = false, ascending = true, set_equal = true;
            constexpr std::array<std::string_view, 8> expected{"0",  "8",  "16", "24",
                                                               "32", "40", "48", "56"};
            for (std::size_t i = 0; i < 8; ++i) {
                ascending &= offsets.children()[i].text() == expected[i];
                bool found = false;
                for (std::size_t j = 0; j < 8; ++j) {
                    found |= offsets.children()[j].text() == expected[i];
                    if (j < i)
                        duplicate |= equal(offsets.children()[i], offsets.children()[j]);
                }
                set_equal &= found;
            }
            status("word_offset.duplicate", duplicate ? "fail" : "pass");
            status("word_offset.set", set_equal ? "pass" : "fail");
            status("word_offset.order", ascending ? "pass" : "fail");
        } else
            need(equal(*finding("unresolved_logical_offset"), affine));
        const auto& accepted = get(stride, "accepted_registers");
        strings(accepted);
        std::size_t matched = 0;
        // Producer sorts final_deltas keys before selecting exactly delta==64.
        for (const auto& delta : get(affine, "final_deltas").children())
            if (delta.text() == "64") {
                need(has_string(accepted, delta.name()));
                ++matched;
            }
        need(accepted.children().size() == matched);
        ordered_strings(accepted);
        status("block_stride", matched                                     ? "pass"
                               : get(affine, "updates").children().empty() ? "review"
                                                                           : "fail");
        const auto& reduction = *finding("terminal_reduction");
        const auto& lanes = get(reduction, "lanes");
        ordered_strings(lanes);
        const auto* lane_values =
            finding(load_count == 8 ? "lane_association" : "unresolved_lane_proof");
        need(lane_values && lane_values->children().size() == load_count);
        for (const auto& lane : lane_values->children())
            if (lane.kind() != K::null)
                need(has_string(lanes, lane.text()));
        for (const auto& lane : lanes.children())
            need(has_string(*lane_values, lane.text()));
        if (load_count == 8) {
            bool eight = lanes.children().size() == 8;
            status("lane_association", eight ? "review" : "fail");
            const auto& count = *finding("accumulator_count");
            const auto& collapse = *finding("accumulator_collapse");
            if (eight) {
                need(equal(count, lanes) && equal(collapse, lanes));
            } else {
                need(u(get(count, "actual")) == lanes.children().size() &&
                     equal(collapse, *lane_values));
            }
        }
        const auto& reaching = get(reduction, "reaching_rax");
        ordered_strings(reaching);
        for (const auto& lane : reaching.children())
            need(has_string(lanes, lane.text()));
        auto terminal_status = reaching.children().size() == 8 ? "pass"
                               : reaching.children().empty()   ? "review"
                                                               : "fail";
        status("terminal_reduction", terminal_status);
        status("terminal_return", terminal_status);
        if (auto unresolved = finding("unresolved_reduction_proof"))
            need(equal(*unresolved, reduction));
        const auto& pass_loop = get(review, "pass_loop");
        const auto& persistence = *finding("accumulator_persistence");
        if (pass_loop.kind() == K::null) {
            need(equal(persistence, lanes));
            return;
        }
        const auto& order = *finding("pass_order");
        need(equal(get(order, "block_back_edge"), get(review, "block_loop").children()[1]) &&
             equal(get(order, "pass_back_edge"), pass_loop.children()[1]));
        const auto& restart_view = *finding("pass_restart");
        const auto& resets = get(restart_view, "accumulator_resets");
        need(equal(persistence, resets));
        status("accumulator_persistence", resets.children().empty() ? "review" : "fail");
        const auto& reloads = get(restart_view, "pointer_reloads");
        const auto& index = get(restart_view, "index_restart");
        bool index_updates = false;
        if (index.kind() != K::null) {
            need(times_eight(get(index, "stride_words"), get(index, "stride_bytes")));
            if (get(index, "index").kind() != K::null) {
                const auto index_name = get(index, "index").text();
                need(!index_name.empty() && canonical_gpr(index_name) == index_name);
                for (const auto& item : instructions()) {
                    const auto address = u(get(item, "address"));
                    const auto mnemonic = get(item, "mnemonic").text();
                    if (address < u(get(restart_view, "outer_target")) ||
                        address > u(get(restart_view, "outer_back_edge")) ||
                        (mnemonic != "add" && mnemonic != "sub"))
                        continue;
                    for (const auto& definition : get(item, "register_def_candidates").children())
                        index_updates |= canonical_gpr(definition.text()) == index_name;
                }
            }
            if (get(index, "stable_base").kind() != K::null)
                need(!get(index, "stable_base").text().empty() &&
                     canonical_gpr(get(index, "stable_base").text()) ==
                         get(index, "stable_base").text());
        }
        std::string_view model, status_value;
        if (!resets.children().empty()) {
            model = "accumulator_reset";
            status_value = "fail";
        } else if (!reloads.children().empty()) {
            model = "pointer_reload";
            status_value = "pass";
        } else {
            bool index_pass = false, index_fail = false;
            if (index.kind() != K::null) {
                need(times_eight(get(index, "stride_words"), get(index, "stride_bytes")));
                const bool stable = get(index, "stable_base").kind() != K::null &&
                                    get(index, "index").kind() != K::null &&
                                    get(index, "base_changes").children().empty();
                index_pass = stable && !get(index, "zeroing").children().empty();
                index_fail = stable && !index_pass &&
                             (!get(index, "header_definitions").children().empty() ||
                              !get(index, "late_zeroing").children().empty() || index_updates);
            }
            if (index_pass || index_fail) {
                model = "scaled_index_reset";
                status_value = index_pass ? "pass" : "fail";
            } else if (index.kind() == K::null &&
                       !get(affine, "cursor_registers").children().empty()) {
                model = "pointer_restart_missing";
                status_value = "fail";
            } else {
                model = "unresolved";
                status_value = "review";
            }
        }
        eq(get(restart_view, "model"), model);
        status("pass_restart", status_value);
    }
    void structural_predicates() const {
        for (auto suffix : {"vector_gather", "benchmark_store"})
            empty_status(suffix);
        if (auto unresolved = finding("unresolved_store_provenance"))
            need(!unresolved->children().empty());
        const auto& review =
            get(get(get(root, "human_review"), "items").children()[0], "evidence").children()[0];
        if (family == Family::latency)
            latency_predicates(review);
        else
            bandwidth_predicates(review);
    }
    void reviews() {
        const auto& human = get(root, "human_review");
        object(human, {"required", "status", "items"});
        need(boolean(get(human, "required")));
        eq(get(human, "status"), "pending");
        kind(get(human, "items"), K::array);
        auto literals = family == Family::latency
                            ? std::span<const detail::ReviewLiteral>{detail::latency_reviews}
                            : std::span<const detail::ReviewLiteral>{detail::bandwidth_reviews};
        auto items = get(human, "items").children();
        need(items.size() == literals.size());
        const V* first = nullptr;
        for (std::size_t i = 0; i < items.size(); ++i) {
            const auto& item = items[i];
            object(item, {"id", "question", "evidence"});
            eq(get(item, "id"), literals[i].id);
            eq(get(item, "question"), literals[i].question);
            const auto& evidence = get(item, "evidence");
            kind(evidence, K::array);
            need(evidence.children().size() == 1);
            const auto& n = evidence.children()[0];
            if (first) {
                need(equal(n, *first));
                continue;
            }
            first = &n;
            if (family == Family::latency) {
                object(n, {"loop", "back_edges", "memory_reads", "proven_successor_candidates",
                           "vector_candidates", "generic_findings", "return_register"});
                nullable(get(n, "loop"), [&](const V& v) { range(v); });
                loads(get(n, "proven_successor_candidates"));
                eq(get(n, "return_register"), "rax");
                if (auto loop_finding = finding("candidate_loop")) {
                    const auto& loop_view = get(n, "loop");
                    need(equal(get(loop_view, "start"), get(*loop_finding, "start")) &&
                         equal(get(loop_view, "stop"), get(*loop_finding, "stop")));
                } else
                    need(get(n, "loop").kind() == K::null);
                if (auto dep = finding("dependency_chain")) {
                    const auto candidates = get(n, "proven_successor_candidates").children();
                    if (candidates.size() == 1)
                        need(equal(get(*dep, "address"), candidates.front()));
                    else
                        need(get(*dep, "address").kind() == K::null);
                }
            } else {
                object(n, {"block_loop", "pass_loop", "back_edges", "memory_reads",
                           "benchmark_candidates", "vector_candidates", "generic_findings",
                           "abi_return_register"});
                nullable(get(n, "block_loop"), [&](const V& v) { pair(v); });
                nullable(get(n, "pass_loop"), [&](const V& v) { pair(v); });
                loads(get(n, "benchmark_candidates"));
                const auto candidates_count = get(n, "benchmark_candidates").children().size();
                if (auto count = finding("benchmark_load.count"))
                    need(equal(*count, get(n, "benchmark_candidates")));
                for (auto suffix : {"benchmark_load.missing", "benchmark_load.extra"})
                    if (auto count = finding(suffix))
                        need(u(get(*count, "actual")) == candidates_count);
                eq(get(n, "abi_return_register"), "rax");
                if (auto traversal = finding("traversal")) {
                    need(equal(get(n, "block_loop"), get(*traversal, "block")) &&
                         equal(get(n, "pass_loop"), get(*traversal, "pass")));
                    auto block = finding("block_loop");
                    need(block && equal(get(*block, "start"), get(n, "block_loop").children()[0]) &&
                         equal(get(*block, "stop"), get(n, "block_loop").children()[1]));
                    auto pass = finding("pass_structure");
                    need(pass != nullptr);
                    if (pass->kind() != K::null) {
                        need(get(n, "pass_loop").kind() == K::array &&
                             get(n, "pass_loop").children().size() == 2);
                        need(equal(get(*pass, "start"), get(n, "pass_loop").children()[0]) &&
                             equal(get(*pass, "stop"), get(n, "pass_loop").children()[1]));
                    } else
                        need(get(n, "pass_loop").kind() == K::null);
                    if (auto restart_view = finding("pass_restart");
                        restart_view && restart_view->kind() != K::null) {
                        need(equal(get(*restart_view, "outer_target"),
                                   get(n, "pass_loop").children()[0]) &&
                             equal(get(*restart_view, "outer_back_edge"),
                                   get(n, "pass_loop").children()[1]));
                        auto candidates = get(n, "benchmark_candidates").children();
                        auto first_load = u(get(n, "block_loop").children()[0]);
                        if (!candidates.empty()) {
                            first_load = std::numeric_limits<std::uint64_t>::max();
                            for (const auto& load_view : candidates)
                                first_load = std::min(first_load, u(get(load_view, "address")));
                        }
                        need(u(get(*restart_view, "first_load")) == first_load);
                    }
                } else
                    need(get(n, "block_loop").kind() == K::null &&
                         get(n, "pass_loop").kind() == K::null);
            }
            addresses(get(n, "back_edges"));
            loads(get(n, "memory_reads"));
            addresses(get(n, "vector_candidates"));
            need(equal(get(n, "back_edges"), get(get(root, "inventory"), "back_edges")) &&
                 equal(get(n, "vector_candidates"),
                       get(get(root, "inventory"), "vector_candidates")) &&
                 equal(get(n, "generic_findings"), get(root, "generic_findings")));
        }
    }
    void run() {
        common();
        framework();
        if (family == Family::latency)
            latency();
        else
            bandwidth();
        need(cursor == findings().size());
        reviews();
        structural_predicates();
        eq(get(root, "automated_disposition"), failed ? "fail" : "pass");
        eq(get(root, "qualification_status"),
           failed ? "automated_failure" : "pending_human_review");
    }
};
} // namespace
struct Owner::Storage {
    std::string bytes;
    Value root;
    core::Sha256Digest digest;
    Usage usage;
    Family family;
    Disposition disposition;
};
Owner::Owner(std::unique_ptr<Storage> s) : storage_(std::move(s)) {}
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
auto Owner::report() const noexcept -> std::optional<ReportView> {
    if (!storage_)
        return std::nullopt;
    return ReportView{storage_->family, storage_->disposition, storage_->root};
}
auto Owner::bytes() const noexcept -> std::string_view {
    return storage_ ? std::string_view{storage_->bytes} : std::string_view{};
}
auto Owner::digest() const noexcept -> const core::Sha256Digest* {
    return storage_ ? &storage_->digest : nullptr;
}
auto Owner::usage() const noexcept -> Usage { return storage_ ? storage_->usage : Usage{}; }
auto retain(std::string_view bytes, Limits limits) -> std::expected<Owner, core::Error> {
    try {
        need(limits.depth <= 64, "invalid_limits");
        need(limits.nodes <= std::vector<Value>{}.max_size() &&
                 limits.entries <= std::vector<Value>{}.max_size(),
             "invalid_limits");
        const auto max = std::string{}.max_size();
        for (auto n : {limits.input_bytes, limits.string_bytes, limits.integer_digits, limits.nodes,
                       limits.entries, limits.retained_bytes})
            need(n <= max, "invalid_limits");
        Parser preflight{bytes, limits};
        (void)preflight.run();
        add(preflight.usage.retained_bytes, sizeof(Owner::Storage) - sizeof(Value),
            limits.retained_bytes);
        Parser parser{bytes, limits, {}, 0, true};
        auto root = parser.run();
        add(parser.usage.retained_bytes, sizeof(Owner::Storage) - sizeof(Value),
            limits.retained_bytes);
        Validator validation{root};
        validation.run();
        auto digest = core::sha256(bytes);
        auto storage = std::make_unique<Owner::Storage>(Owner::Storage{
            std::string{bytes}, std::move(root), digest, parser.usage, validation.family,
            validation.failed ? Disposition::fail : Disposition::pass});
        return Owner{std::move(storage)};
    } catch (const Rejected& r) {
        return std::unexpected(error(r.code));
    }
}
} // namespace epycbench::results::kernel_report
