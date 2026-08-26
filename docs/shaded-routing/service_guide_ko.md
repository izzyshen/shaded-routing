# Valhalla 서비스 실행 가이드

Valhalla 서비스를 실행하는 방법을 단계별로 설명합니다.

## 전제 조건

1. **Valhalla가 빌드되어 있어야 합니다**
2. **타일 데이터가 준비되어 있어야 합니다** (routing graph tiles)
3. **설정 파일(valhalla.json)이 필요합니다**

## 빠른 시작

### 1. 설정 파일 생성

먼저 `valhalla.json` 설정 파일을 생성해야 합니다. `valhalla_build_config` 스크립트를 사용합니다:

```bash
# 기본 설정 파일 생성
valhalla_build_config \
  --mjolnir-tile-dir ./valhalla_tiles \
  --mjolnir-tile-extract ./valhalla_tiles/tiles.tar \
  > valhalla.json
```

또는 더 많은 옵션을 포함하여:

```bash
valhalla_build_config \
  --mjolnir-tile-dir ./valhalla_tiles \
  --mjolnir-tile-extract ./valhalla_tiles/tiles.tar \
  --mjolnir-admin ./valhalla_tiles/admin.sqlite \
  --mjolnir-timezone ./valhalla_tiles/tz_world.sqlite \
  --additional-data-elevation ./valhalla_tiles/elevation/ \
  > valhalla.json
```

### 2. 타일 데이터 준비

타일 데이터가 없다면 먼저 생성해야 합니다:

```bash
# 1. OpenStreetMap 데이터 다운로드 (예: Geofabrik에서)
# https://download.geofabrik.de/

# 2. 타일 빌드
valhalla_build_tiles -c valhalla.json your_region.osm.pbf

# 3. (선택사항) 타일 압축
valhalla_build_extract -c valhalla.json
```

### 3. 서비스 실행

설정 파일이 준비되면 서비스를 실행할 수 있습니다:

```bash
# 기본 실행 (CPU 코어 수만큼 스레드 사용)
valhalla_service valhalla.json

# 또는 특정 스레드 수 지정
valhalla_service valhalla.json 4

# 백그라운드로 실행
valhalla_service valhalla.json 4 &
```

서비스는 기본적으로 `http://localhost:8002`에서 실행됩니다.

### 4. 서비스 확인

서비스가 정상적으로 실행 중인지 확인:

```bash
# 상태 확인
curl http://localhost:8002/status

# 또는 브라우저에서
# http://localhost:8002/status
```

## 테스트용 빠른 실행 방법

개발/테스트 목적이라면, 프로젝트에 포함된 테스트 데이터를 사용할 수 있습니다:

```bash
# 테스트 설정 파일 사용
valhalla_service test/win/valhalla.json 2
```

이 설정은 `test/data/utrecht_tiles` 디렉토리의 테스트 타일을 사용합니다.

## Docker 사용 (권장)

가장 간단한 방법은 Docker를 사용하는 것입니다:

```bash
# Docker 이미지 다운로드 및 실행
docker run -d \
  -p 8002:8002 \
  -v $(pwd)/valhalla_tiles:/data/valhalla \
  ghcr.io/gis-ops/valhalla:latest \
  valhalla_service /custom_files/valhalla.json
```

## 설정 파일 구조

설정 파일의 주요 설정:

```json
{
  "mjolnir": {
    "tile_dir": "/data/valhalla",           // 타일 디렉토리 경로
    "tile_extract": "/data/valhalla/tiles.tar",  // 압축된 타일 (선택사항)
    "admin": "/data/valhalla/admin.sqlite",      // 관리 경계 데이터베이스
    "timezone": "/data/valhalla/tz_world.sqlite"  // 타임존 데이터베이스
  },
  "httpd": {
    "service": {
      "listen": "tcp://*:8002"              // 서버 포트
    }
  },
  "loki": {
    "actions": [
      "locate", "route", "isochrone", ...   // 사용 가능한 API 액션
    ]
  }
}
```

## 문제 해결

### "Cannot find config file" 오류
- 설정 파일 경로가 올바른지 확인하세요
- `valhalla.json` 파일이 존재하는지 확인하세요

### "Cannot find tile directory" 오류
- `mjolnir.tile_dir` 경로가 올바른지 확인하세요
- 타일 디렉토리에 타일 파일이 있는지 확인하세요

### 포트 8002가 이미 사용 중
- 다른 포트를 사용하거나 실행 중인 프로세스를 종료하세요:
  ```bash
  # 포트 사용 확인
  lsof -i :8002
  # 또는
  netstat -an | grep 8002
  ```

### 서비스가 시작되지 않음
- 설정 파일의 JSON 형식이 올바른지 확인하세요
- 로그를 확인하여 오류 메시지를 확인하세요
- 타일 데이터가 올바르게 빌드되었는지 확인하세요

## Isochrone 시각화 도구와 함께 사용

1. Valhalla 서비스 실행:
   ```bash
   valhalla_service valhalla.json 4
   ```

2. 브라우저에서 `isochrone_visualizer.html` 열기

3. 서버 주소를 `http://localhost:8002`로 설정 (기본값)

4. "서버 연결 테스트" 버튼으로 연결 확인

## 추가 리소스

- [Valhalla 공식 문서](https://valhalla.github.io/valhalla/)
- [빌드 가이드](https://valhalla.github.io/valhalla/building/)
- [타일 생성 가이드](https://valhalla.github.io/valhalla/mjolnir/getting_started_guide/)

