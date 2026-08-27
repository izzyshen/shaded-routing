#include "mjolnir/environment.h"
#include "baldr/graphreader.h"
#include "baldr/rapidjson_utils.h"
#include "midgard/constants.h"
#include "midgard/logging.h"
#include "midgard/pointll.h"
#include "mjolnir/graphtilebuilder.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace valhalla::baldr;
using namespace valhalla::midgard;

namespace {

// meters per degree of latitude, used for rough degree<->meter conversions
constexpr double kMetersPerDegree = 111320.0;
// target spacing between shape sample points
constexpr double kSampleSpacingMeters = 10.0;
// cap on samples per edge so very long edges stay cheap
constexpr size_t kMaxSamplesPerEdge = 48;

enum class SourceKind { kPoint, kLine, kPolygon };

// One GeoJSON source feeding a score: a point layer scores by density within
// radius_m (saturating at saturation features), a line layer by proximity
// within radius_m, and a polygon layer by coverage (optionally also counting
// samples within proximity_m of a polygon boundary, e.g. paths beside water).
// Polygon features can carry their score in a property (value_property /
// value_scale), which turns zone datasets like "canopy % per district" into
// graded coverage instead of binary containment.
struct SourceConfig {
  std::string file;
  SourceKind kind = SourceKind::kPoint;
  double radius_m = 20.0;
  double saturation = 4.0;
  double proximity_m = 0.0;
  double weight = 1.0;
  std::string value_property;
  double value_scale = 100.0;
};

struct SlotConfig {
  size_t slot = 0;
  std::string name;
  std::vector<SourceConfig> sources;
};

struct Manifest {
  std::vector<SourceConfig> canopy;
  std::vector<SlotConfig> layers;
};

// A uniform lon/lat grid mapping cells to feature indexes. Cells are sized so
// that a radius query only ever needs the 3x3 neighborhood of the query cell.
class GridIndex {
public:
  explicit GridIndex(double cell_size_deg) : cell_(cell_size_deg) {
  }

  void insert(double lng, double lat, uint32_t id) {
    cells_[key(cell_x(lng), cell_y(lat))].push_back(id);
  }

  void insert_bbox(double minx, double miny, double maxx, double maxy, uint32_t id) {
    for (int32_t x = cell_x(minx); x <= cell_x(maxx); ++x) {
      for (int32_t y = cell_y(miny); y <= cell_y(maxy); ++y) {
        cells_[key(x, y)].push_back(id);
      }
    }
  }

  // collect candidate ids from the 3x3 cells around the point
  void query_neighborhood(double lng, double lat, std::vector<uint32_t>& out) const {
    out.clear();
    const int32_t cx = cell_x(lng), cy = cell_y(lat);
    for (int32_t x = cx - 1; x <= cx + 1; ++x) {
      for (int32_t y = cy - 1; y <= cy + 1; ++y) {
        auto found = cells_.find(key(x, y));
        if (found != cells_.end()) {
          out.insert(out.end(), found->second.begin(), found->second.end());
        }
      }
    }
  }

  // collect candidate ids from exactly the cell containing the point
  void query_cell(double lng, double lat, std::vector<uint32_t>& out) const {
    out.clear();
    auto found = cells_.find(key(cell_x(lng), cell_y(lat)));
    if (found != cells_.end()) {
      out.assign(found->second.begin(), found->second.end());
    }
  }

private:
  int32_t cell_x(double lng) const {
    return static_cast<int32_t>(std::floor(lng / cell_));
  }
  int32_t cell_y(double lat) const {
    return static_cast<int32_t>(std::floor(lat / cell_));
  }
  static int64_t key(int32_t x, int32_t y) {
    return (static_cast<int64_t>(x) << 32) | static_cast<uint32_t>(y);
  }

