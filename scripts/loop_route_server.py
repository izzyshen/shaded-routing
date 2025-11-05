#!/usr/bin/env python3
"""
Dev server that serves map.html on http://localhost:5500/map.html
and exposes an API to build loop routes backed by Valhalla.

Endpoints:
  GET  /map.html                 -> serves project root map.html
  GET  /                         -> redirects to /map.html
  POST /api/loop_route           -> builds a loop route and returns GeoJSON

POST /api/loop_route request JSON:
{
  "start_lat": 40.7486,
  "start_lon": -73.9864,
  "distance_m": 3000,
  "max_candidates": 8,           // optional
  "tolerance": 0.15,             // optional
  "snap_radius": 75,             // optional
  "valhalla_url": "http://localhost:8002" // optional
}

Response: GeoJSON FeatureCollection with LineString and length_m property.
"""

from __future__ import annotations

import json
import os
from typing import Any, Dict

from flask import Flask, jsonify, redirect, request, send_from_directory

from scripts.loop_route_orchestrator import (
    ValhallaClient,
    build_walking_loop,
    default_pedestrian_costing_options,
    default_pedestrian_search_filter,
    to_geojson,
)


ROOT_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), os.pardir))
app = Flask(__name__, static_folder=ROOT_DIR)


@app.get("/")
def root():
    return redirect("/map.html", code=302)


@app.get("/map.html")
def serve_map():
    return send_from_directory(ROOT_DIR, "map.html")


@app.post("/api/loop_route")
def api_loop_route():
    try:
        body: Dict[str, Any] = request.get_json(force=True, silent=False) or {}
    except Exception:
        return jsonify({"error": "invalid_json"}), 400

    try:
        start_lat = float(body.get("start_lat"))
        start_lon = float(body.get("start_lon"))
        distance_m = float(body.get("distance_m"))
    except Exception:
        return jsonify({"error": "missing_or_invalid_parameters"}), 400

    max_candidates = int(body.get("max_candidates", 8))
    tolerance = float(body.get("tolerance", 0.15))
    snap_radius = int(body.get("snap_radius", 75))
    valhalla_url = str(body.get("valhalla_url", os.environ.get("VALHALLA_URL", "http://localhost:8002")))

    client = ValhallaClient(valhalla_url)
    route_json = build_walking_loop(
        valhalla=client,
        start_lat=start_lat,
        start_lon=start_lon,
        desired_distance_m=distance_m,
        max_number_of_candidates=max_candidates,
        tolerance_ratio=tolerance,
        candidate_snap_radius_m=snap_radius,
        search_filter=default_pedestrian_search_filter(),
        costing_options=default_pedestrian_costing_options(),
    )

    if not route_json:
        return jsonify({"error": "no_route"}), 422

    return jsonify(to_geojson(route_json))


def main() -> None:
    port = int(os.environ.get("PORT", "5500"))
    # Bind to localhost only for dev; change host to "0.0.0.0" if needed
    app.run(host="127.0.0.1", port=port, debug=True)


if __name__ == "__main__":
    main()


