# Alternate Routes API 사용 가이드

Valhalla의 Alternate Routes API를 사용하여 다양한 경로를 생성하는 방법을 설명합니다.

## 개요

Alternate Routes는 출발지와 목적지 사이의 최적 경로뿐만 아니라 여러 대체 경로를 제공합니다. 각 대체 경로는 다음 기준을 만족합니다:

- **Stretch (신장)**: 최적 경로의 1.25배 이하 (거리에 따라 조정)
- **Sharing (공유)**: 최대 75%만 최적 경로와 공유 (10km 이하면 60%)
- **Local Optimality**: 불합리한 우회 제거 (최대 2배 이하)

## 기본 사용법

### 1. JSON 요청 예제

```json
{
  "locations": [
    {"lat": 52.111893, "lon": 5.125282},
    {"lat": 52.113731, "lon": 5.091155}
  ],
  "costing": "auto",
  "alternates": 2
}
```

### 2. cURL 예제

```bash
curl -X POST http://localhost:8002/route \
  -H "Content-Type: application/json" \
  -d '{
    "locations": [
      {"lat": 52.111893, "lon": 5.125282},
      {"lat": 52.113731, "lon": 5.091155}
    ],
    "costing": "auto",
    "alternates": 2
  }'
```

### 3. Python 예제

```python
import requests

response = requests.post("http://localhost:8002/route", json={
    "locations": [
        {"lat": 52.111893, "lon": 5.125282},
        {"lat": 52.113731, "lon": 5.091155}
    ],
    "costing": "auto",
    "alternates": 2
})

data = response.json()
print(f"총 {len(data['trip']['routes'])}개의 경로를 찾았습니다")
```

## 파라미터 설명

### `alternates` (필수)

- **타입**: 정수 (0 이상)
- **설명**: 요청할 대체 경로의 최대 개수
- **기본값**: 0 (대체 경로 없음)
- **최대값**: 설정 파일의 `service_limits.max_alternates`에 따라 제한됨

### `costing` (필수)

사용할 코스팅 방법:
- `auto`: 자동차
- `bicycle`: 자전거
- `pedestrian`: 도보
- `truck`: 트럭
- `taxi`: 택시
- `bus`: 버스
- `multimodal`: 대중교통 포함

## 응답 구조

```json
{
  "trip": {
    "routes": [
      {
        "legs": [
          {
            "summary": {
              "length": 5.234,
              "time": 420,
              "min_lat": 52.111,
              "max_lat": 52.114
            },
            "maneuvers": [...],
            "shape": "encoded_polyline"
          }
        ]
      }
    ]
  },
  "alternates": [
    {
      "trip": {
        "routes": [...]
      }
    }
  ]
}
```

- `trip.routes[0]`: 최적 경로
- `trip.routes[1..N]`: 대체 경로들 (있을 경우)
- `alternates`: 대체 경로 (별도 필드, 경우에 따라 다름)

## 고급 사용법

### 1. 코스팅 옵션 조정

거리와 시간의 가중치를 조정하여 다른 경로를 얻을 수 있습니다:

```json
{
  "locations": [...],
  "costing": "auto",
  "alternates": 2,
  "costing_options": {
    "auto": {
      "use_distance": 0.4,    // 거리 가중치 증가
      "use_time": 0.6,         // 시간 가중치 감소
      "highway_factor": 0.7,   // 고속도로 선호도 감소
      "service_factor": 1.3    // 서비스 도로 페널티
    }
  }
}
```

### 2. 특정 도로 타입 회피

```json
{
  "locations": [...],
  "costing": "auto",
  "alternates": 2,
  "costing_options": {
    "auto": {
      "exclude_tolls": true,
      "exclude_highways": false,
      "exclude_ferries": true
    }
  }
}
```

### 3. 자전거 코스팅으로 더 많은 경로

자전거는 더 많은 도로를 사용할 수 있어 더 다양한 경로를 생성할 수 있습니다:

```json
{
  "locations": [...],
  "costing": "bicycle",
  "alternates": 3,
  "costing_options": {
    "bicycle": {
      "use_roads": 0.5,
      "use_tracks": 0.5,
      "use_hills": 0.3
    }
  }
}
```

## 제한사항

1. **다중 경유지**: 현재 alternate routes는 2개의 위치(출발지, 목적지)만 지원합니다.
2. **시간 의존적 라우팅**: `date_time`이 설정된 경우 alternate routes가 지원되지 않을 수 있습니다.
3. **최대 개수**: 설정 파일의 `service_limits.max_alternates`에 따라 제한됩니다.

## 예제 파일

- `alternate_routes_example.json`: 기본 JSON 요청 예제
- `alternate_routes_curl.sh`: cURL 명령어 예제
- `alternate_routes_python.py`: Python 스크립트 예제

## 실행 방법

### 1. Valhalla 서비스 시작

```bash
valhalla_service valhalla.json 4
```

### 2. cURL 스크립트 실행

```bash
chmod +x examples/alternate_routes_curl.sh
./examples/alternate_routes_curl.sh
```

### 3. Python 스크립트 실행

```bash
pip install requests  # 필요시
python examples/alternate_routes_python.py
```

## 참고 자료

- [Valhalla API 문서](https://valhalla.github.io/valhalla/api/turn-by-turn/api-reference/)
- [Alternate Routes 구현](src/thor/alternates.cc)
- [Bidirectional A* 알고리즘](src/thor/bidirectional_astar.cc)