  double cell_;
  std::unordered_map<int64_t, std::vector<uint32_t>> cells_;
};

// A polygon ring with a latitude-banded segment index so containment tests cost
// O(segments in the sample's band) instead of O(all segments) — required for
// city-scale rings with tens of thousands of vertices.
struct Ring {
  std::vector<PointLL> pts;
  double minx = 0, miny = 0, maxx = 0, maxy = 0;
  double value = 1.0; // score applied when a sample is inside (see value_property)
  double band_height = 0;
  std::vector<std::vector<uint32_t>> bands; // per-band indexes of segment start points

  void build_bands() {
    const size_t band_count = std::min<size_t>(std::max<size_t>(pts.size() / 8, 1), 4096);
    band_height = (maxy - miny) / static_cast<double>(band_count);
    if (band_height <= 0) {
      band_height = 1;
    }
    bands.assign(band_count, {});
    for (uint32_t i = 0, j = pts.size() - 1; i < pts.size(); j = i++) {
      const double lo = std::min(pts[i].lat(), pts[j].lat());
      const double hi = std::max(pts[i].lat(), pts[j].lat());
      const size_t lo_band = band_for(lo);
      const size_t hi_band = band_for(hi);
      for (size_t b = lo_band; b <= hi_band; ++b) {
        bands[b].push_back(i);
      }
    }
  }

  size_t band_for(double lat) const {
    const double clamped = std::min(std::max(lat, miny), maxy);
    return std::min(static_cast<size_t>((clamped - miny) / band_height), bands.size() - 1);
  }

  // even-odd containment using only the segments crossing the sample's band
  bool contains(const PointLL& p) const {
    if (p.lng() < minx || p.lng() > maxx || p.lat() < miny || p.lat() > maxy) {
      return false;
    }
    bool inside = false;
    for (const uint32_t i : bands[band_for(p.lat())]) {
      const uint32_t j = i == 0 ? pts.size() - 1 : i - 1;
      const double xi = pts[i].lng(), yi = pts[i].lat();
      const double xj = pts[j].lng(), yj = pts[j].lat();
      if (((yi > p.lat()) != (yj > p.lat())) &&
          (p.lng() < (xj - xi) * (p.lat() - yi) / (yj - yi) + xi)) {
        inside = !inside;
      }
    }
    return inside;
  }
};

// A single indexed segment: proximity queries test point-to-segment distance so
// even city-length rivers and huge ring boundaries stay cheap per sample.
struct Segment {
  PointLL a, b;
  double value = 1.0;
};

// approximate point-to-segment distance in meters (equirectangular locally)
double point_segment_distance_m(const PointLL& p, const Segment& seg) {
  const double coslat = std::cos(p.lat() * kRadPerDegD);
  const double ax = (seg.a.lng() - p.lng()) * kMetersPerDegree * coslat;
  const double ay = (seg.a.lat() - p.lat()) * kMetersPerDegree;
  const double bx = (seg.b.lng() - p.lng()) * kMetersPerDegree * coslat;
  const double by = (seg.b.lat() - p.lat()) * kMetersPerDegree;
  const double dx = bx - ax, dy = by - ay;
  const double len_sq = dx * dx + dy * dy;
  double t = len_sq > 0 ? -(ax * dx + ay * dy) / len_sq : 0.0;
  t = std::min(std::max(t, 0.0), 1.0);
  const double cx = ax + t * dx, cy = ay + t * dy;
  return std::sqrt(cx * cx + cy * cy);
}

// All features of one source, spatially indexed for sampling queries.
struct FeatureStore {
  SourceConfig cfg;
  std::vector<PointLL> points;
  size_t line_count = 0;
  std::vector<Segment> segments; // line geometry and ring boundaries (for proximity)
  std::vector<Ring> rings;
  std::unique_ptr<GridIndex> grid;         // points / rings by kind
  std::unique_ptr<GridIndex> segment_grid; // proximity segments

