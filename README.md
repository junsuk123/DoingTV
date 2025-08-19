# DoingTV

DoingTV는 ESP32-S3 마이크로컨트롤러를 기반으로 한 디스플레이 프로젝트입니다.

## 하드웨어 요구사항

- ESP32-S3 개발 보드
- ST7789 LCD 디스플레이
- 전원 버튼
- 기타 주변 하드웨어

## 주요 기능

- ST7789 LCD 디스플레이 제어
- JPEG 및 PNG 이미지 디코딩 및 표시
- 파일 서버 기능
- 전원 관리 및 충전 표시 기능

## 프로젝트 구조

```
.
├── components/          # 커스텀 컴포넌트
│   ├── charging_indicator/
│   ├── decode_jpeg/    # JPEG 디코딩 컴포넌트
│   ├── decode_png/     # PNG 디코딩 컴포넌트
│   ├── pngle/
│   ├── power_button/   # 전원 버튼 관리
│   └── st7789/        # LCD 드라이버
├── fonts/              # 폰트 파일
├── images/             # 이미지 리소스
└── main/              # 메인 애플리케이션 코드
```

## 빌드 및 설치 방법

1. ESP-IDF 환경 설정:
   ```bash
   . $IDF_PATH/export.sh
   ```

2. 프로젝트 구성:
   ```bash
   idf.py menuconfig
   ```

3. 빌드:
   ```bash
   idf.py build
   ```

4. 플래시:
   ```bash
   idf.py flash
   ```

5. 시리얼 모니터링:
   ```bash
   idf.py monitor
   ```

## 설정

`sdkconfig` 파일을 통해 다음과 같은 설정을 커스터마이징할 수 있습니다:
- LCD 디스플레이 설정
- WiFi 설정
- 파일 시스템 설정
- 기타 하드웨어 설정

## 라이선스

이 프로젝트는 오픈소스로 제공됩니다.

## 기여

버그 리포트, 기능 제안 및 풀 리퀘스트를 환영합니다.
