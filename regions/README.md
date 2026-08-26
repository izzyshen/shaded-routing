# Regions

Each subdirectory is a self-contained region: the GeoJSON layers for that city plus a
`region.json` manifest describing how those layers become routing scores and how the
web UI should display them. **Nothing outside this directory knows about any specific
city** — adding a region requires no code changes.

## Adding a region

1. Create `regions/<id>/` and drop in your GeoJSON files (`EPSG:4326` /
   FeatureCollections). Typical sources: municipal open-data portals (tree
   inventories, canopy assessments, hydrography, parks, street lights).
2. Write `regions/<id>/region.json` (schema below).
3. Add the region id to `regions/index.json`.
4. Build routing tiles for the region's OSM extract, then score them:

   ```bash
   # clip an OSM extract to your region (example bbox)
   osmium extract -b <lonMin>,<latMin>,<lonMax>,<latMax> state.osm.pbf -o region.osm.pbf

   # build the routing graph
   valhalla_build_tiles -c valhalla.json region.osm.pbf

   # score every edge against the region's environmental layers
   valhalla_add_environment -c valhalla.json -m regions/<id>/region.json
   ```

5. Serve the repo root over HTTP and open `web/index.html?region=<id>`.

## region.json schema

```jsonc
{
  "id": "boston",                    // must match the directory name
  "name": "Boston",                  // shown in the UI
  "attribution": "...",              // data credits
  "bounds": {                        // region bounding box (loop generation, sanity checks)
    "latMin": 42.15, "latMax": 42.5, "lonMin": -71.3, "lonMax": -70.85
  },
  "center": { "lat": 42.35, "lon": -71.08, "zoom": 13 },
  "defaults": {                      // initial origin/destination markers
    "origin":      { "lat": ..., "lon": ..., "label": "Boston Common" },
    "destination": { "lat": ..., "lon": ..., "label": "Fenway Park" }
  },

  // Shade signal: blended into the per-edge tree-canopy score (0-127).
  "canopy": {
    "sources": [ /* source objects, see below */ ],
    "display": { "stroke": "#4F6F52", "fill": "#A8C2A5" }
  },

  // Supplemental comfort layers: each occupies one costing slot (0-3) and
  // becomes a per-edge score (0-254) that the costing models can discount.
  "layers": [
    {
      "slot": 0,
      "name": "open_space",
      "label": "Parks & Open Space",
      "sources": [ /* source objects */ ],
      "display": { /* colors for the web UI */ }
    }
  ],

  // Display-only overlays: drawn on the map, never scored.
  "overlays": [
    { "name": "landmarks", "label": "Landmarks", "file": "landmarks.geojson", "display": { ... } }
  ]
}
```

### Source objects

Every source references one GeoJSON file (path relative to the region directory)
and declares how its geometry scores an edge sample point:

| field         | applies to        | meaning                                                            | default |
| ------------- | ----------------- | ------------------------------------------------------------------ | ------- |
| `file`        | all               | GeoJSON file in this region directory                              | —       |
| `kind`        | all               | `point`, `line`, or `polygon`                                      | —       |
| `radius_m`    | point, line       | how close a feature must be to count                               | 20      |
| `saturation`  | point             | feature count within radius that maxes the score                   | 4       |
| `proximity_m` | polygon           | also score samples this close to a polygon boundary (e.g. water)   | 0       |
| `weight`      | all               | blend weight when a score has several sources                      | 1.0     |

Scoring: each edge is sampled every ~10 m along its shape; each source scores each
sample in `[0, 1]` (polygon: inside / boundary-proximity, point: density saturation,
line: proximity); sources are blended by weight; the edge's mean becomes the stored
score. `valhalla_add_environment` writes the scores into the graph tiles as an
`EdgeInfo` tagged value — rerun it whenever tiles are rebuilt or the manifest changes.

### How scores affect routing

At query time the costing options control how strongly scores discount edge cost:

- `tree_canopy_factor` (0–2): scales the shade preference. `0` disables it,
  `1` is the default strength, `2` strongly prefers shaded streets.
- `geojson_layer_factors`: per-slot `{ slot, factor, base_multiplier, max_reduction }`
  overrides for the supplemental layers.

Example request snippet:

```json
{
  "costing": "pedestrian",
  "costing_options": {
    "pedestrian": {
      "tree_canopy_factor": 1.5,
      "geojson_layer_factors": [
        { "slot": 1, "name": "water", "factor": 1.2 }
      ]
    }
  }
}
```
