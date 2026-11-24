#include "results/result_export.hpp"

namespace epycbench::results::result_export {
auto profile(std::string_view text) noexcept -> std::optional<Profile> {
    for (const auto p : {Profile::complete,Profile::shareable,Profile::summary,Profile::observation_table})
        if (name(p)==text) return p;
    return {};
}
auto name(Profile p) noexcept -> std::string_view {
    switch(p) {
    case Profile::complete:return "complete";
    case Profile::shareable:return "shareable";
    case Profile::summary:return "summary";
    case Profile::observation_table:return "observation-table";
    }
    return {};
}
} // namespace epycbench::results::result_export