  size_t feature_count() const {
    switch (cfg.kind) {
      case SourceKind::kPoint:
        return points.size();
      case SourceKind::kLine:
        return line_count;
      case SourceKind::kPolygon:
        return rings.size();
    }
    return 0;
  }
};

void parse_coord_ring(const rapidjson::Value& coords, std::vector<PointLL>& out) {
  out.reserve(coords.Size());
  for (const auto& c : coords.GetArray()) {
    if (c.IsArray() && c.Size() >= 2) {
      out.emplace_back(c[0].GetDouble(), c[1].GetDouble());
    }
  }
}

void add_ring(FeatureStore& store, std::vector<PointLL>&& pts, double value) {
  if (pts.size() < 3) {
    return;
  }
  Ring ring;
  ring.pts = std::move(pts);
  ring.value = value;
  ring.minx = ring.maxx = ring.pts.front().lng();
  ring.miny = ring.maxy = ring.pts.front().lat();
  for (const auto& p : ring.pts) {
    ring.minx = std::min(ring.minx, p.lng());
    ring.maxx = std::max(ring.maxx, p.lng());
    ring.miny = std::min(ring.miny, p.lat());
    ring.maxy = std::max(ring.maxy, p.lat());
  }
  ring.build_bands();
  store.rings.emplace_back(std::move(ring));
}

// score value a polygon feature contributes when a sample is inside it
double feature_value(const SourceConfig& cfg, const rapidjson::Value& feature) {
  if (cfg.value_property.empty()) {
    return 1.0;
  }
  if (feature.HasMember("properties") && feature["properties"].IsObject()) {
    const auto& props = feature["properties"];
    auto prop = props.FindMember(cfg.value_property.c_str());
    if (prop != props.MemberEnd() && prop->value.IsNumber()) {
      const double scale = cfg.value_scale > 0 ? cfg.value_scale : 100.0;
      return std::min(std::max(prop->value.GetDouble() / scale, 0.0), 1.0);
    }
  }
  return 0.0;
}

void add_line(FeatureStore& store, std::vector<PointLL>&& pts) {
  if (pts.size() < 2) {
    return;
  }
  for (size_t i = 1; i < pts.size(); ++i) {
    store.segments.push_back({pts[i - 1], pts[i], 1.0});
  }
  store.line_count++;
}

void add_ring_boundary_segments(FeatureStore& store, const Ring& ring) {
  for (size_t i = 0, j = ring.pts.size() - 1; i < ring.pts.size(); j = i++) {
    store.segments.push_back({ring.pts[j], ring.pts[i], ring.value});
  }
}

// Load one GeoJSON file into a store. Geometry that doesn't match the source
// kind is coerced where sensible (polygon rings become lines for line sources)
// and skipped otherwise.
FeatureStore load_source(const SourceConfig& cfg, const std::filesystem::path& base_dir) {
  FeatureStore store;
  store.cfg = cfg;

  const auto path = base_dir / cfg.file;
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    throw std::runtime_error("Cannot open GeoJSON source: " + path.string());
  }
  std::stringstream buffer;
  buffer << stream.rdbuf();

  rapidjson::Document doc;
  doc.Parse(buffer.str().c_str());
  if (doc.HasParseError() || !doc.IsObject() || !doc.HasMember("features")) {
    throw std::runtime_error("Invalid GeoJSON FeatureCollection: " + path.string());
  }

