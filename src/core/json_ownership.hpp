#pragma once
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <type_traits>
namespace epycbench::core::detail {
template <class Json>
auto json_string_equal(const Json& value, std::string_view text) noexcept -> bool {
    return value.is_string() && value.template get_ref<const typename Json::string_t&>() == text;
}
// Only exclusively owned, parser-bounded trees after their last borrow.
template <class Json> void retire_json(Json& value) noexcept {
    if (value.is_structured()) {
        for (auto& child : value)
            retire_json(child);
        value.clear();
    }
}
template <class Json> struct JsonCleanup {
    Json& value;
    ~JsonCleanup() { retire_json(value); }
};
template <class Expected> struct ExpectedErrorCleanup {
    Expected& owner;
    ~ExpectedErrorCleanup() {
        if (!owner)
            retire_json(owner.error().context);
    }
};
template <class Json> void append_object(Json& object, std::string_view name, Json& child) {
    if constexpr (std::is_same_v<Json, nlohmann::json>) {
        std::string key{name};
        object.template get_ref<typename Json::object_t&>().emplace(std::move(key),
                                                                    std::move(child));
    } else {
        using Container = typename Json::object_t::Container;
        auto& entries =
            static_cast<Container&>(object.template get_ref<typename Json::object_t&>());
        if (entries.size() == entries.capacity()) {
            Json replacement = Json::object();
            JsonCleanup cleanup{replacement};
            auto& destination =
                static_cast<Container&>(replacement.template get_ref<typename Json::object_t&>());
            const auto count = entries.size();
            if (count == destination.max_size())
                throw std::length_error{"JSON object capacity"};
            destination.reserve(
                count == 0 ? 1 : (count > destination.max_size() / 2 ? count + 1 : count * 2));
            for (auto& entry : entries) {
                std::string key{entry.first};
                destination.emplace_back(std::move(key), std::move(entry.second));
            }
            object.swap(replacement);
        }
        std::string key{name};
        auto& destination =
            static_cast<Container&>(object.template get_ref<typename Json::object_t&>());
        destination.emplace_back(std::move(key), std::move(child));
    }
}
template <class Json, class Source> auto clone_json_as(const Source& source) -> Json {
    if (!source.is_structured())
        return source;
    Json result = source.is_object() ? Json::object() : Json::array();
    JsonCleanup cleanup{result};
    if (source.is_object()) {
        if constexpr (std::is_same_v<Json, nlohmann::ordered_json>) {
            using Container = typename Json::object_t::Container;
            static_cast<Container&>(result.template get_ref<typename Json::object_t&>())
                .reserve(source.size());
        }
        for (auto it = source.begin(); it != source.end(); ++it) {
            auto child = clone_json_as<Json>(it.value());
            JsonCleanup child_cleanup{child};
            append_object(result, it.key(), child);
        }
    } else {
        result.template get_ref<typename Json::array_t&>().reserve(source.size());
        for (const auto& value : source) {
            auto child = clone_json_as<Json>(value);
            JsonCleanup child_cleanup{child};
            result.push_back(std::move(child));
        }
    }
    return Json(std::move(result));
}
template <class Json> auto clone_json(const Json& source) -> Json {
    return clone_json_as<Json>(source);
}
template <class Json>
void set_json(Json& object, std::string_view key, std::type_identity_t<Json>&& value) {
    JsonCleanup cleanup{value};
    if (auto it = object.find(key); it != object.end()) {
        retire_json(it.value());
        it.value().swap(value);
    } else
        append_object(object, key, value);
}
template <class Json>
void set_json(Json& object, std::string_view key, const std::type_identity_t<Json>& value) {
    auto copy = clone_json(value);
    JsonCleanup cleanup{copy};
    set_json(object, key, std::move(copy));
}
template <class Json = nlohmann::ordered_json, class F> auto json_object(F&& fill) -> Json {
    auto object = Json::object();
    JsonCleanup cleanup{object};
    fill(object);
    return Json(std::move(object));
}
template <class Json> struct OwnedJson {
    Json value;
    OwnedJson(Json&& json) noexcept : value(std::move(json)) {}
    OwnedJson(const OwnedJson& other) : value(clone_json(other.value)) {}
    OwnedJson(OwnedJson&&) noexcept = default;
    auto operator=(OwnedJson&& other) noexcept -> OwnedJson& {
        if (this != &other) {
            retire_json(value);
            value = std::move(other.value);
        }
        return *this;
    }
    auto operator=(const OwnedJson& other) -> OwnedJson& {
        OwnedJson copy{other};
        return *this = std::move(copy);
    }
    ~OwnedJson() { retire_json(value); }
    operator const Json&() const noexcept { return value; }
};
using JsonOwner = OwnedJson<nlohmann::ordered_json>;
template <class Json> struct JsonPointerCleanup {
    Json* value;
    ~JsonPointerCleanup() {
        if (value)
            retire_json(*value);
    }
};
template <class Json> void append_json(Json& array, std::type_identity_t<Json>&& value) {
    JsonCleanup cleanup{value};
    array.push_back(std::move(value));
}
template <class Json, class F> void append_object_json(Json& array, F&& fill) {
    auto value = json_object<Json>(std::forward<F>(fill));
    JsonCleanup cleanup{value};
    array.push_back(std::move(value));
}
} // namespace epycbench::core::detail
