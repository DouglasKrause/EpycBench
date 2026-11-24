#include "cli/application.hpp"
#include "epycbench/build_manifest.hpp"
#include "runtime/package_runner.hpp"
#include "topology/topology_cli.hpp"

#include <iostream>
#include <string_view>

int main(int argc, char* argv[]) {
    if (argc == 2 && std::string_view{argv[1]} == "__package-runner")
        return epycbench::runtime::serve_package_runner();
    if (argc == 3 && std::string_view{argv[1]} == "version" && std::string_view{argv[2]} == "--json") {
        std::cout << epycbench::build_manifest_json().dump() << '\n';
        return 0;
    }
    auto environment = epycbench::topology::cli::make_production_environment();
    return epycbench::cli::run(argc, argv, environment, std::cout, std::cerr);
}