  for (const auto& feature : doc["features"].GetArray()) {
    if (!feature.IsObject() || !feature.HasMember("geometry") || !feature["geometry"].IsObject()) {
      continue;
    }
    const auto& geom = feature["geometry"];
    if (!geom.HasMember("type") || !geom.HasMember("coordinates")) {
      continue;
    }
    const std::string type = geom["type"].GetString();
    const auto& coords = geom["coordinates"];
    const double value = cfg.kind == SourceKind::kPolygon ? feature_value(cfg, feature) : 1.0;

    if (type == "Point" && coords.IsArray() && coords.Size() >= 2) {
      store.points.emplace_back(coords[0].GetDouble(), coords[1].GetDouble());
    } else if (type == "MultiPoint" && coords.IsArray()) {
      for (const auto& c : coords.GetArray()) {
        if (c.IsArray() && c.Size() >= 2) {
          store.points.emplace_back(c[0].GetDouble(), c[1].GetDouble());
        }
      }
    } else if (type == "LineString" && coords.IsArray()) {
      std::vector<PointLL> pts;
      parse_coord_ring(coords, pts);
      add_line(store, std::move(pts));
    } else if (type == "MultiLineString" && coords.IsArray()) {
      for (const auto& line : coords.GetArray()) {
        std::vector<PointLL> pts;
        parse_coord_ring(line, pts);
        add_line(store, std::move(pts));
      }
    } else if (type == "Polygon" && coords.IsArray()) {
      for (const auto& ring : coords.GetArray()) {
        std::vector<PointLL> pts;
        parse_coord_ring(ring, pts);
        if (cfg.kind == SourceKind::kLine) {
          add_line(store, std::move(pts));
        } else {
          add_ring(store, std::move(pts), value);
        }
      }
    } else if (type == "MultiPolygon" && coords.IsArray()) {
      for (const auto& poly : coords.GetArray()) {
        for (const auto& ring : poly.GetArray()) {
          std::vector<PointLL> pts;
          parse_coord_ring(ring, pts);
          if (cfg.kind == SourceKind::kLine) {
            add_line(store, std::move(pts));
          } else {
            add_ring(store, std::move(pts), value);
          }
        }
      }
    }
  }

  // build the spatial index; cells sized to the largest distance we query so a
  // 3x3 neighborhood always covers a radius query. Polygon sources use coarser
  // cells because their rings are inserted per overlapped cell and can span a
  // whole city.
  const double query_m = std::max({cfg.radius_m, cfg.proximity_m, 30.0});
  const double cell_deg =
      (cfg.kind == SourceKind::kPolygon ? std::max(query_m, 150.0) : query_m) / kMetersPerDegree;
  store.grid = std::make_unique<GridIndex>(cell_deg);
  switch (cfg.kind) {
    case SourceKind::kPoint:
      for (uint32_t i = 0; i < store.points.size(); ++i) {
        store.grid->insert(store.points[i].lng(), store.points[i].lat(), i);
      }
      break;
    case SourceKind::kLine:
      break; // lines live in the segment grid built below
    case SourceKind::kPolygon:
      for (uint32_t i = 0; i < store.rings.size(); ++i) {
        const auto& r = store.rings[i];
        store.grid->insert_bbox(r.minx, r.miny, r.maxx, r.maxy, i);
        // ring boundaries feed the proximity segment grid
        if (cfg.proximity_m > 0.0) {
          add_ring_boundary_segments(store, r);
        }
      }
      break;
  }

  // segments (line geometry, ring boundaries) get their own index; every
  // segment is short enough that inserting its bbox touches only a few cells
  if (!store.segments.empty()) {
    store.segment_grid = std::make_unique<GridIndex>(cell_deg);
    for (uint32_t i = 0; i < store.segments.size(); ++i) {
      const auto& seg = store.segments[i];
      store.segment_grid->insert_bbox(std::min(seg.a.lng(), seg.b.lng()),
                                      std::min(seg.a.lat(), seg.b.lat()),
                                      std::max(seg.a.lng(), seg.b.lng()),
                                      std::max(seg.a.lat(), seg.b.lat()), i);
    }
  }
  return store;
}

