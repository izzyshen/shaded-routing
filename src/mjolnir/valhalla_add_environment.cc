#include "argparse_utils.h"
#include "mjolnir/environment.h"

#include <cxxopts.hpp>

#include <filesystem>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
  const auto program = std::filesystem::path(__FILE__).stem().string();
  boost::property_tree::ptree config;
  std::string manifest_path;

  try {
    // clang-format off
    cxxopts::Options options(
      program,
      program + " " + VALHALLA_PRINT_VERSION + "\n\n"
      "valhalla_add_environment scores every edge of an existing graph tileset against the\n"
      "environmental GeoJSON layers declared in a region manifest (tree canopy, water,\n"
      "open space, street lights, ...) and stores the scores in the tiles, where the\n"
      "costing models use them to prefer shaded, comfortable routes.\n"
      "\n\n");

    options.add_options()
      ("h,help", "Print this help message.")
      ("v,version", "Print the version of this software.")
      ("c,config", "Path to the json configuration file.", cxxopts::value<std::string>())
      ("i,inline-config", "Inline JSON config", cxxopts::value<std::string>())
      ("j,concurrency", "Number of threads to use when processing the data.", cxxopts::value<unsigned int>())
      ("m,manifest", "Path to the region manifest (region.json).", cxxopts::value<std::string>(manifest_path));
    // clang-format on

    auto result = options.parse(argc, argv);
    if (!parse_common_args(program, options, result, &config, "mjolnir.logging", true))
      return EXIT_SUCCESS;

    if (manifest_path.empty()) {
      std::cerr << "A region manifest is required, e.g. -m regions/boston/region.json\n";
      return EXIT_FAILURE;
    }
  } catch (cxxopts::exceptions::exception& e) {
    std::cerr << e.what() << std::endl;
    return EXIT_FAILURE;
  } catch (std::exception& e) {
    std::cerr << "Unable to parse command line options because: " << e.what() << "\n"
              << "This is a bug, please report it at " PACKAGE_BUGREPORT << "\n";
    return EXIT_FAILURE;
  }

  if (!valhalla::mjolnir::AddEnvironment(config, manifest_path)) {
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
