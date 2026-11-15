#include "platform_linux/presentation_files.hpp"

#include "epycbench/platform_linux/file_descriptor.hpp"
#include "platform_linux/journal_file.hpp"

#include <cerrno>
#include <fcntl.h>
#include <span>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace epycbench::platform_linux::presentation_files {
namespace {
auto leaf_valid(std::string_view name) noexcept -> bool {
    return !name.empty()&&name.size()<=255&&name!="."&&name!=".."&&
        name.find('/')==name.npos&&name.find('\0')==name.npos;
}
auto absolute_path(const std::filesystem::path& path) -> std::expected<std::filesystem::path,Failure> {
    std::error_code error;
    auto result=std::filesystem::absolute(path.empty()?".":path,error).lexically_normal();
    if(error)return std::unexpected{Failure{Stage::argument,error.value()}};
    const auto text=result.string();
    if(text.size()>4095||text.find('\0')!=text.npos)
        return std::unexpected{Failure{Stage::argument,ENAMETOOLONG}};
    return result;
}
auto opened(int raw) -> std::expected<FileDescriptor,Failure> {
    if(raw<0)return std::unexpected{Failure{Stage::open,errno}};
    auto result=FileDescriptor::adopt(raw);
    if(!result) {
        static_cast<void>(::close(raw));
        return std::unexpected{Failure{Stage::open,0}};
    }
    return std::move(*result);
}
auto directory(const std::filesystem::path& path) -> std::expected<FileDescriptor,Failure> {
    auto current=opened(::open("/",O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW));
    if(!current)return std::unexpected{current.error()};
    for(const auto& part:path.relative_path()) {
        const auto name=part.string();
        if(name.empty())continue;
        if(!leaf_valid(name))return std::unexpected{Failure{Stage::argument,EINVAL}};
        auto next=opened(::openat(current->native_handle(),name.c_str(),
            O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW));
        if(!next)return std::unexpected{next.error()};
        if(!current->close())return std::unexpected{Failure{Stage::close,0}};
        current=std::move(next);
    }
    return current;
}
auto private_identity(int descriptor) -> std::expected<void,Failure> {
    struct stat status{};
    if(::fstat(descriptor,&status)!=0)return std::unexpected{Failure{Stage::identity,errno}};
    if(!S_ISDIR(status.st_mode)||status.st_uid!=::geteuid()||(status.st_mode&07777)!=0700)
        return std::unexpected{Failure{Stage::identity,EACCES}};
    return {};
}
}
auto ensure_private_subdirectory(const std::filesystem::path& parent,std::string_view name)
    -> std::expected<std::string,Failure> {
    if(!leaf_valid(name))return std::unexpected{Failure{Stage::argument,EINVAL}};
    auto path=absolute_path(parent);if(!path)return std::unexpected{path.error()};
    const auto result=(*path/std::string{name}).string();
    if(result.size()>4095)return std::unexpected{Failure{Stage::argument,ENAMETOOLONG}};
    auto owner=directory(*path);if(!owner)return std::unexpected{owner.error()};
    const std::string child{name};
    if(::mkdirat(owner->native_handle(),child.c_str(),0700)!=0&&errno!=EEXIST)
        return std::unexpected{Failure{Stage::directory,errno}};
    auto held=opened(::openat(owner->native_handle(),child.c_str(),
        O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW));
    if(!held)return std::unexpected{held.error()};
    auto checked=private_identity(held->native_handle());if(!checked)return std::unexpected{checked.error()};
    if(!held->close()||!owner->close())return std::unexpected{Failure{Stage::close,0}};
    return result;
}
auto save_numbered(const std::filesystem::path& directory_path,std::string_view filename,std::string_view bytes)
    -> std::expected<std::string,Failure> {
    if(!leaf_valid(filename)||bytes.size()>(256U<<20U))
        return std::unexpected{Failure{Stage::argument,EINVAL}};
    auto path=absolute_path(directory_path);if(!path)return std::unexpected{path.error()};
    const auto root=path->string();
    const std::filesystem::path original{std::string{filename}};
    const journal_file::Limits limits{4095,256,255,1,2,256U<<20U,64U<<10U,20000};
    for(unsigned number=0;number<10000;++number) {
        const auto leaf=number==0?std::string{filename}:
            original.stem().string()+"-"+std::to_string(number)+original.extension().string();
        auto file=journal_file::File::create(root,leaf,limits);
        if(!file) {
            const auto& failure=file.error();
            if(failure.primary.reason==journal_file::Reason::collision&&!failure.cleanup.count)continue;
            return std::unexpected{Failure{Stage::open,failure.primary.native_error}};
        }
        std::size_t offset{};
        while(offset<bytes.size()) {
            auto wrote=file->write_some(std::as_bytes(std::span{bytes.data()+offset,bytes.size()-offset}));
            if(!wrote)return std::unexpected{Failure{Stage::write,wrote.error().primary.native_error}};
            offset+=*wrote;
        }
        const auto closed=file->close();
        if(closed.count)return std::unexpected{Failure{Stage::close,closed.issues[0].native_error}};
        return (*path/leaf).string();
    }
    return std::unexpected{Failure{Stage::open,EEXIST}};
}
auto file_status(const std::filesystem::path& filename) -> std::expected<FileStatus,Failure> {
    auto path=absolute_path(filename);if(!path)return std::unexpected{path.error()};
    auto owner=directory(path->parent_path());
    if(!owner) {
        if(owner.error().error_number==ENOENT)return FileStatus{};
        return std::unexpected{owner.error()};
    }
    struct stat value{};
    const auto result=::fstatat(owner->native_handle(),path->filename().c_str(),&value,AT_SYMLINK_NOFOLLOW);
    const auto number=errno;
    if(!owner->close())return std::unexpected{Failure{Stage::close,0}};
    if(result!=0) {
        if(number==ENOENT)return FileStatus{};
        return std::unexpected{Failure{Stage::status,number}};
    }
    if(!S_ISREG(value.st_mode))return std::unexpected{Failure{Stage::identity,EINVAL}};
    return FileStatus{true,static_cast<std::int64_t>(value.st_mtim.tv_sec)};
}
auto stdout_is_terminal() noexcept -> bool {return ::isatty(STDOUT_FILENO)==1;}
auto terminal_dimensions() noexcept -> TerminalDimensions {
    struct winsize size{};
    if(::ioctl(STDOUT_FILENO,TIOCGWINSZ,&size)==0&&size.ws_col&&size.ws_row)
        return {size.ws_col,size.ws_row};
    return {};
}
} // namespace epycbench::platform_linux::presentation_files