// Score one sample point against a source, in [0, 1].
double
score_sample(const FeatureStore& store, const PointLL& sample, std::vector<uint32_t>& scratch) {
  const auto& cfg = store.cfg;
  switch (cfg.kind) {
    case SourceKind::kPoint: {
      store.grid->query_neighborhood(sample.lng(), sample.lat(), scratch);
      size_t count = 0;
      for (const auto id : scratch) {
        if (sample.Distance(store.points[id]) <= cfg.radius_m) {
          ++count;
        }
      }
      return std::min(static_cast<double>(count) / cfg.saturation, 1.0);
    }
    case SourceKind::kLine: {
      if (!store.segment_grid) {
        return 0.0;
      }
      store.segment_grid->query_neighborhood(sample.lng(), sample.lat(), scratch);
      for (const auto id : scratch) {
        if (point_segment_distance_m(sample, store.segments[id]) <= cfg.radius_m) {
          return 1.0;
        }
      }
      return 0.0;
    }
    case SourceKind::kPolygon: {
      store.grid->query_cell(sample.lng(), sample.lat(), scratch);
      // the strongest polygon containing the sample wins; ring values default to
      // 1.0 and come from value_property for graded zone datasets
      double best = 0.0;
      for (const auto id : scratch) {
        const auto& ring = store.rings[id];
        if (ring.value > best && ring.contains(sample)) {
          best = ring.value;
        }
      }
      if (best < 1.0 && cfg.proximity_m > 0.0 && store.segment_grid) {
        store.segment_grid->query_neighborhood(sample.lng(), sample.lat(), scratch);
        for (const auto id : scratch) {
          const auto& seg = store.segments[id];
          if (seg.value > best && point_segment_distance_m(sample, seg) <= cfg.proximity_m) {
            best = seg.value;
          }
        }
      }
      return best;
    }
  }
  return 0.0;
}

// Weighted blend of all sources of one score over the edge's sample points.
double score_edge(const std::vector<FeatureStore>& sources,
                  const std::vector<PointLL>& samples,
                  std::vector<uint32_t>& scratch) {
  double total_weight = 0.0, blended = 0.0;
  for (const auto& store : sources) {
    double sum = 0.0;
    for (const auto& sample : samples) {
      sum += score_sample(store, sample, scratch);
    }
    blended += store.cfg.weight * (sum / samples.size());
    total_weight += store.cfg.weight;
  }
  return total_weight > 0.0 ? blended / total_weight : 0.0;
}

// Walk the shape and emit samples roughly every kSampleSpacingMeters.
std::vector<PointLL> sample_shape(const std::vector<PointLL>& shape, double edge_length_m) {
  std::vector<PointLL> samples;
  if (shape.empty()) {
    return samples;
  }
  const double spacing =
      std::max(kSampleSpacingMeters, edge_length_m / static_cast<double>(kMaxSamplesPerEdge));
  samples.push_back(shape.front());
  double carried = 0.0;
  for (size_t i = 1; i < shape.size(); ++i) {
    const double seg = shape[i - 1].Distance(shape[i]);
    if (seg <= 0.0) {
      continue;
    }
    double along = spacing - carried;
    while (along < seg) {
      const double t = along / seg;
      samples.emplace_back(shape[i - 1].lng() + (shape[i].lng() - shape[i - 1].lng()) * t,
                           shape[i - 1].lat() + (shape[i].lat() - shape[i - 1].lat()) * t);
      along += spacing;
    }
    carried = seg - (along - spacing);
  }
  return samples;
}

SourceConfig parse_source(const rapidjson::Value& v) {
  SourceConfig cfg;
  if (!v.IsObject() || !v.HasMember("file") || !v.HasMember("kind")) {
    throw std::runtime_error("Each source needs at least a \"file\" and a \"kind\"");
  }
  cfg.file = v["file"].GetString();
  const std::string kind = v["kind"].GetString();
  if (kind == "point") {
    cfg.kind = SourceKind::kPoint;
  } else if (kind == "line") {
    cfg.kind = SourceKind::kLine;
  } else if (kind == "polygon") {
    cfg.kind = SourceKind::kPolygon;
  } else {
    throw std::runtime_error("Unknown source kind: " + kind);
  }
  if (v.HasMember("radius_m") && v["radius_m"].IsNumber()) {
    cfg.radius_m = v["radius_m"].GetDouble();
  }
  if (v.HasMember("saturation") && v["saturation"].IsNumber()) {
    cfg.saturation = std::max(v["saturation"].GetDouble(), 1.0);
  }
  if (v.HasMember("proximity_m") && v["proximity_m"].IsNumber()) {
    cfg.proximity_m = v["proximity_m"].GetDouble();
  }
  if (v.HasMember("weight") && v["weight"].IsNumber()) {
    cfg.weight = std::max(v["weight"].GetDouble(), 0.0);
  }
  if (v.HasMember("value_property") && v["value_property"].IsString()) {
    cfg.value_property = v["value_property"].GetString();
  }
  if (v.HasMember("value_scale") && v["value_scale"].IsNumber()) {
    cfg.value_scale = v["value_scale"].GetDouble();
  }
  return cfg;
}

