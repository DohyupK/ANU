https://github.com/user-attachments/assets/bfc36234-ff4a-4007-a5fa-6293c50ce8ba

# 국가 검색 지도 시스템

세계 지도에서 **나라·수도를 검색**하고, 위치를 핀으로 보여 주는 프로그램입니다.  
C언어 + SQLite(DB) + Raylib(화면)로 만들었습니다.

---

## 무엇을 할 수 있나요?

| 기능 | 설명 |
|------|------|
| 검색 | 국가명 또는 수도명 입력 → Enter |
| 핀 | 검색 성공 시 수도 위치에 빨간 핀 |
| 국기 | 검색 결과에 국기 카드 표시 |
| 주요 3도시 | **핀을 클릭**하면 도시 이름·좌표·이미지 |
| DB 목록 | F5로 저장된 국가 목록 (클릭 선택, ESC 닫기) |
| 추가 / 수정 / 삭제 | F2 / F3 / F4 |

처음 실행하면 `countries.csv`, `cities.csv`가 SQLite DB(`countries.db`)로 자동 들어갑니다.

---

## 왜 Raylib을 썼나요?

창·그림·마우스까지 다루는 화면용 라이브러리가 필요하여 **Raylib**를 사용

| 구분 | 담당 |
|------|------|
| **SQLite** | 데이터 저장·검색·CRUD (`countries` / `cities`) |
| **Raylib** | 창, 세계 지도, 핀, 검색창·목록, 국기·도시 이미지, 키보드·마우스 입력 |

**데이터는 SQLite, 지도 앱처럼 보이게 하는 UI는 Raylib**

---

## 필요한 환경

- Windows 10 / 11
- [MinGW-w64](https://www.mingw-w64.org/) (`gcc`, `mingw32-make`)

프로젝트 폴더에 이미 들어 있는 것:

- 소스: `country.c`, `sqlite3.c`, `sqlite3.h`
- 데이터: `countries.csv`, `cities.csv`
- 그래픽 라이브러리: `deps/raylib-…`
- 이미지·폰트: `assets/`

---

## 실행 방법

프로젝트 폴더에서:

```powershell
mingw32-make
.\country.exe
```

또는 한 번에:

```powershell
mingw32-make run
```

창이 뜨면 상단 검색창에 예를 들어 `대한민국` 또는 `서울`을 입력하고 **Enter**를 누르세요.

다시 빌드하거나 DB를 처음부터 채우려면:

```powershell
mingw32-make clean
mingw32-make
.\country.exe
```

(`clean`은 실행 파일·DLL·`countries.db`를 지웁니다. CSV는 그대로 둡니다.)

---

## 조작 안내

| 키 / 동작 | 내용 |
|-----------|------|
| Enter | 검색 (또는 추가·수정 입력 확정) |
| 핀 클릭 | 해당 국가 주요 3도시 + 이미지 |
| F2 | 국가 추가 |
| F3 | 국가 수정 |
| F4 | 국가 삭제 |
| F5 | DB 목록 열기 |
| ↑ ↓ | 목록 스크롤 |
| ESC | 목록/입력 취소, 또는 종료 |

---

## 폴더 구조 (간단)

```
ANU/
├── country.c          ← 프로그램 본체
├── sqlite3.c / .h     ← SQLite (같이 컴파일)
├── Makefile           ← 빌드 설정
├── countries.csv      ← 국가·수도·좌표
├── cities.csv         ← 국가별 주요 3도시 + 이미지 경로
├── deps/              ← Raylib
└── assets/
    ├── world_map.jpg  ← 세계 지도
    ├── malgun.ttf     ← 한글 폰트
    ├── flags/         ← 국기 PNG
    └── cities/        ← 도시 이미지
```

실행 후 생기는 파일: `country.exe`, `raylib.dll`, `countries.db`  
(깃에 없어도 빌드·첫 실행으로 다시 만들 수 있습니다.)

---

## DB는 어떻게 되어 있나요?

관계형 SQLite 파일 하나(`countries.db`)입니다.

- **countries** … 국가명, 수도, 위도, 경도
- **cities** … 그 나라의 주요 도시 최대 3개 (`country_id`로 연결)

한 나라를 지우면(F4) 연결된 도시 행도 함께 지워집니다.  
F5 목록에는 **국가만** 보이고, 도시 정보는 **핀을 클릭했을 때**만 나옵니다.

---

## 데이터·이미지 출처 (요약)

| 항목 | 출처 |
|------|------|
| 위도·경도 | [Open-Meteo Geocoding](https://geocoding-api.open-meteo.com/v1/search) (+ 일부 수동 보정) |
| 지도 | Wikimedia Commons *Equirectangular projection SW* (CC BY-SA 3.0) |
| 국기 | Flagcdn / Flagpedia, 일부 Commons — 자세한 목록: `assets/flags/SOURCES.txt` |
| 도시 사진 | Wikipedia / Commons / Openverse — `assets/cities/SOURCES.txt` |
| DB | SQLite (Public Domain) |
| 화면 | Raylib 5.5 (zlib) |

---

## 팀 역할 (참고)

- DB·CSV 적재·CRUD·countries↔cities 관계
- Raylib 지도, 검색창, 목록, 국기 카드, 3도시 패널

카카오톡으로 주제를 맞춘 뒤 위처럼 나눠 작업했습니다.

---

## 자주 생기는 질문

**CSV를 고쳤는데 화면에 안 반영돼요**  
이미 `countries.db`가 있으면 CSV를 다시 안 읽습니다. `mingw32-make clean` 후 다시 실행하거나, 프로그램의 수정(F3)·추가(F2)를 쓰세요.

**빌드가 안 돼요**  
`gcc`와 `mingw32-make`가 PATH에 있는지, `deps` 폴더가 있는지 확인하세요.

**첫 실행이 느려요**  
한글 폰트·국기·도시 이미지를 불러오는 과정입니다.

---

지도에서 나라를 찾아 보고, 핀을 눌러 주요 도시까지 확인하는 미니 데이터 관리 프로그램입니다. 즐겁게 써 보세요.
