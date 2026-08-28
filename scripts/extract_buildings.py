#!/usr/bin/env python3
"""Extract building footprints + heights from a region OSM extract.

Buildings shade sidewalks, so the web app's shade model needs their outlines
and heights. This derives them from the same .osm.pbf used to build tiles and
writes them as small grid-cell files the app lazy-loads near a route.

Usage:
  python3 scripts/extract_buildings.py .data/boston.osm.pbf regions/boston/buildings

Requires osmium-tool (brew install osmium-tool). Output is gitignored: it is
derivable data, regenerate it per region instead of committing it.

Cell files: {i}_{j}.json where i=floor(lat/0.02), j=floor(lon/0.02), each
  {"b": [[h_m, [[lon,lat], ...outer ring...]], ...]}
"""

import json
import math
import os
import re
import subprocess
import sys
import tempfile

CELL_DEG = 0.02
DEFAULT_HEIGHT_M = 9.0
METERS_PER_LEVEL = 3.2
MIN_RING_POINTS = 4


def parse_height(props):
    h = props.get("height")
    if h:
        m = re.match(r"^\s*(\d+(?:\.\d+)?)", str(h))
        if m:
            return min(150.0, float(m.group(1)))
    levels = props.get("building:levels")
    if levels:
        m = re.match(r"^\s*(\d+(?:\.\d+)?)", str(levels))
        if m:
            return min(150.0, 2.0 + float(m.group(1)) * METERS_PER_LEVEL)
    return DEFAULT_HEIGHT_M


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    pbf, outdir = sys.argv[1], sys.argv[2]
    os.makedirs(outdir, exist_ok=True)

    with tempfile.TemporaryDirectory() as tmp:
        filtered = os.path.join(tmp, "buildings.osm.pbf")
        geojson = os.path.join(tmp, "buildings.geojson")
        subprocess.run(["osmium", "tags-filter", pbf, "a/building", "-o", filtered],
                       check=True)
        subprocess.run(["osmium", "export", filtered, "--geometry-types=polygon",
                        "-o", geojson], check=True)

        cells = {}
        n_buildings = 0
        with open(geojson) as fh:
            data = json.load(fh)
        for feat in data.get("features", []):
            geom = feat.get("geometry") or {}
            polys = ([geom["coordinates"]] if geom.get("type") == "Polygon"
                     else geom["coordinates"] if geom.get("type") == "MultiPolygon"
                     else [])
            if not polys:
                continue
            h = parse_height(feat.get("properties") or {})
            for poly in polys:
                ring = poly[0] if poly else []
                if len(ring) < MIN_RING_POINTS:
                    continue
                ring = [[round(c[0], 6), round(c[1], 6)] for c in ring]
                # index by centroid cell
                clat = sum(p[1] for p in ring) / len(ring)
                clon = sum(p[0] for p in ring) / len(ring)
                key = f"{math.floor(clat / CELL_DEG)}_{math.floor(clon / CELL_DEG)}"
                cells.setdefault(key, []).append([round(h, 1), ring])
                n_buildings += 1

        for key, buildings in cells.items():
            with open(os.path.join(outdir, f"{key}.json"), "w") as fh:
                json.dump({"b": buildings}, fh, separators=(",", ":"))

    print(f"wrote {n_buildings} building rings into {len(cells)} cells at {outdir}")


if __name__ == "__main__":
    main()