Manifest parse_manifest(const std::string& manifest_path) {
  std::ifstream stream(manifest_path, std::ios::binary);
  if (!stream) {
    throw std::runtime_error("Cannot open region manifest: " + manifest_path);
  }
  std::stringstream buffer;
  buffer << stream.rdbuf();
  rapidjson::Document doc;
  doc.Parse(buffer.str().c_str());
  if (doc.HasParseError() || !doc.IsObject()) {
    throw std::runtime_error("Region manifest is not valid JSON: " + manifest_path);
  }

  Manifest manifest;
  if (doc.HasMember("canopy") && doc["canopy"].IsObject() && doc["canopy"].HasMember("sources") &&
      doc["canopy"]["sources"].IsArray()) {
    for (const auto& s : doc["canopy"]["sources"].GetArray()) {
      manifest.canopy.push_back(parse_source(s));
    }
  }
  if (doc.HasMember("layers") && doc["layers"].IsArray()) {
    for (const auto& layer : doc["layers"].GetArray()) {
      if (!layer.IsObject() || !layer.HasMember("slot") || !layer.HasMember("sources")) {
        continue;
      }
      SlotConfig slot;
      slot.slot = layer["slot"].GetUint();
      if (slot.slot >= kMaxGeoJsonLayers) {
        throw std::runtime_error("Layer slot " + std::to_string(slot.slot) + " exceeds the max of " +
                                 std::to_string(kMaxGeoJsonLayers - 1));
      }
      if (layer.HasMember("name") && layer["name"].IsString()) {
        slot.name = layer["name"].GetString();
      }
      for (const auto& s : layer["sources"].GetArray()) {
        slot.sources.push_back(parse_source(s));
      }
      manifest.layers.push_back(std::move(slot));
    }
  }
  if (manifest.canopy.empty() && manifest.layers.empty()) {
    throw std::runtime_error("Region manifest declares no canopy sources and no layers");
  }
  return manifest;
}

} // namespace

