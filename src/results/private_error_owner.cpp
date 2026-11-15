#include "results/private_error_owner.hpp"
#include "core/json_ownership.hpp"
#include <algorithm>
#include <array>
#include <limits>
#include <utility>
namespace epycbench::results::private_error {
using Json=nlohmann::json;
struct Owner::Storage {
    core::Error error;
    Usage usage;
    ~Storage(){core::detail::retire_json(error.context);}
};
namespace {
struct Reject {const char* code;};
void add(std::size_t& value,std::size_t amount){if(!detail::checked_add(value,amount))throw Reject{"capacity"};}
template<class Enum,class Names> auto known(Enum value,const Names& names) noexcept -> bool {
    return std::any_of(names.begin(),names.end(),[value](const auto& item){return item.first==value;});
}
auto failure(const char* suffix) -> core::Error {
    return {.category=core::ErrorCategory::results,
        .code=core::StableErrorCode{"results.private_error."+std::string{suffix}},
        .message="Supplied private error rejected",.operation="retain_private_error",
        .os_error=std::nullopt,.context=Json::object()};
}
auto preflight(const core::Error& source,Limits limits) -> Usage {
    if(!known(source.category,core::detail::error_category_names) ||
       !known(source.severity,core::detail::error_severity_names) ||
       !known(source.continuation,core::detail::continuation_policy_names))throw Reject{"invalid_input"};
    if(limits.depth>64 || limits.nodes>Json::array_t{}.max_size() ||
       limits.string_bytes>std::string{}.max_size() || limits.binary_bytes>Json::binary_t{}.max_size())throw Reject{"capacity"};
    Usage usage{};usage.retained_bytes=Owner::fixed_bytes();
    auto check=[&]{if(usage.nodes>limits.nodes || usage.depth>limits.depth || usage.string_bytes>limits.string_bytes ||
        usage.binary_bytes>limits.binary_bytes || usage.retained_bytes>limits.retained_bytes)throw Reject{"capacity"};};
    auto string=[&](std::size_t bytes){add(usage.string_bytes,bytes);add(usage.retained_bytes,bytes);check();};
    string(source.code.value().size());string(source.message.size());string(source.operation.size());
    if(source.os_error){string(source.os_error->domain.size());string(source.os_error->message.size());}
    struct Frame {const Json* container{};Json::const_iterator current{},end{};std::size_t depth{};};
    std::array<Frame,65> stack{};std::size_t active{};
    const Json* node=&source.context;std::size_t depth{};
    while(true){
        add(usage.nodes,1);usage.depth=std::max(usage.depth,depth);check();
        if(node->is_string()){add(usage.retained_bytes,sizeof(Json::string_t));string(node->get_ref<const Json::string_t&>().size());}
        else if(node->is_binary()){add(usage.retained_bytes,sizeof(Json::binary_t));add(usage.binary_bytes,node->get_binary().size());add(usage.retained_bytes,node->get_binary().size());}
        else if(node->is_array())add(usage.retained_bytes,sizeof(Json::array_t));
        else if(node->is_object())add(usage.retained_bytes,sizeof(Json::object_t));
        check();
        if(node->is_structured() && !node->empty()){
            if(depth==64 || active==stack.size())throw Reject{"capacity"};
            stack[active++]={node,node->cbegin(),node->cend(),depth};
        }
        while(active && stack[active-1].current==stack[active-1].end)--active;
        if(!active)break;
        auto& frame=stack[active-1];
        if(frame.container->is_object()){
            add(usage.retained_bytes,sizeof(Json::object_t::value_type));string(frame.current.key().size());
        }else add(usage.retained_bytes,sizeof(Json));
        check();node=&frame.current.value();depth=frame.depth+1;++frame.current;
    }
    return usage;
}
}
auto detail::checked_add(std::size_t& value,std::size_t amount) noexcept -> bool {
    if(amount>std::numeric_limits<std::size_t>::max()-value)return false;
    value+=amount;return true;
}
Owner::Owner(std::unique_ptr<Storage> storage) noexcept:storage_(std::move(storage)){}
Owner::Owner(Owner&&) noexcept=default;
auto Owner::operator=(Owner&&) noexcept -> Owner& =default;
Owner::~Owner()=default;
auto Owner::source() const noexcept -> const core::Error* {return storage_?&storage_->error:nullptr;}
auto Owner::usage() const noexcept -> Usage {return storage_?storage_->usage:Usage{};}
auto Owner::fixed_bytes() noexcept -> std::size_t {return sizeof(Owner)+sizeof(Storage);}
auto retain(const core::Error& source,Limits limits) -> std::expected<Owner,core::Error> {
    try{
        const auto usage=preflight(source,limits);
        auto storage=std::make_unique<Owner::Storage>();
        storage->usage=usage;
        auto& result=storage->error;
        result.category=source.category;result.code=source.code;result.message=source.message;result.operation=source.operation;
        result.os_error=source.os_error;result.severity=source.severity;result.continuation=source.continuation;
        auto context=core::detail::clone_json(source.context);core::detail::JsonCleanup cleanup{context};
        result.context=std::move(context);
        return Owner{std::move(storage)};
    }catch(const Reject& rejected){return std::unexpected{failure(rejected.code)};}
}
} // namespace epycbench::results::private_error
