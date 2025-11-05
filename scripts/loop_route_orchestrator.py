#!/usr/bin/env python3
"""
Loop Route Orchestrator for Valhalla (Pedestrian)

This script builds a walking loop that starts and ends at the same point
with total length near the desired distance by:
  - Sampling candidate via points on a ring around the start
  - Snapping candidates to the Valhalla routable network via /locate
  - Building multi-leg routes via /route (start -> vias... -> start)
  - Scoring routes by distance error and 'loopiness'

Usage (example):
  python scripts/loop_route_orchestrator.py \
    --start-lat 40.7486 --start-lon -73.9864 \
    --distance-m 3000 \
    --valhalla-url http://localhost:8002 \
    --max-candidates 10 \
    --tolerance 0.15 \
    --output geojson

Requires: requests
  pip install requests
"""

from __future__ import annotations

import argparse
import json
import math
import os
import random
import sys
from dataclasses import dataclass
from typing import Any, Dict, List, Optional, Sequence, Tuple

try:
    import requests
except Exception as exc:  # pragma: no cover
    print("This script requires the 'requests' package. Install with: pip install requests", file=sys.stderr)
    raise


R_EARTH_M = 6_371_000.0
POLYLINE6_FACTOR = 1e6


@dataclass
class SnappedLocation:
    lat: float
    lon: float
    radius: int
    search_filter: Optional[Dict[str, Any]] = None

    def to_location_json(self) -> Dict[str, Any]:
        location = {"lat": self.lat, "lon": self.lon, "radius": self.radius}
        if self.search_filter:
            location["search_filter"] = self.search_filter
        return location


def offset_from_bearing(lat_deg: float, lon_deg: float, distance_m: float, bearing_rad: float) -> Tuple[float, float]:
    lat0 = math.radians(lat_deg)
    lon0 = math.radians(lon_deg)
    ang = distance_m / R_EARTH_M
    lat1 = math.asin(math.sin(lat0) * math.cos(ang) + math.cos(lat0) * math.sin(ang) * math.cos(bearing_rad))
    lon1 = lon0 + math.atan2(
        math.sin(bearing_rad) * math.sin(ang) * math.cos(lat0),
        math.cos(ang) - math.sin(lat0) * math.sin(lat1),
    )
    return (math.degrees(lat1), (math.degrees(lon1) + 540.0) % 360.0 - 180.0)


def decode_polyline6(encoded: str) -> List[Tuple[float, float]]:
    coords: List[Tuple[float, float]] = []
    index = 0
    lat = 0
    lon = 0
    length = len(encoded)

    while index < length:
        result = 0
        shift = 0
        while True:
            b = ord(encoded[index]) - 63
            index += 1
            result |= (b & 0x1F) << shift
            shift += 5
            if b < 0x20:
                break
        dlat = ~(result >> 1) if (result & 1) else (result >> 1)
        lat += dlat

        result = 0
        shift = 0
        while True:
            b = ord(encoded[index]) - 63
            index += 1
            result |= (b & 0x1F) << shift
            shift += 5
            if b < 0x20:
                break
        dlon = ~(result >> 1) if (result & 1) else (result >> 1)
        lon += dlon

        coords.append((lat / POLYLINE6_FACTOR, lon / POLYLINE6_FACTOR))

    return coords


def vector_heading_deg(a: Tuple[float, float], b: Tuple[float, float]) -> float:
    dlat = math.radians(b[0] - a[0])
    dlon = math.radians(b[1] - a[1])
    lat1 = math.radians(a[0])
    y = math.sin(dlon) * math.cos(math.radians(b[0]))
    x = math.cos(lat1) * math.sin(math.radians(b[0])) - math.sin(lat1) * math.cos(math.radians(b[0])) * math.cos(dlon)
    brng = math.degrees(math.atan2(y, x))
    return (brng + 360.0) % 360.0


def angular_separation_deg(h1: float, h2: float) -> float:
    diff = abs(h1 - h2) % 360.0
    return diff if diff <= 180.0 else 360.0 - diff


