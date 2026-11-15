#pragma once

namespace epycbench::runtime::detail {

// nlohmann's destructor allocates a flattening stack for nonempty structured
// values. Retire children of exclusively owned diagnostic/request trees first
// so every subsequent owning destructor sees an empty container. Call only
// after all borrowed views and diagnostic classification have been consumed.
template <typename Json>
void retire_json(Json& value) noexcept {
    while (value.is_structured() && !value.empty()) {
        auto* parent = &value;
        auto last = parent->end();
        --last;
        auto* child = &*last;
        while (child->is_structured() && !child->empty()) {
            parent = child;
            last = parent->end();
            --last;
            child = &*last;
        }
        // Erase only a primitive or empty container. Restarting from the root
        // trades traversal time for constant stack space at arbitrary depth.
        parent->erase(last);
    }
}

} // namespace epycbench::runtime::detail
