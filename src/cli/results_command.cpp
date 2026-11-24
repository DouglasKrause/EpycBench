#include "cli/results_command.hpp"
#include "cli/json_csv.hpp"
#include "cli/run_energy.hpp"

#include "epycbench/kernel/package.h"
#include "results/package_result.hpp"
#include "results/result_export.hpp"
#include "platform_linux/presentation_files.hpp"
#include <CLI/CLI.hpp>
#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <numeric>
#include <ostream>
#include <sstream>
#include <string_view>
#include <variant>

namespace epycbench::cli {
namespace {
constexpr std::size_t document_limit = results_document_input_bytes;
constexpr std::size_t maximum_display_observations = 4096;
constexpr std::string_view help =
    "Usage: epycbench result validate FILE\n"
    "Usage: epycbench result energy FILE [--estimate [--max-power-w W] [--max-reporting-delay-ms MS] [--reset-behavior zero|unknown]]\n"
    "Usage: epycbench result index DIRECTORY\n"
    "Usage: epycbench result replay FILE\n"
    "Usage: epycbench result fingerprint FILE [--pair N] [--package-energy]\n"
    "Usage: epycbench result compare BASELINE CANDIDATE [--baseline-pair N] [--candidate-pair N]"
    " [--baseline-variant first|second] [--candidate-variant first|second]\n"
    "Usage: epycbench result export --profile {complete,shareable,summary,observation-table} [--format json|csv] FILE\n"
    "Export validates once. Complete retains all evidence; derived profiles are reduced, linkable, not anonymous.\n"
    "Exports save beside the result in exports/; redirected output contains only the selected format.\n"
    "JSON is the default. CSV converts the same JSON into path,type,value rows.\n"
    "Offline, read-only validation of a canonical EpycBench Results document.\n"
    "Validates saved package Results without loading the original modules.\n"
    "A valid document does not imply successful execution or hardware qualification.\n";

enum class Diagnostic { usage, input_io, limit_exceeded, unsupported_profile, invalid_document, output_io, internal_failure };

// Convert a decimal CLI quantity to integral base units without a floating-point round trip.
auto scaled_quantity(std::string_view text, std::size_t decimal_places) -> std::optional<std::uint64_t> {
    const auto point = text.find('.');
    const auto whole = text.substr(0, point);
    auto fraction = point == std::string_view::npos ? std::string_view{} : text.substr(point + 1U);
    if (whole.empty() && fraction.empty()) return {};
    while (fraction.ends_with('0')) fraction.remove_suffix(1U);
    if (fraction.size() > decimal_places) return {};
    std::string digits{whole};
    digits.append(fraction);
    digits.append(decimal_places - fraction.size(), '0');
    std::uint64_t value{};
    const auto end = digits.data() + digits.size();
    const auto parsed = std::from_chars(digits.data(), end, value);
    if (parsed.ec != std::errc{} || parsed.ptr != end) return {};
    return value;
}

auto message(Diagnostic diagnostic) noexcept -> std::string_view {
    switch (diagnostic) {
    case Diagnostic::usage: return "result validate: failed\ncode: results.cli.usage\n";
    case Diagnostic::input_io: return "result validate: failed\ncode: results.cli.input_io\n";
    case Diagnostic::limit_exceeded: return "result validate: failed\ncode: results.cli.limit_exceeded\n";
    case Diagnostic::unsupported_profile: return "result validate: failed\ncode: results.cli.unsupported_profile\n";
    case Diagnostic::invalid_document: return "result validate: failed\ncode: results.cli.invalid_document\n";
    case Diagnostic::output_io: return "result validate: failed\ncode: results.cli.output_io\n";
    case Diagnostic::internal_failure: return "result validate: failed\ncode: results.cli.internal_failure\n";
    }
    return "result validate: failed\ncode: results.cli.internal_failure\n";
}

auto export_message(Diagnostic diagnostic) noexcept -> std::string_view {
    switch (diagnostic) {
    case Diagnostic::usage: return "result export: failed\ncode: results.export.usage\n";
    case Diagnostic::input_io: return "result export: failed\ncode: results.export.input_io\n";
    case Diagnostic::limit_exceeded: return "result export: failed\ncode: results.export.limit_exceeded\n";
    case Diagnostic::unsupported_profile: return "result export: failed\ncode: results.export.unsupported_profile\n";
    case Diagnostic::invalid_document: return "result export: failed\ncode: results.export.invalid_document\n";
    case Diagnostic::output_io: return "result export: failed\ncode: results.export.output_io\n";
    case Diagnostic::internal_failure: return "result export: failed\ncode: results.export.internal_failure\n";
    }
    return "result export: failed\ncode: results.export.internal_failure\n";
}

auto fail(std::ostream& error, int status, Diagnostic diagnostic, bool exporting = false,
          std::string_view saved_command = {}) noexcept -> int {
    const auto text = exporting ? export_message(diagnostic) : message(diagnostic);
    try {
        if (!saved_command.empty()) {
            constexpr auto prefix = std::string_view{"result validate: failed\ncode: results.cli."};
            const auto rendered = "result " + std::string{saved_command} + ": failed\ncode: results." +
                std::string{saved_command} + "." + std::string{text.substr(prefix.size())};
            error.write(rendered.data(), static_cast<std::streamsize>(rendered.size()));
        } else error.write(text.data(), static_cast<std::streamsize>(text.size()));
    } catch (...) {
        // Diagnostics cannot replace the already selected primary failure.
    }
    return status;
}

auto emit(std::ostream& output, std::ostream& error, std::string_view text, bool exporting = false,
          std::string_view saved_command = {}) -> int {
    try {
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (output) return 0;
    } catch (...) {
    }
    return fail(error, 3, Diagnostic::output_io, exporting, saved_command);
}

auto read_validated_result(const std::filesystem::path& path, topology::cli::Environment& environment)
    -> std::expected<results::PackageResult, Diagnostic> {
    auto bytes = environment.read_bounded_file(path, document_limit);
    if (!bytes) return std::unexpected{bytes.error().code == "topology.serialization.limit_exceeded"
        ? Diagnostic::limit_exceeded : Diagnostic::input_io};
    if (bytes->size() > document_limit) return std::unexpected{Diagnostic::limit_exceeded};
    auto document = results::validate_package_result(*bytes);
    if (!document) return std::unexpected{Diagnostic::invalid_document};
    return std::move(*document);
}

auto read_status(Diagnostic diagnostic) noexcept -> int {
    if (diagnostic == Diagnostic::internal_failure) return 1;
    if (diagnostic == Diagnostic::input_io) return 3;
    return 9;
}

using HistoryJson = nlohmann::json;

struct HistoricalSource {
    std::filesystem::path path;
    std::size_t pair{};
    std::string variant{"second"};

};

#include "cli/package_result_presentation.inc"

auto compare_saved_results(const HistoricalSource& before, const HistoricalSource& after,
    topology::cli::Environment& environment, std::ostream& output, std::ostream& error) -> int {
    auto baseline = read_validated_result(before.path, environment);
    auto candidate = read_validated_result(after.path, environment);
    if (!baseline) return fail(error, read_status(baseline.error()), baseline.error(), false, "compare");
    if (!candidate) return fail(error, read_status(candidate.error()), candidate.error(), false, "compare");
    const auto report = compare_package_results(&*baseline, &*candidate, before, after);
    return emit(output, error, report.dump() + '\n', false, "compare");
}

template<class Result>
auto read_package_energy_context(const std::filesystem::path& path, const Result& document,
    std::string_view command, std::ostream& error) -> std::expected<HistoryJson, int> {
    const auto evidence = run_energy::load(path, document);
    if (evidence) return HistoryJson::parse(*evidence);
    const auto& problem = evidence.error();
    if (problem.operation == "open_file" && problem.os_error && problem.os_error->value == ENOENT)
        return HistoryJson(nullptr);
    error << "result " << command << ": failed\ncode: " << problem.code.value() << '\n';
    return std::unexpected{9};
}

auto fingerprint_saved_result(const std::filesystem::path& path, std::optional<std::size_t> pair, bool include_energy,
    topology::cli::Environment& environment, std::ostream& output, std::ostream& error) -> int {
    auto document = read_validated_result(path, environment);
    if (!document) return fail(error, read_status(document.error()), document.error(), false, "fingerprint");
    {
        const auto* package = &*document;
        std::optional<HistoryJson> context;
        if (include_energy) {
            auto energy = read_package_energy_context(path, *package, "fingerprint", error);
            if (!energy) return energy.error();
            context = energy->is_null() ? no_package_energy() : std::move(*energy);
        }
        return emit(output, error, package_fingerprint(*package, path, pair, std::move(context)).dump() + '\n',
                    false, "fingerprint");
    }
}

auto index_saved_results(const std::filesystem::path& directory, topology::cli::Environment& environment,
    std::ostream& output, std::ostream& error) -> int {
    constexpr std::size_t maximum_directory_entries = 4096U;
    auto names = platform_linux::presentation_files::regular_leaf_names(
        directory, ".epycbench.json", maximum_directory_entries);
    if (!names) {
        const auto diagnostic = names.error().error_number == EFBIG ? Diagnostic::limit_exceeded : Diagnostic::input_io;
        return fail(error, read_status(diagnostic), diagnostic, false, "index");
    }
    auto rows = HistoryJson::array();
    for (const auto& name : *names) {
        auto row = HistoryJson{{"artifact", name}};
        auto document = read_validated_result(directory / name, environment);
        if (!document) {
            switch (document.error()) {
            case Diagnostic::limit_exceeded: row["validation"] = "limit_exceeded"; break;
            case Diagnostic::unsupported_profile: row["validation"] = "unsupported_profile"; break;
            case Diagnostic::invalid_document: row["validation"] = "invalid_document"; break;
            case Diagnostic::input_io: row["validation"] = "input_io"; break;
            default: return fail(error, 1, Diagnostic::internal_failure, false, "index");
            }
        } else {
            row = package_source(*document, name);
            row["validation"] = "valid";
        }
        rows.push_back(std::move(row));
    }
    const auto report = HistoryJson{{"schema", "epycbench.result-index/v1"}, {"entries", std::move(rows)}};
    return emit(output, error, report.dump(-1, ' ', false, HistoryJson::error_handler_t::replace) + '\n', false, "index");
}

auto write_results_export(std::string_view bytes, results::result_export::Profile profile,
    const std::filesystem::path& source, std::ostream& output, std::ostream& error, ExportFormat format, bool save,
    const PackageResult& package,
    bool present = true) -> int {
    namespace files=platform_linux::presentation_files;
    try {
        if (bytes.size() > document_limit) return fail(error, 9, Diagnostic::limit_exceeded, true);
        // Both formats use the same JSON projection and the same save/emit path.
        std::string csv;
        std::string_view output_bytes=bytes;
        if(format==ExportFormat::csv) {
            auto converted=json_to_csv(bytes);
            if(!converted) {
                const bool limit=converted.error()==CsvError::limits;
                return fail(error,limit?9:1,limit?Diagnostic::limit_exceeded:Diagnostic::internal_failure,true);
            }
            csv=std::move(*converted);output_bytes=csv;
        }
        if(!save)return emit(output,error,output_bytes,true);
        const auto parent=source.has_parent_path()?source.parent_path():std::filesystem::path{"."};
        auto directory=files::ensure_private_subdirectory(parent,"exports");
        if(!directory)return fail(error,3,Diagnostic::output_io,true);
        auto filename=source.stem().string()+"."+std::string(results::result_export::name(profile))+
            (format==ExportFormat::csv?".csv":".json");
        auto saved=files::save_numbered(*directory,filename,output_bytes);
        if(!saved)return fail(error,3,Diagnostic::output_io,true);
        if (!present) return 0;
        output << "Export saved: " << *saved << '\n';
        if (profile == ExportProfile::summary) {
            output << render_package_performance_summary(package);
        }
        return output?0:3;
    } catch(...) {
        return fail(error,save?3:1,save?Diagnostic::output_io:Diagnostic::internal_failure,true);
    }
}

auto output_results_export(const PackageResult& result, ExportProfile profile, const std::filesystem::path& source,
    std::ostream& output, std::ostream& error, ExportFormat format, bool save, bool present = true) -> int {
    try {
        const auto bytes = profile == ExportProfile::complete ? result.document.dump()
            : project_package_export(result, profile).dump();
        return write_results_export(bytes, profile, source, output, error, format, save, result, present);
    } catch (...) {
        return fail(error, save ? 3 : 1, save ? Diagnostic::output_io : Diagnostic::internal_failure, true);
    }
}
} // namespace

auto save_results_export(const results::PackageResult& document, const std::filesystem::path& source,
    std::ostream& output, std::ostream& error, bool present) -> int {
    return output_results_export(document, ExportProfile::summary, source, output, error, ExportFormat::json, true, present);
}

auto run_results_command(int argc, char* argv[], topology::cli::Environment& environment,
                         std::ostream& output, std::ostream& error, bool save_export) -> int {
    const bool exporting = argc > 2 && argv && argv[2] && std::string_view{argv[2]} == "export";
    const auto command = argc > 2 && argv && argv[2] ? std::string_view{argv[2]} : std::string_view{};
    const auto saved_command = command == "compare" || command == "index" ||
        command == "fingerprint" || command == "replay" ? command : std::string_view{};
    const auto reject = [&](int status, Diagnostic diagnostic) { return fail(error, status, diagnostic, exporting, saved_command); };
    try {
        CLI::App app{"Offline Results validation, export and comparison", "epycbench"};
        app.require_subcommand(1, 1);
        auto* result = app.add_subcommand("result");
        result->require_subcommand(1, 1);
        auto* validate = result->add_subcommand("validate");
        auto* energy = result->add_subcommand("energy");
        auto* export_command = result->add_subcommand("export");
        auto* compare = result->add_subcommand("compare");
        auto* index = result->add_subcommand("index");
        auto* fingerprint = result->add_subcommand("fingerprint");
        auto* replay = result->add_subcommand("replay");
        std::filesystem::path fingerprint_path;
        std::size_t fingerprint_pair{};
        bool fingerprint_energy{};
        fingerprint->add_option("FILE", fingerprint_path)->required();
        fingerprint->add_option("--pair", fingerprint_pair)->expected(1);
        fingerprint->add_flag("--package-energy", fingerprint_energy);
        std::filesystem::path directory;
        index->add_option("DIRECTORY", directory)->required();
        HistoricalSource before, after;
        compare->add_option("BASELINE", before.path)->required();
        compare->add_option("CANDIDATE", after.path)->required();
        compare->add_option("--baseline-pair", before.pair)->expected(1);
        compare->add_option("--candidate-pair", after.pair)->expected(1);
        compare->add_option("--baseline-variant", before.variant)->check(CLI::IsMember({"first", "second"}))->expected(1);
        compare->add_option("--candidate-variant", after.variant)->check(CLI::IsMember({"first", "second"}))->expected(1);
        std::string path, selected_profile, selected_format{"json"};
        validate->add_option("FILE", path)->required();
        energy->add_option("FILE", path)->required();
        bool estimate_energy{};
        std::string power_w{"1000"}, delay_ms{"100"}, reset_behavior{"zero"};
        auto* estimate_flag = energy->add_flag("--estimate", estimate_energy);
        energy->add_option("--max-power-w", power_w)->needs(estimate_flag);
        energy->add_option("--max-reporting-delay-ms", delay_ms)->needs(estimate_flag);
        energy->add_option("--reset-behavior", reset_behavior)
            ->check(CLI::IsMember({"zero", "unknown"}))->needs(estimate_flag);
        replay->add_option("FILE", path)->required();
        export_command->add_option("FILE", path)->required();
        export_command->add_option("--profile", selected_profile)->required()->expected(1);
        export_command->add_option("--format", selected_format)->expected(1);

        try {
            app.parse(argc, argv);
        } catch (const CLI::ParseError& exception) {
            if (exception.get_exit_code() == 0) return emit(output, error, help, exporting, saved_command);
            return reject(2, Diagnostic::usage);
        }
        if (*fingerprint) {
            if (fingerprint_path.empty() || fingerprint_path == "-") return reject(2, Diagnostic::usage);
            return fingerprint_saved_result(fingerprint_path, fingerprint->count("--pair")
                ? std::optional{fingerprint_pair} : std::nullopt, fingerprint_energy, environment, output, error);
        }
        if (*index) {
            if (directory.empty() || directory == "-") return reject(2, Diagnostic::usage);
            return index_saved_results(directory, environment, output, error);
        }
        if (*compare) {
            if (before.path.empty() || after.path.empty() || before.path == "-" || after.path == "-")
                return reject(2, Diagnostic::usage);
            return compare_saved_results(before, after, environment, output, error);
        }
        const auto selected = results::result_export::profile(selected_profile);
        if ((!*validate && !*energy && !*replay && !*export_command) || path.empty() || path == "-" ||
            (*export_command && (!selected || export_command->count("--profile") != 1 ||
                export_command->count("--format") > 1 ||
                (selected_format != "json" && selected_format != "csv"))))
            return reject(2, Diagnostic::usage);
        std::optional<run_energy::EstimateOptions> energy_options;
        if (*energy && estimate_energy) {
            const auto power_mw = scaled_quantity(power_w, 3);
            const auto delay_ns = scaled_quantity(delay_ms, 6);
            if (!power_mw || !delay_ns) return reject(2, Diagnostic::usage);
            energy_options.emplace(*power_mw, *delay_ns, reset_behavior == "zero"
                ? run_energy::ResetModel::accumulation_or_reset_to_zero : run_energy::ResetModel::unknown);
        }
        auto document = read_validated_result(path, environment);
        if (!document) return reject(read_status(document.error()), document.error());
        if (*validate) return emit(output, error, "result validate: valid\n");
        const auto format = selected_format == "csv" ? ExportFormat::csv : ExportFormat::json;
        {
        const auto* package = &*document;
            if (*replay) {
                const auto timeline = package_replay(*package, path, maximum_display_observations);
                return emit(output, error, timeline.dump() + '\n',
                            false, "replay");
            }
            if (*energy) {
                const auto evidence = energy_options ? run_energy::load_estimate(path, *package, *energy_options)
                                                    : run_energy::load(path, *package);
                if (evidence) return emit(output, error, *evidence + '\n');
                const auto& failure = evidence.error();
                if (failure.operation == "open_file" && failure.os_error && failure.os_error->value == ENOENT)
                    return emit(output, error, no_package_energy(estimate_energy).dump() + '\n');
                error << "result energy: failed\ncode: " << failure.code.value() << '\n';
                return 9;
            }
            return output_results_export(*package, *selected, path, output, error, format, save_export);
        }
    } catch (const nlohmann::json::exception&) {
        return reject(9, Diagnostic::invalid_document);
    } catch (...) {
        return reject(1, Diagnostic::internal_failure);
    }
}
} // namespace epycbench::cli