class ValhallaClient:
    def __init__(self, base_url: str, timeout_s: float = 20.0) -> None:
        self.base_url = base_url.rstrip("/")
        self.timeout_s = timeout_s

    def locate(self, lat: float, lon: float, radius_m: int, search_filter: Optional[Dict[str, Any]] = None) -> Optional[Tuple[float, float]]:
        url = f"{self.base_url}/locate"
        body: Dict[str, Any] = {"locations": [{"lat": lat, "lon": lon, "radius": radius_m}]}
        if search_filter:
            body["locations"][0]["search_filter"] = search_filter
        try:
            resp = requests.post(url, json=body, timeout=self.timeout_s)
            resp.raise_for_status()
            data = resp.json()
        except Exception:
            return None

        # Response can be a list per input location; use first candidate if present
        if isinstance(data, list) and data:
            cand = data[0]
            lat_snapped = cand.get("lat")
            lon_snapped = cand.get("lon")
            if isinstance(lat_snapped, (int, float)) and isinstance(lon_snapped, (int, float)):
                return float(lat_snapped), float(lon_snapped)

        # Some Valhalla builds return dict with key 'results'
        if isinstance(data, dict) and "results" in data and data["results"]:
            cand = data["results"][0]
            loc = cand.get("location") or {}
            lat_snapped = loc.get("lat")
            lon_snapped = loc.get("lon")
            if isinstance(lat_snapped, (int, float)) and isinstance(lon_snapped, (int, float)):
                return float(lat_snapped), float(lon_snapped)

        return None

    def route(self, locations: Sequence[SnappedLocation], costing: str = "pedestrian", costing_options: Optional[Dict[str, Any]] = None, osrm: bool = False, units: str = "kilometers") -> Optional[Dict[str, Any]]:
        url = f"{self.base_url}/route"
        body: Dict[str, Any] = {
            "locations": [loc.to_location_json() for loc in locations],
            "costing": costing,
            "directions_options": {"units": "kilometers" if units not in ("miles", "kilometers") else units},
        }
        if costing_options:
            body["costing_options"] = costing_options
        if osrm:
            body["format"] = "osrm"

        try:
            resp = requests.post(url, json=body, timeout=self.timeout_s)
            resp.raise_for_status()
            return resp.json()
        except Exception:
            return None


def extract_total_length_m(route_json: Dict[str, Any]) -> Optional[float]:
    # Valhalla default format: trip.summary.length in kilometers
    try:
        if "trip" in route_json:
            length_km = float(route_json["trip"]["summary"]["length"])  # kilometers
            return length_km * 1000.0
        # OSRM format or other variants can be handled here if needed
    except Exception:
        pass
    return None


def extract_shape(route_json: Dict[str, Any]) -> Optional[str]:
    try:
        if "trip" in route_json and route_json["trip"].get("legs"):
            # Concatenate shapes of all legs to get full loop
            shapes = [leg.get("shape") for leg in route_json["trip"]["legs"] if leg.get("shape")]
            if not shapes:
                return None
            # Shapes are polyline6-encoded per leg; concatenate decoded and re-encode is expensive
            # For loopiness scoring, we'll decode each leg separately
            return "|".join(shapes)  # sentinel-joined for later decoding per leg
    except Exception:
        pass
    return None