namespace valhalla {
namespace mjolnir {

bool AddEnvironment(const boost::property_tree::ptree& config, const std::string& manifest_path) {
  const auto& mjolnir_config = config.get_child("mjolnir");
  const std::string tile_dir = mjolnir_config.get<std::string>("tile_dir");
  const size_t num_threads =
      mjolnir_config.get<size_t>("concurrency", std::thread::hardware_concurrency());

  // load the manifest and all of its sources up front
  const auto manifest = parse_manifest(manifest_path);
  const auto base_dir = std::filesystem::path(manifest_path).parent_path();

  std::vector<FeatureStore> canopy_sources;
  for (const auto& cfg : manifest.canopy) {
    canopy_sources.push_back(load_source(cfg, base_dir));
    LOG_INFO("Loaded canopy source " + cfg.file + " with " +
             std::to_string(canopy_sources.back().feature_count()) + " features");
  }
  // slot -> sources; untouched slots score 0 and cost nothing at query time
  std::array<std::vector<FeatureStore>, kMaxGeoJsonLayers> layer_sources;
  for (const auto& slot : manifest.layers) {
    for (const auto& cfg : slot.sources) {
      layer_sources[slot.slot].push_back(load_source(cfg, base_dir));
      LOG_INFO("Loaded layer " + std::to_string(slot.slot) + " (" + slot.name + ") source " +
               cfg.file + " with " + std::to_string(layer_sources[slot.slot].back().feature_count()) +
               " features");
    }
  }

  // list all tiles once, then let workers claim them
  std::vector<GraphId> tiles;
  {
    GraphReader reader(mjolnir_config);
    auto tile_set = reader.GetTileSet();
    tiles.assign(tile_set.begin(), tile_set.end());
  }
  LOG_INFO("Scoring environment on " + std::to_string(tiles.size()) + " tiles with " +
           std::to_string(num_threads) + " threads");

  std::atomic<size_t> next_tile{0};
  std::atomic<size_t> scored_edges{0};
  std::atomic<size_t> updated_tiles{0};
  std::atomic<bool> failed{false};

  auto worker = [&]() {
    GraphReader reader(mjolnir_config);
    std::vector<uint32_t> scratch;
    while (true) {
      const size_t index = next_tile.fetch_add(1);
      if (index >= tiles.size() || failed.load()) {
        break;
      }
      if (index > 0 && index % 250 == 0) {
        LOG_INFO("Scoring tile " + std::to_string(index) + "/" + std::to_string(tiles.size()));
      }
      try {
        auto tile = reader.GetGraphTile(tiles[index]);
        if (!tile) {
          continue;
        }

        // score each unique edgeinfo once; twin edges share the result
        std::unordered_map<uint32_t, EnvironmentScores> scored;
        std::vector<std::pair<uint32_t, uint32_t>> edge_offsets; // edge idx -> edgeinfo offset
        for (uint32_t i = 0; i < tile->header()->directededgecount(); ++i) {
          const DirectedEdge* edge = tile->directededge(i);
          const uint32_t offset = edge->edgeinfo_offset();
          edge_offsets.emplace_back(i, offset);
          if (scored.count(offset)) {
            continue;
          }

          auto samples = sample_shape(tile->edgeinfo(edge).shape(), edge->length());
          EnvironmentScores scores;
          if (!samples.empty()) {
            if (!canopy_sources.empty()) {
              scores.tree_canopy = static_cast<uint8_t>(
                  std::lround(kMaxTreeCanopy * score_edge(canopy_sources, samples, scratch)));
            }
            for (size_t slot = 0; slot < kMaxGeoJsonLayers; ++slot) {
              if (!layer_sources[slot].empty()) {
                scores.layer_scores[slot] = static_cast<uint8_t>(
                    std::lround(254 * score_edge(layer_sources[slot], samples, scratch)));
              }
            }
          }
          scored.emplace(offset, scores);
        }

        // only rewrite the tile if something scored non-zero
        const auto is_zero = [](const EnvironmentScores& s) {
          return s.tree_canopy == 0 && std::all_of(s.layer_scores.begin(), s.layer_scores.end(),
                                                   [](uint8_t v) { return v == 0; });
        };
        size_t nonzero = 0;
        for (const auto& kv : scored) {
          nonzero += !is_zero(kv.second);
        }
        if (nonzero == 0) {
          continue;
        }

        std::vector<std::pair<GraphId, std::string>> tagged;
        tagged.reserve(edge_offsets.size());
        for (const auto& [edge_index, offset] : edge_offsets) {
          const auto& scores = scored[offset];
          if (is_zero(scores)) {
            continue;
          }
          tagged.emplace_back(GraphId(tiles[index].tileid(), tiles[index].level(), edge_index),
                              EdgeInfo::EncodeEnvironment(scores));
        }
        GraphTileBuilder builder(tile_dir, tiles[index], true);
        const size_t added = builder.AddTaggedValues(TaggedValue::kEnvironment, tagged);
        builder.StoreTileData();
        scored_edges += added;
        ++updated_tiles;
      } catch (const std::exception& e) {
        LOG_ERROR("Failed to add environment to tile " + std::to_string(tiles[index].value) + ": " +
                  e.what());
        failed.store(true);
      }
    }
  };

  std::vector<std::thread> threads;
  for (size_t i = 0; i < std::max<size_t>(num_threads, 1); ++i) {
    threads.emplace_back(worker);
  }
  for (auto& thread : threads) {
    thread.join();
  }

  if (failed.load()) {
    return false;
  }
  LOG_INFO("Environment scoring done: " + std::to_string(scored_edges.load()) +
           " edge infos tagged across " + std::to_string(updated_tiles.load()) + " tiles");
  return true;
}

} // namespace mjolnir
} // namespace valhalla
