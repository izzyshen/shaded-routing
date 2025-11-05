#!/bin/bash

# Alternate Routes API 사용 예제
# Valhalla 서비스가 http://localhost:8002에서 실행 중이어야 합니다

echo "=== Alternate Routes API 테스트 ==="
echo ""

# 1. 기본 alternate routes 요청 (2개의 대체 경로)
echo "1. 2개의 대체 경로 요청:"
curl -X POST http://localhost:8002/route \
  -H "Content-Type: application/json" \
  -d '{
    "locations": [
      {"lat": 52.111893, "lon": 5.125282},
      {"lat": 52.113731, "lon": 5.091155}
    ],
    "costing": "auto",
    "alternates": 2
  }' | jq '.trip.routes | length'

echo ""
echo "2. 자전거 코스팅으로 alternate routes:"
curl -X POST http://localhost:8002/route \
  -H "Content-Type: application/json" \
  -d '{
    "locations": [
      {"lat": 52.111893, "lon": 5.125282},
      {"lat": 52.113731, "lon": 5.091155}
    ],
    "costing": "bicycle",
    "alternates": 1,
    "costing_options": {
      "bicycle": {
        "use_roads": 0.5
      }
    }
  }' | jq '.trip.routes | length'

echo ""
echo "3. 최대 3개의 alternate routes:"
curl -X POST http://localhost:8002/route \
  -H "Content-Type: application/json" \
  -d '{
    "locations": [
      {"lat": 52.111893, "lon": 5.125282},
      {"lat": 52.113731, "lon": 5.091155}
    ],
    "costing": "auto",
    "alternates": 3,
    "directions_options": {
      "units": "kilometers"
    }
  }' | jq '.trip.routes | length'

echo ""
echo "=== 테스트 완료 ==="