def compute_loopiness_from_shape(encoded_multi: str) -> float:
    # Higher is better. Approximate as sum of absolute heading changes after decimation.
    legs = encoded_multi.split("|")
    points: List[Tuple[float, float]] = []
    for enc in legs:
        try:
            pts = decode_polyline6(enc)
            if points and pts:
                # Avoid duplicating junction point
                if points[-1] == pts[0]:
                    points.extend(pts[1:])
                else:
                    points.extend(pts)
            else:
                points.extend(pts)
        except Exception:
            continue

    if len(points) < 8:
        return 0.0

    # Decimate to every Nth point to reduce noise
    step = max(1, len(points) // 200)
    pts = points[::step]
    if len(pts) < 3:
        return 0.0

    headings: List[float] = []
    for i in range(len(pts) - 1):
        headings.append(vector_heading_deg(pts[i], pts[i + 1]))
    score = 0.0
    for i in range(1, len(headings)):
        score += angular_separation_deg(headings[i - 1], headings[i])
    return score


def choose_vias(primary: SnappedLocation, candidates: Sequence[SnappedLocation], k: int, start_lat: float, start_lon: float, min_angle_sep_deg: float = 50.0, min_spacing_m: float = 80.0) -> List[SnappedLocation]:
    if k <= 0:
        return [primary]

    def bearing_from_start(loc: SnappedLocation) -> float:
        return vector_heading_deg((start_lat, start_lon), (loc.lat, loc.lon))

    selected: List[SnappedLocation] = [primary]
    sel_bearings: List[float] = [bearing_from_start(primary)]

    pool = [c for c in candidates if c is not primary]
    random.shuffle(pool)

    for cand in pool:
        if len(selected) >= k + 1:
            break
        b = bearing_from_start(cand)
        if all(angular_separation_deg(b, sb) >= min_angle_sep_deg for sb in sel_bearings):
            # Optional: spacing filter (approx using simple haversine)
            ok = True
            for s in selected:
                if haversine_m((cand.lat, cand.lon), (s.lat, s.lon)) < min_spacing_m:
                    ok = False
                    break
            if ok:
                selected.append(cand)
                sel_bearings.append(b)

    return selected


def haversine_m(a: Tuple[float, float], b: Tuple[float, float]) -> float:
    lat1, lon1 = map(math.radians, a)
    lat2, lon2 = map(math.radians, b)
    dlat = lat2 - lat1
    dlon = lon2 - lon1
    h = math.sin(dlat / 2) ** 2 + math.cos(lat1) * math.cos(lat2) * math.sin(dlon / 2) ** 2
    return 2 * R_EARTH_M * math.asin(math.sqrt(h))


def build_walking_loop(
    valhalla: ValhallaClient,
    start_lat: float,
    start_lon: float,
    desired_distance_m: float,
    max_number_of_candidates: int = 8,
    tolerance_ratio: float = 0.15,
    extra_via_count_options: Sequence[int] = (0, 1, 2),
    candidate_snap_radius_m: int = 75,
    search_filter: Optional[Dict[str, Any]] = None,
    costing_options: Optional[Dict[str, Any]] = None,
) -> Optional[Dict[str, Any]]:
    if desired_distance_m <= 0:
        return None

    # 1) Derive sampling ring radii
    radius_min = 0.33 * desired_distance_m
    radius_max = 0.50 * desired_distance_m
    if radius_max <= 0:
        return None

    # 2) Snap start
    snapped_start = valhalla.locate(start_lat, start_lon, candidate_snap_radius_m, search_filter)
    if not snapped_start:
        return None
    start_loc = SnappedLocation(snapped_start[0], snapped_start[1], candidate_snap_radius_m, search_filter)

    # 3) Generate and snap candidates
    snapped_candidates: List[SnappedLocation] = []
    for _ in range(max_number_of_candidates * 2):  # oversample, we may discard
        if len(snapped_candidates) >= max_number_of_candidates:
            break
        r = random.uniform(radius_min, radius_max)
        theta = random.uniform(0.0, 2.0 * math.pi)
        cand_lat, cand_lon = offset_from_bearing(start_lat, start_lon, r, theta)
        snapped = valhalla.locate(cand_lat, cand_lon, candidate_snap_radius_m, search_filter)
        if not snapped:
            continue
        snapped_loc = SnappedLocation(snapped[0], snapped[1], candidate_snap_radius_m, search_filter)
        # Avoid near-duplicates
        if all(haversine_m((snapped_loc.lat, snapped_loc.lon), (c.lat, c.lon)) > 50.0 for c in snapped_candidates):
            snapped_candidates.append(snapped_loc)

    if not snapped_candidates:
        return None

    target = desired_distance_m
    tol = tolerance_ratio * desired_distance_m

    # 4) Try building routes
    candidate_routes: List[Tuple[Tuple[float, float, float], Dict[str, Any]]] = []

    random.shuffle(snapped_candidates)
    for primary in snapped_candidates:
        for k in extra_via_count_options:
            vias = choose_vias(primary, snapped_candidates, k, start_loc.lat, start_loc.lon)
            locations = [start_loc] + vias + [start_loc]

            route_json = valhalla.route(locations, costing="pedestrian", costing_options=costing_options, osrm=False)
            if not route_json:
                continue
            total_length_m = extract_total_length_m(route_json)
            if total_length_m is None:
                continue
            error = abs(total_length_m - target)
            if error <= tol:
                encoded = extract_shape(route_json)
                if not encoded:
                    continue
                loopiness = compute_loopiness_from_shape(encoded)
                # overlap_penalty could be a proxy using small loopiness => higher penalty
                overlap_penalty = 36000.0 / (loopiness + 1.0)
                score = (error, -loopiness, overlap_penalty)
                candidate_routes.append((score, route_json))

    # 5) Fallback: no route within tolerance, keep closest-length among basic shapes
    if not candidate_routes:
        best: Optional[Tuple[float, Dict[str, Any]]] = None
        for primary in snapped_candidates:
            locations = [start_loc, primary, start_loc]
            route_json = valhalla.route(locations, costing="pedestrian", costing_options=costing_options, osrm=False)
            if not route_json:
                continue
            total_length_m = extract_total_length_m(route_json)
            if total_length_m is None:
                continue
            error = abs(total_length_m - target)
            if best is None or error < best[0]:
                best = (error, route_json)
        return best[1] if best else None

    # 6) Rank and return best
    candidate_routes.sort(key=lambda x: x[0])
    return candidate_routes[0][1]


def default_pedestrian_search_filter() -> Dict[str, Any]:
    return {
        "max_road_class": "residential",
        "exclude_motorway": True,
    }


def default_pedestrian_costing_options() -> Dict[str, Any]:
    return {
        "pedestrian": {
            "alley_factor": 1.0,
            "use_ferry": 0.0,
            "walkway_factor": 0.9,
        }
    }


def to_geojson(route_json: Dict[str, Any]) -> Dict[str, Any]:
    # Convert trip legs to a single LineString geometry
    shape_concat = extract_shape(route_json)
    if not shape_concat:
        return {"type": "FeatureCollection", "features": []}
    coords: List[List[float]] = []
    for part in shape_concat.split("|"):
        for lat, lon in decode_polyline6(part):
            if coords and coords[-1] == [lon, lat]:
                continue
            coords.append([lon, lat])

    props = {}
    length_m = extract_total_length_m(route_json)
    if length_m is not None:
        props["length_m"] = round(length_m, 1)

    return {
        "type": "FeatureCollection",
        "features": [
            {
                "type": "Feature",
                "geometry": {"type": "LineString", "coordinates": coords},
                "properties": props,
            }
        ],
    }


def main() -> None:
    parser = argparse.ArgumentParser(description="Build a pedestrian loop route via Valhalla")
    parser.add_argument("--start-lat", type=float, required=True)
    parser.add_argument("--start-lon", type=float, required=True)
    parser.add_argument("--distance-m", type=float, required=True)
    parser.add_argument("--valhalla-url", type=str, default=os.environ.get("VALHALLA_URL", "http://localhost:8002"))
    parser.add_argument("--max-candidates", type=int, default=8)
    parser.add_argument("--tolerance", type=float, default=0.15, help="±ratio for distance match")
    parser.add_argument("--snap-radius", type=int, default=75)
    parser.add_argument("--output", choices=["json", "geojson"], default="json")
    parser.add_argument("--seed", type=int, default=None)
    args = parser.parse_args()

    if args.seed is not None:
        random.seed(args.seed)

    client = ValhallaClient(args.valhalla_url)
    route_json = build_walking_loop(
        valhalla=client,
        start_lat=args.start_lat,
        start_lon=args.start_lon,
        desired_distance_m=args.distance_m,
        max_number_of_candidates=args.max_candidates,
        tolerance_ratio=args.tolerance,
        candidate_snap_radius_m=args.snap_radius,
        search_filter=default_pedestrian_search_filter(),
        costing_options=default_pedestrian_costing_options(),
    )

    if not route_json:
        print(json.dumps({"error": "no_route"}))
        sys.exit(2)

    if args.output == "geojson":
        print(json.dumps(to_geojson(route_json)))
    else:
        print(json.dumps(route_json))


if __name__ == "__main__":
    main()


