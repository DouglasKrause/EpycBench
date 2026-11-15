#include "platform_linux/benchmark_run_input.hpp"

#include <algorithm>
#include <bit>
#include <charconv>
#include <cerrno>
#include <cpuid.h>
#include <fcntl.h>
#include <sys/prctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>

namespace epycbench::platform_linux::benchmark_run_input {
namespace {
constexpr std::size_t mib=1024U*1024U;
void fail(Outcome& result,Failure failure,bool cleanup=false) {
    if (!result.primary) result.primary=std::move(failure);
    else if (cleanup && result.cleanup_count<result.cleanup.size())
        result.cleanup[result.cleanup_count++]=std::move(failure);
}
auto failure(Stage stage,int number=0) -> Failure { return {stage,number,std::nullopt}; }
void observe(ObservationTime time,Outcome& outcome) noexcept {
    try {
        auto recorded=time.record(time.context);
        if(!recorded) fail(outcome,std::move(recorded.error()));
    } catch (...) { fail(outcome,failure(Stage::clock)); }
}
auto file_operations_valid(const FileOperations& operations) noexcept -> bool {
    const auto& f=operations.files;
    return f.open_root && f.open_at && f.stat_fd && f.read && f.close && operations.effective_uid;
}
class Descriptor {
  public:
    Descriptor(int value,const FileOperations& operations,Outcome& result) noexcept
        :value_{value},operations_{operations},result_{result} {}
    Descriptor(const Descriptor&)=delete;
    ~Descriptor() noexcept { close(); }
    auto get() const noexcept -> int { return value_; }
    void close() noexcept {
        if (value_<0) return;
        const auto value=value_;value_=-1;
        const auto status=operations_.files.close(operations_.files.context,value);
        if (!status.succeeded()) fail(result_,failure(Stage::close,status.native_error),true);
    }
    auto release() noexcept -> int { const auto value=value_;value_=-1;return value; }
  private:
    int value_;
    const FileOperations& operations_;
    Outcome& result_;
};
auto valid_absolute(std::string_view path) noexcept -> bool {
    if (path.empty() || path.front()!='/' || path.size()>4095 || path.find('\0')!=path.npos) return false;
    std::size_t count{},start=1;
    while (start<path.size()) {
        const auto end=path.find('/',start);
        const auto part=path.substr(start,end==path.npos ? path.size()-start : end-start);
        if (part.empty() || part=="." || part==".." || part.size()>255 || ++count>256) return false;
        if (end==path.npos) return true;
        start=end+1;
        if (start==path.size()) return false;
    }
    return path=="/";
}
auto stat_identity(int fd,bool directory,bool private_mode,const FileOperations& operations,
                   Outcome& outcome) -> bool {
    struct stat status{};
    if (operations.files.stat_fd(operations.files.context,fd,&status)!=0) {
        fail(outcome,failure(Stage::identity,errno));return false;
    }
    const bool kind=directory ? S_ISDIR(status.st_mode) : S_ISREG(status.st_mode);
    const bool privacy=!private_mode ||
        (static_cast<std::uint64_t>(status.st_uid)==operations.effective_uid(operations.files.context) &&
         (status.st_mode&0777)==(directory ? 0700 : 0600));
    if (!kind || !privacy || (!directory && status.st_nlink!=1)) {
        fail(outcome,failure(Stage::identity));return false;
    }
    return true;
}
auto open_directory(std::string_view root,std::size_t private_from,const FileOperations& operations,
                    Outcome& outcome,bool relative=false) -> int {
    const auto& f=operations.files;
    int current=f.open_root(f.context,relative?".":"/",O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
    if (current<0) { fail(outcome,failure(Stage::open,errno));return -1; }
    for (std::size_t start=relative?0U:1U;start<root.size();) {
        Descriptor parent{current,operations,outcome};
        const auto slash=root.find('/',start);
        const auto part=root.substr(start,slash==root.npos ? root.size()-start : slash-start);
        std::array<char,256> name{};std::copy(part.begin(),part.end(),name.begin());
        current=f.open_at(f.context,parent.get(),name.data(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
        if (current<0) { fail(outcome,failure(Stage::open,errno));return -1; }
        Descriptor child{current,operations,outcome};
        const auto component_end=slash==root.npos ? root.size() : slash;
        if (!stat_identity(current,true,private_from && component_end>=private_from,operations,outcome)) return -1;
        parent.close();
        if (!outcome.succeeded()) return -1;
        current=child.release();
        if (slash==root.npos) return current;
        start=slash+1;
    }
    Descriptor parent{current,operations,outcome};
    if (!stat_identity(current,true,private_from!=0,operations,outcome)) return -1;
    return parent.release();
}
auto read_from_directory(int root,std::string_view leaf,std::size_t limit,
    bool private_mode,FileOperations operations,ObservationTime time={}) -> TextRead {
    TextRead result;
    try {
        std::array<char,256> name{};std::copy(leaf.begin(),leaf.end(),name.begin());
        const auto fd=operations.files.open_at(operations.files.context,root,name.data(),
            O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
        if (fd<0) { fail(result.outcome,failure(Stage::open,errno));return result; }
        Descriptor file{fd,operations,result.outcome};
        if (stat_identity(fd,false,private_mode,operations,result.outcome)) {
            std::array<char,4096> buffer{};
            std::int64_t offset{};
            for (;;) {
                const auto room=limit-result.bytes.size();
                const auto requested=std::min(buffer.size(),room+1);
                const auto count=operations.files.read(operations.files.context,fd,buffer.data(),requested,offset);
                if (count<0) { fail(result.outcome,failure(Stage::read,errno));break; }
                if (count==0) break;
                const auto received=static_cast<std::size_t>(count);
                if (received>requested || received>room) { fail(result.outcome,failure(Stage::admission));break; }
                try { result.bytes.append(buffer.data(),received); }
                catch(...) {
                    // Select the allocation failure while the descriptor is
                    // still owned; retirement cannot precede its primary.
                    fail(result.outcome,failure(Stage::admission));break;
                }
                offset+=count;
            }
        }
        if(time.record && result.outcome.succeeded()) observe(time,result.outcome);
        file.close();
    } catch(...) { fail(result.outcome,failure(Stage::admission)); }
    return result;
}
auto read_leaf(std::string_view directory,std::string_view leaf,std::size_t limit,
               bool private_mode,FileOperations operations,std::size_t private_from=0,ObservationTime time={}) -> TextRead {
    TextRead result;
    if (!file_operations_valid(operations) || !valid_absolute(directory) || leaf.empty() ||
        leaf.size()>255 || leaf.find('/')!=leaf.npos || leaf.find('\0')!=leaf.npos || limit>8U*mib) {
        fail(result.outcome,failure(Stage::admission));return result;
    }
    try {
        const auto root=open_directory(directory,private_from,operations,result.outcome);
        if (root<0) return result;
        Descriptor parent{root,operations,result.outcome};
        result=read_from_directory(root,leaf,limit,private_mode,operations,time);
        parent.close();
    } catch (...) { fail(result.outcome,failure(Stage::admission)); }
    return result;
}
struct SourceForward {
        detail::BoundedSysfsOperations operations;
        std::size_t policies{},bytes{},reads{},call_limit{};
        bool frequency{};
        std::size_t byte_limit{};
    auto forwarding_operations() noexcept -> detail::BoundedSysfsOperations {
    auto wrapped=operations;wrapped.context=this;
    wrapped.open_root=[](void* p,const char* path,int flags) noexcept {
        auto& f=*static_cast<SourceForward*>(p);return f.operations.open_root(f.operations.context,path,flags);
    };
    wrapped.open_at=[](void* p,int fd,const char* name,int flags) noexcept {
        auto& f=*static_cast<SourceForward*>(p);return f.operations.open_at(f.operations.context,fd,name,flags);
    };
    wrapped.stat_fd=[](void* p,int fd,struct stat* value) noexcept {
        auto& f=*static_cast<SourceForward*>(p);return f.operations.stat_fd(f.operations.context,fd,value);
    };
    wrapped.stat_at=[](void* p,int fd,const char* name,struct stat* value) noexcept {
        auto& f=*static_cast<SourceForward*>(p);return f.operations.stat_at(f.operations.context,fd,name,value);
    };
    wrapped.next=[](void* p,int fd,detail::BoundedSysfsEnumerationState* state,char* out,std::size_t size) noexcept {
        auto& f=*static_cast<SourceForward*>(p);
        const auto next=f.operations.next(f.operations.context,fd,state,out,size);
        if(next<=0) return next;
        const auto end=std::find(out,out+size,'\0');
        if(end==out+size) { errno=ENAMETOOLONG;return -1; }
        const std::string_view name{out,static_cast<std::size_t>(end-out)};
        if(f.frequency && name.starts_with("policy") && name.size()>6) {
            std::uint64_t number{};
            const auto parsed=std::from_chars(name.data()+6,name.data()+name.size(),number);
            if(parsed.ec==std::errc{} && parsed.ptr==name.data()+name.size() && number<=4095) {
                if(f.policies==1024) { errno=EOVERFLOW;return -1; }
                ++f.policies;
            }
        }
        return next;
    };
    wrapped.read=[](void* p,int fd,void* out,std::size_t size,std::int64_t offset) noexcept -> std::ptrdiff_t {
        auto& f=*static_cast<SourceForward*>(p);
        if(f.reads==f.call_limit) { errno=EOVERFLOW;return -1; }
        ++f.reads;
        const auto remaining=f.byte_limit-f.bytes;
        const auto request=std::min(size,remaining+1);
        const auto value=f.operations.read(f.operations.context,fd,out,request,offset);
        if(value>0) {
            const auto received=static_cast<std::size_t>(value);
            if(received>request || received>remaining) { errno=EOVERFLOW;return -1; }
            f.bytes+=received;
        }
        return value;
    };
    wrapped.close=[](void* p,int fd) noexcept {
        auto& f=*static_cast<SourceForward*>(p);return f.operations.close(f.operations.context,fd);
    };
    return wrapped;
    }
};

}
auto read_evidence(std::string_view root,std::string_view identity,EvidenceFile file,
                   std::size_t limit,FileOperations operations) -> TextRead {
    TextRead failure_result;
    if (!valid_absolute(root) || identity.size()!=64 ||
        !std::ranges::all_of(identity,[](char c){ return (c>='0' && c<='9') || (c>='a' && c<='f'); })) {
        fail(failure_result.outcome,failure(Stage::admission));return failure_result;
    }
    std::string_view subdirectory,leaf;
    switch(file) {
    case EvidenceFile::approval:
        leaf="approval.json";
        if(limit>mib) { fail(failure_result.outcome,failure(Stage::admission));return failure_result; }
        break;
    case EvidenceFile::gcc_machine:subdirectory="/gcc";leaf="object-code-audit.json";break;
    case EvidenceFile::gcc_human:subdirectory="/gcc";leaf="object-code-audit.txt";break;
    case EvidenceFile::clang_machine:subdirectory="/clang";leaf="object-code-audit.json";break;
    case EvidenceFile::clang_human:subdirectory="/clang";leaf="object-code-audit.txt";break;
    default:fail(failure_result.outcome,failure(Stage::admission));return failure_result;
    }
    if (root.size()+1+identity.size()+subdirectory.size()>4095) {
        fail(failure_result.outcome,failure(Stage::admission));return failure_result;
    }
    std::string directory{root};directory+='/';directory+=identity;directory+=subdirectory;
    return read_leaf(directory,leaf,limit,true,operations,root.size());
}
auto read_authored(std::string_view path,FileOperations operations) -> TextRead {
    TextRead result;
    if(!file_operations_valid(operations)||path.empty()||path.size()>4095||path.find('\0')!=path.npos) {
        fail(result.outcome,failure(Stage::admission));return result;
    }
    const bool relative=path.front()!='/';std::size_t count{};
    for(std::size_t begin=relative?0U:1U;begin<path.size();) {
        const auto slash=path.find('/',begin);const auto part=path.substr(begin,slash==path.npos?path.size()-begin:slash-begin);
        if(part.empty()||part=="."||part==".."||part.size()>255||++count>256) {
            fail(result.outcome,failure(Stage::admission));return result;
        }
        if(slash==path.npos)break;
        begin=slash+1;if(begin==path.size()){fail(result.outcome,failure(Stage::admission));return result;}
    }
    const auto slash=path.rfind('/');const auto leaf=path.substr(slash==path.npos?0U:slash+1);
    if(leaf.empty()){fail(result.outcome,failure(Stage::admission));return result;}
    const auto directory=slash==path.npos?std::string_view{}:path.substr(0,slash==0?1:slash);
    const auto fd=open_directory(directory,0,operations,result.outcome,relative);
    if(fd<0)return result;
    Descriptor parent{fd,operations,result.outcome};
    result=read_from_directory(fd,leaf,16U*mib,false,operations);
    parent.close();return result;
}
auto prepare_demo_directory() -> TextRead {
    TextRead result;
    try {
        std::array<char,4096> current{};
        if(!::getcwd(current.data(),current.size())) {
            fail(result.outcome,failure(Stage::open,errno));return result;
        }
        const std::string_view directory{current.data()};
        constexpr std::string_view leaf="epycbench-demo-results";
        if(!valid_absolute(directory)||directory.size()+1+leaf.size()+10>4095) {
            fail(result.outcome,failure(Stage::admission,ENAMETOOLONG));return result;
        }
        result.bytes=directory;
        if(result.bytes.back()!='/')result.bytes+='/';
        result.bytes+=leaf;

        const auto operations=native_file_operations();
        const auto fd=open_directory(directory,0,operations,result.outcome);
        if(fd<0)return result;
        Descriptor parent{fd,operations,result.outcome};
        const auto create_child=[&](int parent_fd,const char* name) -> int {
            if(::mkdirat(parent_fd,name,0700)!=0&&errno!=EEXIST) {
                fail(result.outcome,failure(Stage::open,errno));return -1;
            }
            const auto child_fd=operations.files.open_at(operations.files.context,parent_fd,name,
                O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
            if(child_fd<0) {
                fail(result.outcome,failure(Stage::open,errno));return -1;
            }
            Descriptor child{child_fd,operations,result.outcome};
            if(!stat_identity(child_fd,true,true,operations,result.outcome)) {
                child.close();return -1;
            }
            return child.release();
        };
        Descriptor output{create_child(parent.get(),leaf.data()),operations,result.outcome};
        if(output.get()>=0) {
            Descriptor journal{create_child(output.get(),"epycbench"),operations,result.outcome};
            journal.close();
        }
        output.close();
        parent.close();
    } catch(...) { fail(result.outcome,failure(Stage::admission)); }
    return result;
}
auto read_source(SourceFile source,std::uint32_t cpu,FileOperations operations,ObservationTime time) -> TextRead {
    if(!time.record) { TextRead result;fail(result.outcome,failure(Stage::admission));return result; }
    switch(source) {
    case SourceFile::smt_control:return read_leaf("/sys/devices/system/cpu/smt","control",4096,false,operations,0,time);
    case SourceFile::smt_active:return read_leaf("/sys/devices/system/cpu/smt","active",4096,false,operations,0,time);
    case SourceFile::numa_balancing:return read_leaf("/proc/sys/kernel","numa_balancing",4096,false,operations,0,time);
    case SourceFile::thp_enabled:return read_leaf("/sys/kernel/mm/transparent_hugepage","enabled",4096,false,operations,0,time);
    case SourceFile::thp_defrag:return read_leaf("/sys/kernel/mm/transparent_hugepage","defrag",4096,false,operations,0,time);
    case SourceFile::microcode: {
        if(cpu>4095) break;
        std::array<char,16> text{};const auto formatted=std::to_chars(text.data(),text.data()+text.size(),cpu);
        std::string directory{"/sys/devices/system/cpu/cpu"};directory.append(text.data(),formatted.ptr);directory+="/microcode";
        return read_leaf(directory,"version",4096,false,operations,0,time);
    }
    }
    TextRead result;fail(result.outcome,failure(Stage::admission));return result;
}
auto read_kernel_identity(HostOperations operations,ObservationTime time) -> KernelIdentity {
    KernelIdentity result;
    if(!operations.kernel_identity || !time.record) {
        fail(result.outcome,failure(Stage::admission));return result;
    }
    try {
        auto fields=operations.kernel_identity(operations.context);
        if(!fields) fail(result.outcome,std::move(fields.error()));
        else if(std::ranges::any_of(*fields,[](const std::string& field) {
            return field.empty() || field.size()>4096 || field.find('\0')!=field.npos;
        })) fail(result.outcome,failure(Stage::kernel_identity));
        else { result.fields=std::move(*fields);observe(time,result.outcome); }
    } catch(...) { fail(result.outcome,failure(Stage::kernel_identity)); }
    return result;
}
auto read_thp_inventory(FileOperations operations,ObservationTime time) -> ThpInventory {
    ThpInventory result;
    if(!file_operations_valid(operations) || !operations.files.next || !operations.files.stat_at || !time.record) {
        fail(result.outcome,failure(Stage::admission));return result;
    }
    result.rows.reserve(54);
    constexpr std::string_view path="/sys/kernel/mm/transparent_hugepage";
    const auto fd=operations.files.open_root(operations.files.context,path.data(),
        O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
    if(fd<0) { fail(result.outcome,failure(Stage::open,errno));return result; }
    Descriptor root{fd,operations,result.outcome};
    if(!stat_identity(fd,true,false,operations,result.outcome)) {
        root.close();return result;
    }
    detail::BoundedSysfsEnumerationState enumeration;
    std::array<char,4097> name{};std::size_t entries{},candidates{};
    std::uint64_t seen_sizes{};
    for(;;) {
        const auto next=operations.files.next(operations.files.context,fd,&enumeration,name.data(),name.size());
        if(next<0) { fail(result.outcome,failure(Stage::read,errno));break; }
        if(next==0) { result.enumeration_complete=true;observe(time,result.outcome);break; }
        if(entries==256) { fail(result.outcome,failure(Stage::admission));break; }
        ++entries;
        const auto end=std::find(name.begin(),name.end(),'\0');
        if(end==name.end()) { fail(result.outcome,failure(Stage::admission));break; }
        const std::string_view entry{name.data(),static_cast<std::size_t>(end-name.begin())};
        if(!entry.starts_with("hugepages-")) continue;
        constexpr std::string_view prefix="hugepages-",suffix="kB";
        if(!entry.ends_with(suffix) || entry.size()<=prefix.size()+suffix.size()) {
            fail(result.outcome,failure(Stage::admission));break;
        }
        const auto number=entry.substr(prefix.size(),entry.size()-prefix.size()-suffix.size());
        std::uint64_t kib{};const auto parsed=std::from_chars(number.data(),number.data()+number.size(),kib);
        if(parsed.ec!=std::errc{} || parsed.ptr!=number.data()+number.size() || !kib ||
           (kib&(kib-1)) || kib>std::numeric_limits<std::uint64_t>::max()/1024) {
            fail(result.outcome,failure(Stage::admission));break;
        }
        const auto size_bit=std::uint64_t{1}<<std::countr_zero(kib);
        if(candidates==54 || (seen_sizes&size_bit)!=0) {
            fail(result.outcome,failure(Stage::admission));break;
        }
        ++candidates;seen_sizes|=size_bit;
        result.rows.push_back({kib*1024,false,{}});
        auto& row=result.rows.back();
        const auto directory=operations.files.open_at(operations.files.context,fd,name.data(),
            O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
        row.attempted=true;
        if(directory<0) { fail(row.enabled.outcome,failure(Stage::open,errno));fail(result.outcome,failure(Stage::open,errno));break; }
        bool shmem_only{};
        {
            Descriptor size{directory,operations,row.enabled.outcome};
            if(stat_identity(directory,true,false,operations,row.enabled.outcome)) {
                row.enabled=read_from_directory(directory,"enabled",4096,false,operations,time);
                const auto& outcome=row.enabled.outcome;
                if(outcome.primary && outcome.primary->stage==Stage::open &&
                   outcome.primary->error_number==ENOENT && outcome.cleanup_count==0) {
                    // Size directories cover both anonymous and shmem THP. Only
                    // a positively identified shmem interface explains an absent
                    // anonymous leaf; other missing/read/identity errors remain.
                    struct stat shmem{},anonymous{};
                    if(operations.files.stat_at(operations.files.context,directory,"shmem_enabled",&shmem)==0 &&
                       S_ISREG(shmem.st_mode) && shmem.st_nlink==1 &&
                       operations.files.stat_at(operations.files.context,directory,"enabled",&anonymous)<0 && errno==ENOENT) {
                        shmem_only=true;
                    }
                }
            }
            size.close();
        }
        // The directory owner refers to this row's outcome and must be gone
        // before exclusion. A failed consuming close keeps the original missing
        // leaf and its ordered cleanup fact in the failed candidate row.
        if(shmem_only && row.enabled.outcome.cleanup_count==0) {
            result.rows.pop_back();continue;
        }
        if(!row.enabled.outcome.succeeded()) {
            fail(result.outcome,*row.enabled.outcome.primary);break;
        }
    }
    root.close();
    return result;
}
auto read_process_thp(HostOperations operations,ObservationTime time) -> ProcessThp {
    ProcessThp result;
    if(!operations.process_thp || !time.record) {
        fail(result.outcome,failure(Stage::admission));return result;
    }
    try {
        auto value=operations.process_thp(operations.context);
        if(!value) fail(result.outcome,std::move(value.error()));
        else { result.value=*value;observe(time,result.outcome); }
    } catch(...) { fail(result.outcome,failure(Stage::process_thp)); }
    return result;
}
auto read_page_faults(LinuxThreadId tid,detail::BoundedTaskStatOperations operations,
    ObservationTime time) noexcept -> PageFaultRead {
    PageFaultRead result;
    if(!operations.open || !operations.read || !operations.close || !time.record) {
        fail(result.outcome,failure(Stage::admission));return result;
    }
    struct Forward {
        detail::BoundedTaskStatOperations operations;
        ObservationTime time;
        Outcome* outcome;
        std::size_t reads{};
        bool read_failed{};
    } forward{operations,time,&result.outcome};
    detail::BoundedTaskStatOperations wrapped{
        &forward,
        [](void* p,LinuxThreadId thread) noexcept {
            auto& f=*static_cast<Forward*>(p);
            return f.operations.open(f.operations.context,thread);
        },
        [](void* p,int fd,void* buffer,std::size_t count) noexcept {
            auto& f=*static_cast<Forward*>(p);
            using Integer=detail::BoundedTaskStatOperations::IntegerResult;
            if(f.reads==16385) { f.read_failed=true;return Integer{-1,EOVERFLOW}; }
            ++f.reads;
            auto value=f.operations.read(f.operations.context,fd,buffer,count);
            if(value.value>0 && static_cast<unsigned long>(value.value)>count) {
                f.read_failed=true;return Integer{-1,EOVERFLOW};
            }
            if(value.value<0 && value.error_number!=EINTR) f.read_failed=true;
            return value;
        },
        [](void* p,int fd) noexcept {
            auto& f=*static_cast<Forward*>(p);
            // Candidate timestamp is acquired after bytes, before retirement;
            // only the existing parser can validate the resulting observation.
            if(!f.read_failed) observe(f.time,*f.outcome);
            return f.operations.close(f.operations.context,fd);
        },
        [](void* p) {
            auto& f=*static_cast<Forward*>(p);
            if(f.operations.before) f.operations.before(f.operations.context);
        }};
    std::array<char,16384> workspace{};
    auto read=detail::TaskStatAccess::read_bounded(tid,workspace,wrapped);
    if(!read) {
        result.source_failure=read.error();
        const bool close=read.error().primary.kind==detail::BoundedTaskStatFailureKind::close_failed;
        fail(result.outcome,failure(close ? Stage::close : Stage::read,read.error().primary.error_number),close);
        if(read.error().cleanup)
            fail(result.outcome,failure(Stage::close,read.error().cleanup->error_number),true);
    } else if(result.outcome.succeeded()) result.counters=*read;
    return result;
}
auto read_temperature(detail::BoundedSysfsOperations operations,ObservationTime time,SourceReadLimits limits)
    noexcept -> TemperatureRead {
    TemperatureRead result;
    if(!operations.open_root || !operations.open_at || !operations.stat_fd ||
       !operations.stat_at || !operations.next || !operations.read || !operations.close || !time.record ||
       limits.bytes>16*mib) {
        fail(result.outcome,failure(Stage::admission));return result;
    }
    const auto shape=detail::bounded_hwmon_storage_shape({128,256,4096});
    if(!shape) { fail(result.outcome,failure(Stage::admission));return result; }
    const auto calls=16*mib+shape->operation_capacity;
    SourceForward forward{operations,0,0,0,std::min(calls,limits.calls.value_or(calls)),false,limits.bytes};
    auto prepared=detail::PreparedBoundedHwmonDiscoveryStorage::prepare({128,256,4096},forward.forwarding_operations());
    if(!prepared) result.source_failure=prepared.error();
    else {
        auto discovered=detail::BoundedHwmonTemperatureSource::discover_prepared(std::move(*prepared));
        if(!discovered) {
            result.source_failure=detail::PreparedBoundedHwmonDiscoveryAccess::sequence(discovered.error());
            const auto cleanup=detail::PreparedBoundedHwmonDiscoveryAccess::cleanup(discovered.error());
            if(cleanup) std::ranges::copy(*cleanup,result.source_cleanup.begin());
        } else if(!*discovered) { result.evaluated=true;observe(time,result.outcome); }
        else {
            std::array<char,4097> workspace{};
            auto value=(**discovered).read(workspace,result.source_cleanup);
            if(!value) result.source_failure=value.error();
            else {
                result.millicelsius=*value;result.evaluated=true;
                observe(time,result.outcome);result.supported=result.outcome.succeeded();
            }
            result.retirement_count=(**discovered).retire(result.retirement);
        }
    }
    if(result.source_failure) fail(result.outcome,failure(Stage::read,result.source_failure->primary.native_error));
    for(const auto& close:result.retirement)
        if(close.attempted && close.kind!=detail::BoundedSysfsDispositionKind::success)
            fail(result.outcome,failure(Stage::close,close.native_error));
    if(!result.outcome.succeeded()) result.supported=false;
    result.source_bytes=forward.bytes;
    return result;
}
auto read_frequency(std::span<const core::LinuxCpuId> present,
    std::span<const core::LinuxCpuId> selected,detail::BoundedSysfsOperations operations,
    ObservationTime time,SourceReadLimits limits) -> FrequencyRead {
    FrequencyRead result;
    if(!operations.open_root || !operations.open_at || !operations.stat_fd ||
       !operations.stat_at || !operations.next || !operations.read || !operations.close ||
       !time.record || limits.bytes>16*mib || present.empty() || present.size()>1024 || selected.size()>1024 ||
       std::ranges::any_of(present,[](auto cpu) { return cpu.value()>4095; }) ||
       std::ranges::any_of(selected,[](auto cpu) { return cpu.value()>4095; })) {
        fail(result.outcome,failure(Stage::admission));return result;
    }
    constexpr auto calls=16*mib+131076;
    SourceForward forward{operations,0,0,0,std::min(calls,limits.calls.value_or(calls)),true,limits.bytes};
    const auto wrapped=forward.forwarding_operations();
    result.domains.resize(1024);
    for(auto& domain:result.domains) { domain.driver.reserve(4096);domain.governor.reserve(4096); }
    result.memberships.reserve(16384);result.source_cleanup.reserve(8193);result.retirement.resize(8193);
    std::array<detail::BoundedCpuFrequencySourceFailure,3> read_cleanup{};
    std::array<char,4097> workspace{};
    auto prepared=detail::PreparedBoundedCpuFrequencyDiscoveryStorage::prepare(
        {4095,4097,4096,16384,4096},present,selected,wrapped);
    std::size_t domains{};bool retired{};
    if(!prepared) result.source_failure=prepared.error();
    else {
        auto discovered=detail::BoundedCpuFrequencySources::discover_prepared(std::move(*prepared));
        if(!discovered) {
            result.source_failure=detail::PreparedBoundedCpuFrequencyDiscoveryAccess::sequence(discovered.error());
            const auto cleanup=detail::PreparedBoundedCpuFrequencyDiscoveryAccess::cleanup(discovered.error());
            if(cleanup) result.source_cleanup.assign(cleanup->begin(),cleanup->end());
        } else if(!*discovered) {
            result.evaluated=true;observe(time,result.outcome);
        } else {
            auto& source=**discovered;
            const auto available=source.domain_count();
            if(available>1024) fail(result.outcome,failure(Stage::admission));
            else for(std::size_t index=0;index<available;++index) {
                auto& target=result.domains[index];
                const auto members=source.related_cpus(index);
                if(members.size()>16384-result.memberships.size()) {
                    fail(result.outcome,failure(Stage::admission));break;
                }
                target.membership_offset=result.memberships.size();target.membership_count=members.size();
                result.memberships.insert(result.memberships.end(),members.begin(),members.end());
                target.kind=source.kind(index);target.driver=source.driver(index);target.governor=source.governor(index);
                target.minimum_hz=source.minimum_hz(index);target.maximum_hz=source.maximum_hz(index);
                ++domains;
                auto current=source.read(index,workspace,read_cleanup);
                if(!current) {
                    result.source_failure=current.error();
                    result.source_cleanup.assign(read_cleanup.begin(),read_cleanup.begin()+static_cast<std::ptrdiff_t>(current.error().cleanup_count));
                    break;
                }
                target.current_hz=*current;observe(time,result.outcome);
                if(!result.outcome.succeeded()) break;
            }
            result.evaluated=!result.source_failure && result.outcome.succeeded();
            result.supported=result.evaluated && std::ranges::all_of(selected,[&](auto cpu) {
                return std::ranges::find(result.memberships,cpu)!=result.memberships.end();
            });
            result.retirement.resize(source.retire(result.retirement));
            retired=true;
        }
    }
    result.domains.resize(domains);
    if(result.source_failure) fail(result.outcome,failure(Stage::read,result.source_failure->primary.native));
    // Full source cleanup and retirement vectors remain authoritative; never
    // truncate them into the small primitive Outcome cleanup array.
    for(const auto& close:result.retirement)
        if(close.attempted && close.kind!=detail::BoundedSysfsDispositionKind::success)
            fail(result.outcome,failure(Stage::close,close.native_error));
    if(!result.outcome.succeeded()) result.supported=false;
    if(!retired) result.retirement.clear();
    result.discovered_policies=forward.policies;result.source_bytes=forward.bytes;
    return result;
}
auto read_cpu_identity(core::LinuxCpuId cpu,HostOperations operations,ObservationTime time) -> CpuIdentity {
    CpuIdentity result;
    if (!operations.affinity || !operations.set_affinity || !operations.cpuid_leaf1_eax || !time.record) {
        fail(result.outcome,failure(Stage::admission));return result;
    }
    std::optional<core::CpuSet> original;
    try {
        auto value=operations.affinity(operations.context);
        if(!value) { fail(result.outcome,{Stage::affinity_save,0,std::move(value.error())});return result; }
        original=std::move(*value);
    } catch(...) { fail(result.outcome,failure(Stage::affinity_save));return result; }
    try {
        auto bound=operations.set_affinity(operations.context,core::CpuSet{cpu});
        if (!bound) fail(result.outcome,{Stage::affinity_set,0,std::move(bound.error())});
        else {
            auto value=operations.cpuid_leaf1_eax(operations.context);
            if (!value) fail(result.outcome,std::move(value.error()));
            else { result.eax=*value;observe(time,result.outcome); }
        }
    } catch (...) { fail(result.outcome,failure(Stage::cpuid)); }
    try {
        auto restored=operations.set_affinity(operations.context,*original);
        if (!restored) fail(result.outcome,{Stage::affinity_restore,0,std::move(restored.error())},true);
    } catch (...) { fail(result.outcome,failure(Stage::affinity_restore),true); }
    return result;
}
auto check_perf(LinuxThreadId tid,std::size_t page,detail::BoundedPerfContextSwitchOperations ops,ObservationTime time)
    noexcept -> PerfCapability {
    PerfCapability result;
    if (!ops.open || !ops.map || !ops.unmap || !ops.close || !time.record || !page || page>65536 || (page&(page-1))) {
        fail(result.outcome,failure(Stage::admission));return result;
    }
    const auto descriptor=ops.open(ops.context,tid);
    if(descriptor.value<0) { fail(result.outcome,failure(Stage::open,descriptor.error_number));return result; }
    const auto length=page*9;
    const auto mapped=ops.map(ops.context,descriptor.value,length);
    if(!mapped.value || mapped.value==MAP_FAILED) fail(result.outcome,failure(Stage::perf_map,mapped.error_number));
    else {
        if(ops.before_metadata) ops.before_metadata(ops.context);
        constexpr auto record_bytes=sizeof(perf_event_header)+sizeof(std::uint64_t);
        const auto metadata=detail::validate_bounded_perf_metadata(mapped.value,length,page*8,page*8/record_bytes);
        if(!metadata) fail(result.outcome,failure(Stage::perf_metadata));
        else { result.observed=true;observe(time,result.outcome); }
        const auto unmapped=ops.unmap(ops.context,mapped.value,length);
        if(unmapped.value!=0) fail(result.outcome,failure(Stage::perf_unmap,unmapped.error_number),true);
    }
    const auto closed=ops.close(ops.context,descriptor.value);
    if(closed.value!=0) fail(result.outcome,failure(Stage::close,closed.error_number),true);
    return result;
}
auto native_file_operations() noexcept -> FileOperations {
    return {detail::production_bounded_sysfs_operations(),[](void*) noexcept -> std::uint64_t { return ::geteuid(); }};
}
auto native_host_operations() noexcept -> HostOperations {
    return {nullptr,
        [](void*) { return get_current_thread_affinity(); },
        [](void*,const core::CpuSet& cpus) { return set_current_thread_affinity(cpus); },
        [](void*) -> std::expected<std::uint32_t,Failure> {
            unsigned eax{},ebx{},ecx{},edx{};
            if(!__get_cpuid(1,&eax,&ebx,&ecx,&edx)) return std::unexpected{failure(Stage::cpuid)};
            return eax;
        },
        [](void*) -> std::expected<std::array<std::string,3>,Failure> {
            struct utsname value{};
            if(::uname(&value)!=0) return std::unexpected{failure(Stage::kernel_identity,errno)};
            return std::array<std::string,3>{value.sysname,value.release,value.version};
        },
        [](void*) -> std::expected<std::uint64_t,Failure> {
            const auto value=::prctl(PR_GET_THP_DISABLE,0,0,0,0);
            if(value<0) return std::unexpected{failure(Stage::process_thp,errno)};
            return static_cast<std::uint64_t>(value);
        }};
}
}
