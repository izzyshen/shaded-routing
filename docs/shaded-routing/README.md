# Shaded Routing — Architecture

This fork extends [Valhalla](https://github.com/valhalla/valhalla) with **comfort-first
routing**: edges gain environmental scores (tree canopy, water proximity, parks, street
lights), and the costing models discount scored edges so pedestrians and cyclists are
guided along shaded, cooler, more pleasant streets — then hand the chosen route to the
user's own Google Maps for turn-by-turn navigation.

## Data flow

```
regions/<id>/*.geojson            OSM extract
        │                              │
        │  region.json manifest        │  valhalla_build_tiles
        ▼                              ▼
valhalla_add_environment  ──────►  graph tiles
  (samples every edge shape,       (EdgeInfo tagged value:
   scores each layer 0-254,         1 byte canopy + 4 layer bytes)
   canopy 0-127)                        │
                                        ▼
                              valhalla_service (routing)
                                        │  tree_canopy_factor /
                                        │  geojson_layer_factors
                                        ▼
                                  web/index.html
                                        │  Visvalingam waypoint selection
                                        ▼
                          Google Maps directions deep link
                         (≤9 waypoints, walking/bicycling)
```

## Pieces

- **Storage** (`baldr`): `TaggedValue::kEnvironment` carries a fixed, null-free
  6-byte payload per edge (`EdgeInfo::environment()` / `EncodeEnvironment()`).
  No tile binary-format change; tiles stay compatible with upstream tooling.
- **Ingestion** (`mjolnir`): `valhalla_add_environment` loads a region manifest,
  spatially indexes its GeoJSON sources (uniform grid), samples each edge shape
  every ~10 m, blends per-source scores, and rewrites tiles through
  `GraphTileBuilder::AddTaggedValue` (generalized from the landmark machinery).
- **Costing** (`sif`): all motorized and active modes read the scores in
  `EdgeCost()`. Canopy reduces edge cost up to a per-mode cap (90% pedestrian,
  85% bicycle, 80% others at `tree_canopy_factor = 2`); supplemental layers
  multiply in via `GeoJsonCostMultiplier`. Requests opt in/out per query —
  tiles don't need rebuilding to change preferences.
- **Web UI** (`web/index.html`): Leaflet app, entirely region-driven by
  `regions/<id>/region.json` (`?region=<id>` to switch). Draws the exact shaded
  polylines and offers **Open in Google Maps** / **QR** per route.

## Google Maps handoff

Google Maps cannot ingest third-party polylines, so the web UI compresses each
route into its 9 most shape-defining interior vertices (Visvalingam–Whyatt) and
builds a `https://www.google.com/maps/dir/?api=1&...&waypoints=...` deep link.
Google then navigates through those waypoints — on short urban walks this pins
its route to the same streets the shade-aware costing picked. The QR button
shows the same link for phone handoff. Caveats: the Maps app honors at most 9
waypoints (mobile browsers only 3), and between waypoints Google may deviate
from the shaded ideal on long routes.

## Shade model

Boston's street-tree inventory mixes real trees with empty planting pits,
scheduled sites, and stumps; `trees_clean.geojson` keeps only live trees with a
trunk (DBH > 1"). Each tree's shade is modeled from its trunk diameter:

```
crown radius  r_c = clamp(0.5 + 0.33·DBH_in, 1, 8)  m
crown height  h_c = clamp(2.5 + 0.40·DBH_in, 3, 12) m
shadow offset o   = min(h_c / tan(sun elevation), 25) m, cast away from the sun
```

A route sample (every ~12 m) counts as shaded when it falls within
`r_c + 2 m` of a tree's displaced shade center; the sun's azimuth/elevation
come from the NOAA solar equations, computed client-side. How much shade
matters *right now* scales the routing request's canopy factor:

```
D = clamp(sin(el)/sin(60°), 0, 1)      sun height: 0 at horizon, 1 near overhead
  · (1 − 0.75·cloud)                    Open-Meteo cloud cover (keyless API)
  · clamp((T − 18)/14, 0.3, 1)          heat: mild days need less shade
effective factor = slider · (0.25 + 0.75·D)
```

So an overcast dusk routes nearly straight, a clear 32 °C noon applies the
user's full shade preference, and the per-route "% shaded" reflects where
shadows actually fall at walk time.

## Build notes (macOS)

Two environment pitfalls are worth knowing:

- **Conda/Anaconda protobuf is ABI-incompatible** with a system-toolchain build:
  the service compiles fine but crashes (SIGBUS in protobuf's parser) on every
  request. CMake now refuses a conda-resolved protobuf at configure time; fix it
  by keeping conda out of the search path:
  `cmake -DCMAKE_IGNORE_PREFIX_PATH=/opt/anaconda3 ...` with Homebrew's
  `protobuf` installed. Protobuf ≥ 30 also needs
  `-DCMAKE_CXX_FLAGS="-Wno-deprecated-declarations -Wno-unused-result"`.
- **`pkgconf` must be installed** (`brew install pkgconf`) or CMake reports
  misleading "cURL not found" / "prime_server not found" errors; prime_server
  itself must be built from source and pointed to via `PKG_CONFIG_PATH`.

## Running Boston end-to-end

```bash
# 1. build (see upstream docs for deps; macOS needs prime_server built from source)
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build

# 2. tiles for the Boston bbox
osmium extract -b -71.3,42.15,-70.85,42.5 massachusetts-latest.osm.pbf -o boston.osm.pbf
python3 scripts/valhalla_build_config --mjolnir-tile-dir $PWD/tiles > valhalla.json
./build/valhalla_build_tiles -c valhalla.json boston.osm.pbf

# 3. environmental scores
./build/valhalla_add_environment -c valhalla.json -m regions/boston/region.json

# 4. serve
./build/valhalla_service valhalla.json 4 &
python3 -m http.server 8080 &   # from the repo root
open http://localhost:8080/web/index.html
```
