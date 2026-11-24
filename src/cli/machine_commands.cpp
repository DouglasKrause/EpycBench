#include "cli/machine_commands.hpp"
#include "platform_linux/presentation_files.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <map>
#include <ostream>
#include <sstream>
#include <tuple>
#include <vector>

namespace epycbench::cli {
namespace {
namespace files=platform_linux::presentation_files;
constexpr std::string_view saved_name="machine-info/machine.json";

auto timestamp(std::int64_t seconds) -> std::string {
    using namespace std::chrono;
    const sys_seconds time{std::chrono::seconds{seconds}};
    const auto day=floor<days>(time);
    const year_month_day date{day};
    const hh_mm_ss clock{time-day};
    std::ostringstream output;
    output << std::setfill('0') << std::setw(4) << static_cast<int>(date.year())
           << '-' << std::setw(2) << static_cast<unsigned>(date.month())
           << '-' << std::setw(2) << static_cast<unsigned>(date.day())
           << ' ' << std::setw(2) << clock.hours().count()
           << ':' << std::setw(2) << clock.minutes().count()
           << ':' << std::setw(2) << clock.seconds().count() << " UTC";
    return output.str();
}

auto size_text(std::uint64_t bytes) -> std::string {
    std::ostringstream output;
    const char* unit="B";
    double value=static_cast<double>(bytes);
    for(const auto* next:{"KiB","MiB","GiB","TiB"}) {
        if(value<1024)break;
        value/=1024;
        unit=next;
    }
    output << std::fixed << std::setprecision(value<10&&bytes>=1024?1:0) << value << ' ' << unit;
    return output.str();
}

auto clipped(std::string text,std::size_t width) -> std::string {
    for(auto& character:text)
        if(static_cast<unsigned char>(character)<32||static_cast<unsigned char>(character)>126)
            character='?';
    if(text.size()>width) {
        if(width>3)text=text.substr(0,width-3)+"...";
        else text.resize(width);
    }
    return text;
}

auto failed(std::ostream& error,std::string_view message,int status=3) -> int {
    error << message << '\n';
    return status;
}
} // namespace

auto render_machine_architecture(const topology::TopologySnapshot& snapshot,
    std::string_view inspected_at,std::string_view saved_path,std::size_t columns,std::size_t rows)
    -> std::string {
    columns=std::clamp(columns,std::size_t{1},std::size_t{80});
    rows=std::clamp(rows,std::size_t{1},std::size_t{24});
    const auto& machine=snapshot.machine();
    std::vector<std::string> lines{
        "Machine architecture",
        "Inspected: "+std::string(inspected_at),
        "CPU: "+(machine.cpu_model.value()?*machine.cpu_model.value():std::string{"not reported"}),
        "Sockets: "+std::to_string(snapshot.packages().size())+
            "   Cores: "+std::to_string(snapshot.cores().size())+
            "   Hardware threads: "+std::to_string(snapshot.processing_units().size()),
        "Available threads: "+(machine.effective_cpus.value()?
            std::to_string(machine.effective_cpus.value()->cardinality()):std::string{"not reported"})+
            "   Architecture: "+machine.architecture,
        "Memory: "+(machine.total_memory.value()?size_text(machine.total_memory.value()->value()):
            std::string{"not reported"})+
            "   NUMA nodes: "+std::to_string(snapshot.numa_nodes().size())};
    using Key=std::tuple<std::uint32_t,topology::CacheKind,std::uint64_t>;
    std::map<Key,std::size_t> caches;
    for(const auto& cache:snapshot.caches())++caches[{cache.level,cache.kind,cache.size.value()}];
    if(caches.empty())lines.emplace_back("Caches: not reported");
    for(const auto& [key,count]:caches) {
        const auto [level,kind,bytes]=key;
        const auto* name=kind==topology::CacheKind::data?"data":
            kind==topology::CacheKind::instruction?"instruction":
            kind==topology::CacheKind::unified?"unified":"unknown";
        lines.emplace_back("L"+std::to_string(level)+" "+name+": "+std::to_string(count)+
            " x "+size_text(bytes));
    }
    for(const auto& node:snapshot.numa_nodes())
        lines.emplace_back("NUMA "+(node.linux_node_id.value()?std::to_string(*node.linux_node_id.value()):
            std::string{"?"})+": "+std::to_string(node.member_pus.cardinality())+" threads, "+
            (node.installed_memory.value()?size_text(node.installed_memory.value()->value()):
                std::string{"memory not reported"}));
    // Reserve a final row for the file location and, when needed, an omission notice.
    const auto detail_budget=rows>1?rows-1:0;
    const bool omitted=lines.size()>detail_budget;
    const auto count=omitted&&detail_budget?detail_budget-1:std::min(lines.size(),detail_budget);
    std::ostringstream output;
    for(std::size_t index{};index<count;++index)output << clipped(lines[index],columns) << '\n';
    if(omitted&&detail_budget)
        output << clipped(std::to_string(lines.size()-count)+" more detail lines in the saved file.",columns) << '\n';
    output << clipped("Full details: "+std::string(saved_path),columns) << '\n';
    return output.str();
}

auto inspect_machine(topology::cli::Environment& environment,std::ostream& output,std::ostream& error) -> int {
    try {
        auto directory=files::ensure_private_subdirectory(".","machine-info");
        if(!directory)return failed(error,"Machine inspection: could not prepare machine-info.");
        std::vector<std::string> words{"epycbench","topology","export","--live","--output",
            (std::filesystem::path{*directory}/"machine.json").string(),"--force"};
        std::vector<char*> arguments;
        for(auto& word:words)arguments.push_back(word.data());
        std::ostringstream details;
        const auto status=topology::cli::run(static_cast<int>(arguments.size()),arguments.data(),
            environment,details,error);
        if(status)return status;
        output << "Machine inspection saved: " << words[5] << '\n';
        return output?0:3;
    } catch(...) {return failed(error,"Machine inspection failed.",1);}
}

auto view_machine(topology::cli::Environment& environment,std::ostream& output,std::ostream& error) -> int {
    try {
        auto status=files::file_status(std::filesystem::path{saved_name});
        if(!status)return failed(error,"Machine architecture: could not inspect the saved file.");
        if(!status->exists) {
            std::ostringstream saved;
            const auto result=inspect_machine(environment,saved,error);
            if(result)return result;
            status=files::file_status(std::filesystem::path{saved_name});
            if(!status||!status->exists)return failed(error,"Machine architecture: inspection was not saved.");
        }
        if(!environment.read_bounded_file||!environment.deserialize)
            return failed(error,"Machine architecture: reader unavailable.",1);
        auto bytes=environment.read_file(std::filesystem::path{saved_name});
        if(!bytes)return failed(error,"Machine architecture: could not read the saved inspection.");
        auto loaded=environment.deserialize(*bytes,topology::TopologyJsonInputPolicy::require_canonical);
        if(!loaded)return failed(error,"Machine architecture: saved inspection is invalid; inspect again to refresh it.",5);
        const auto dimensions=files::terminal_dimensions();
        output << render_machine_architecture(loaded->topology,timestamp(status->modified_seconds),saved_name,
            dimensions.columns,dimensions.rows>2?dimensions.rows-2:1);
        return output?0:3;
    } catch(...) {return failed(error,"Machine architecture view failed.",1);}
}

} // namespace epycbench::cli
