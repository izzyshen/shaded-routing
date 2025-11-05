#!/usr/bin/env python3
"""
Valhalla Alternate Routes API 사용 예제

이 스크립트는 Valhalla 서비스의 alternate routes 기능을 사용하는 방법을 보여줍니다.
"""

import requests
import json
from typing import List, Dict, Any

# Valhalla 서비스 URL
VALHALLA_URL = "http://localhost:8002/route"


def request_alternate_routes(
    origin: Dict[str, float],
    destination: Dict[str, float],
    num_alternates: int = 2,
    costing: str = "auto",
    costing_options: Dict[str, Any] = None
) -> Dict[str, Any]:
    """
    Alternate routes를 요청하는 함수
    
    Args:
        origin: 출발지 좌표 {"lat": float, "lon": float}
        destination: 목적지 좌표 {"lat": float, "lon": float}
        num_alternates: 요청할 대체 경로 개수 (기본값: 2)
        costing: 코스팅 방법 ("auto", "bicycle", "pedestrian", "truck" 등)
        costing_options: 코스팅 옵션 (선택사항)
    
    Returns:
        API 응답 JSON
    """
    payload = {
        "locations": [
            {"lat": origin["lat"], "lon": origin["lon"]},
            {"lat": destination["lat"], "lon": destination["lon"]}
        ],
        "costing": costing,
        "alternates": num_alternates,
        "directions_options": {
            "units": "kilometers"
        }
    }
    
    if costing_options:
        payload["costing_options"] = costing_options
    
    response = requests.post(VALHALLA_URL, json=payload)
    response.raise_for_status()
    
    return response.json()


def print_route_summary(response: Dict[str, Any]):
    """
    경로 요약 정보 출력
    """
    trip = response.get("trip", {})
    routes = trip.get("routes", [])
    
    print(f"\n총 {len(routes)}개의 경로를 찾았습니다:\n")
    
    for i, route in enumerate(routes):
        legs = route.get("legs", [])
        if not legs:
            continue
        
        leg = legs[0]
        summary = leg.get("summary", {})
        
        print(f"경로 {i + 1}:")
        print(f"  - 거리: {summary.get('length', 0):.2f} km")
        print(f"  - 시간: {summary.get('time', 0) / 60:.1f} 분")
        print(f"  - 최소 위도: {summary.get('min_lat', 0)}")
        print(f"  - 최대 위도: {summary.get('max_lat', 0)}")
        
        maneuvers = leg.get("maneuvers", [])
        print(f"  - 교차로 수: {len(maneuvers)}")
        print()


def compare_routes(response: Dict[str, Any]):
    """
    여러 경로를 비교하여 출력
    """
    trip = response.get("trip", {})
    routes = trip.get("routes", [])
    
    if len(routes) < 2:
        print("비교할 경로가 충분하지 않습니다.")
        return
    
    print("\n=== 경로 비교 ===")
    print(f"{'경로':<10} {'거리 (km)':<15} {'시간 (분)':<15} {'교차로 수':<15}")
    print("-" * 60)
    
    for i, route in enumerate(routes):
        legs = route.get("legs", [])
        if not legs:
            continue
        
        leg = legs[0]
        summary = leg.get("summary", {})
        maneuvers = leg.get("maneuvers", [])
        
        route_name = "최적 경로" if i == 0 else f"대체 경로 {i}"
        
        print(f"{route_name:<10} "
              f"{summary.get('length', 0):>10.2f} km  "
              f"{summary.get('time', 0) / 60:>10.1f} 분  "
              f"{len(maneuvers):>10}")
    
    print()


def main():
    """
    메인 실행 함수
    """
    print("=== Valhalla Alternate Routes API 예제 ===\n")
    
    # 예제 1: 기본 alternate routes 요청
    print("예제 1: 기본 alternate routes (2개)")
    origin = {"lat": 52.111893, "lon": 5.125282}
    destination = {"lat": 52.113731, "lon": 5.091155}
    
    try:
        response = request_alternate_routes(
            origin=origin,
            destination=destination,
            num_alternates=2,
            costing="auto"
        )
        
        print_route_summary(response)
        compare_routes(response)
        
    except requests.exceptions.RequestException as e:
        print(f"요청 실패: {e}")
        print("Valhalla 서비스가 http://localhost:8002에서 실행 중인지 확인하세요.")
        return
    
    # 예제 2: 자전거 코스팅으로 alternate routes
    print("\n예제 2: 자전거 코스팅으로 alternate routes")
    try:
        response = request_alternate_routes(
            origin=origin,
            destination=destination,
            num_alternates=1,
            costing="bicycle",
            costing_options={
                "bicycle": {
                    "use_roads": 0.5,
                    "use_tracks": 0.5
                }
            }
        )
        
        print_route_summary(response)
        
    except requests.exceptions.RequestException as e:
        print(f"요청 실패: {e}")
    
    # 예제 3: 코스팅 옵션을 사용한 alternate routes
    print("\n예제 3: 코스팅 옵션 조정 (고속도로 회피)")
    try:
        response = request_alternate_routes(
            origin=origin,
            destination=destination,
            num_alternates=2,
            costing="auto",
            costing_options={
                "auto": {
                    "highway_factor": 0.5,  # 고속도로 선호도 감소
                    "use_distance": 0.4,   # 거리 가중치 증가
                    "use_time": 0.6        # 시간 가중치 감소
                }
            }
        )
        
        print_route_summary(response)
        compare_routes(response)
        
    except requests.exceptions.RequestException as e:
        print(f"요청 실패: {e}")


if __name__ == "__main__":
    main()

