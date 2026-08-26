#ifndef VALHALLA_MJOLNIR_ENVIRONMENT_H
#define VALHALLA_MJOLNIR_ENVIRONMENT_H

#include <boost/property_tree/ptree.hpp>

#include <string>

namespace valhalla {
namespace mjolnir {

/**
 * Read a region manifest and write environmental scores (tree canopy plus up to
 * kMaxGeoJsonLayers supplemental layer scores) onto every edge of an already
 * built graph tileset. Scores are stored as a kEnvironment tagged value in each
 * edge's EdgeInfo, where the costing models read them at query time.
 *
 * The manifest is a region.json file (see regions/README.md) whose "canopy"
 * and "layers" sections declare GeoJSON sources (point, line or polygon) and
 * how they map onto scores. GeoJSON paths are resolved relative to the
 * manifest's directory, which makes any region a drop-in: point the tool at a
 * different manifest and no code changes are needed.
 *
 * @param config         the full valhalla config (mjolnir.tile_dir is used)
 * @param manifest_path  path to the region.json manifest
 * @return true on success
 */
bool AddEnvironment(const boost::property_tree::ptree& config, const std::string& manifest_path);

} // namespace mjolnir
} // namespace valhalla

#endif // VALHALLA_MJOLNIR_ENVIRONMENT_H
