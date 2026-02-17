# ZX-OS (T-Embed CC1101)

ZX-OS는 LilyGo **T-Embed CC1101** 보드에서 동작하는 임베디드 펌웨어입니다.
기존 OpenClaw 기반 기능을 포함하며, 게이트웨이 연결/무선 제어/UI 앱 실행을 지원합니다.

## 주요 특징

- OS/장치 기본 이름을 **ZX-OS Node**로 사용
- LVGL 기반 런처 UI
- OpenClaw Gateway 연동 (`ws://`, `wss://`)
- RF(CC1101), NFC, RFID, NRF24 앱 제공
- SD 기반 설정 저장 및 펌웨어 업데이트 지원

## 빌드

```bash
pio run -e t-embed-cc1101
```

## 업로드

```bash
pio run -e t-embed-cc1101 -t upload
```

## 시리얼 모니터

```bash
pio device monitor -b 115200
```

## 프로젝트 구조

- `src/main.cpp`: 시스템 부트스트랩/런처 시작
- `src/core/*`: 런타임 설정, 게이트웨이, 무선/통신 핵심 로직
- `src/apps/*`: 기능별 앱 구현
- `src/ui/*`: LVGL UI 및 입력 어댑터

## 라이선스

MIT License (`LICENSE` 참고)
