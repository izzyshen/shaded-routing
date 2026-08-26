# Isochrone 시각화 도구 사용 가이드

이 도구는 Valhalla의 isochrone API를 사용하여 특정 위치에서 주어진 시간 내에 도달 가능한 영역을 시각화하는 웹 인터페이스입니다.

## 기능

- 🗺️ 인터랙티브 지도에서 위치 선택
- ⏱️ 여러 시간 간격 입력 (예: 15분, 30분, 45분, 60분)
- 🚗 다양한 이동 수단 선택 (자동차, 도보, 자전거, 대중교통)
- 🎨 각 컨투어에 대한 색상 커스터마이징
- 📊 GeoJSON 형식의 isochrone 결과를 지도에 표시

## 사용 방법

### 1. Valhalla 서비스 실행

먼저 Valhalla 서비스가 실행 중이어야 합니다. 기본적으로 `localhost:8002`에서 실행됩니다.

```bash
# 예시: Valhalla 서비스 실행
valhalla_service valhalla.json 4
```

서비스가 다른 포트나 주소에서 실행 중이라면, 사이드바의 "Valhalla 서버 주소" 필드에서 변경할 수 있습니다.

### 2. 웹 브라우저에서 열기

`isochrone_visualizer.html` 파일을 웹 브라우저에서 엽니다:

```bash
# macOS/Linux
open isochrone_visualizer.html

# 또는 직접 브라우저에서 파일 열기
```

### 3. 사용 단계

1. **위치 선택**: 지도를 클릭하여 isochrone의 중심점을 선택합니다.
2. **이동 수단 선택**: 드롭다운 메뉴에서 원하는 이동 수단을 선택합니다.
   - 자동차 (Auto)
   - 도보 (Pedestrian)
   - 자전거 (Bicycle)
   - 대중교통 (Multimodal)
3. **시간 입력**: 도달 가능한 시간을 분 단위로 입력합니다.
   - 기본적으로 하나의 시간 입력 필드가 제공됩니다.
   - "+ 시간 추가" 버튼을 클릭하여 여러 시간 간격을 추가할 수 있습니다.
   - 각 시간에 대해 색상을 지정할 수 있습니다.
4. **서버 주소 확인**: Valhalla 서비스가 실행 중인 주소를 확인합니다 (기본값: `http://localhost:8002`).
5. **Isochrone 생성**: "Isochrone 생성" 버튼을 클릭합니다.
6. **결과 확인**: 지도에 선택한 시간에 따른 도달 가능 영역이 컬러 폴리곤으로 표시됩니다.

### 4. 지도 초기화

"지도 초기화" 버튼을 클릭하여 모든 isochrone 레이어와 마커를 제거할 수 있습니다.

## API 요청 형식

이 도구는 다음과 같은 형식으로 Valhalla API에 요청을 보냅니다:

```json
{
  "locations": [{
    "lat": 37.5665,
    "lon": 126.9780
  }],
  "costing": "pedestrian",
  "contours": [
    {"time": 15, "color": "ff0000"},
    {"time": 30, "color": "ff8800"}
  ],
  "polygons": true,
  "show_locations": true
}
```

## 요구사항

- 실행 중인 Valhalla 서비스 (`valhalla_service`)
- 최신 웹 브라우저 (Chrome, Firefox, Safari, Edge 등)
- 인터넷 연결 (Leaflet과 OpenStreetMap 타일을 로드하기 위해)

## 문제 해결

### "오류: HTTP error! status: 404"
- Valhalla 서비스가 실행 중인지 확인하세요.
- 서버 주소가 올바른지 확인하세요.

### "오류: Failed to fetch"
- CORS 문제일 수 있습니다. Valhalla 서비스가 CORS를 허용하도록 설정되어 있는지 확인하세요.
- 또는 로컬 파일 대신 간단한 HTTP 서버를 사용하여 HTML 파일을 제공하세요:
  ```bash
  # Python 3
  python3 -m http.server 8080
  # 그 다음 브라우저에서 http://localhost:8080/isochrone_visualizer.html 열기
  ```

### 지도가 표시되지 않음
- 인터넷 연결을 확인하세요 (OpenStreetMap 타일을 로드하기 위해 필요합니다).
- 브라우저 콘솔에서 JavaScript 오류를 확인하세요.

## 참고 자료

- [Valhalla Isochrone API 문서](docs/docs/api/isochrone/api-reference.md)
- [Leaflet 공식 문서](https://leafletjs.com/)

## 라이선스

이 시각화 도구는 Valhalla 프로젝트의 일부로 제공됩니다.

