/*
 * SPDX-FileCopyrightText: 2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */
/* HTTP File Server + ST7789 Example
 *
 * - SPIFFS에 저장된 이미지를 Wi-Fi 파일 서버를 통해 업로드/삭제/다운로드
 * - ST7789 LCD에 하드웨어 MADCTL 회전 후, 스케일만 적용하여 이미지 출력
 * - JPEG/PNG 파일 형식에 따라 자동으로 디코딩 처리
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <dirent.h>
#include <sys/stat.h>


#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_system.h"
#include "esp_vfs.h"
#include "esp_spiffs.h"
#include "protocol_examples_common.h"

#include "st7789.h"
#include "fontx.h"
#include "pngle.h"
#include "decode_png.h"
#include "decode_jpeg.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "protocol_examples_common.h"
#include "wifi_provisioning/manager.h"
#include "wifi_provisioning/scheme_ble.h"

#include "file_serving_example_common.h"
#include "power_button.h"
#include "wifi_provision.h"
#include "charging_indicator.h"

/* EventGroup for provisioning-done notification */
#include "freertos/event_groups.h"

static EventGroupHandle_t wifi_event_group;
#define WIFI_CONNECTED_BIT   BIT0

/* IP 획득 시 호출될 콜백 */
static void on_ip_event(void* arg, esp_event_base_t event_base,
                        int32_t event_id, void* event_data)
{
    xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
}

static const char *TAG       = "MAIN";
static const char *TAG_POWER = "POWER_BUTTON";

// --------------------------------------------------
// 전역 변수들
// --------------------------------------------------
static int origW, origH;          // 원본 이미지 크기
static int scrW, scrH;            // 회전 후 화면 가로·세로 (MADCTL 적용된 상태)
static float scaleF;              // 확대/축소 배율
static int scaledW, scaledH;      // 스케일된 크기
static int colOffset, rowOffset;  // 중앙 정렬 오프셋
TFT_t *g_dev = NULL;       // LCD 디바이스 포인터

static void on_power_long_press(void)
{
    ESP_LOGW(TAG_POWER, "전원 꺼짐 직전 콜백 호출됨: 필요한 마무리 작업 수행");
}

// --------------------------------------------------
// SPIFFS 마운트 및 디렉터리 리스트 함수
// --------------------------------------------------
static void listSPIFFS(const char *path)
{
    DIR *dir = opendir(path);
    assert(dir != NULL);
    while (true) {
        struct dirent *pe = readdir(dir);
        if (!pe) break;
        ESP_LOGI(TAG, "d_name=%s  d_ino=%d  d_type=%x",
                 pe->d_name, pe->d_ino, pe->d_type);
    }
    closedir(dir);
}

static esp_err_t mountSPIFFS(const char *path, const char *label, int max_files)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path              = path,
        .partition_label        = label,
        .max_files              = max_files,
        .format_if_mount_failed = true
    };
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "파일 시스템 마운트/포맷 실패");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "SPIFFS 파티션을 찾을 수 없음");
        } else {
            ESP_LOGE(TAG, "SPIFFS 초기화 실패 (%s)", esp_err_to_name(ret));
        }
        return ret;
    }
    size_t total = 0, used = 0;
    ret = esp_spiffs_info(conf.partition_label, &total, &used);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Mounted %s as %s (total:%u used:%u)",
                 path, label, (unsigned)total, (unsigned)used);
    } else {
        ESP_LOGE(TAG, "SPIFFS 정보 조회 실패 (%s)", esp_err_to_name(ret));
    }
    return ret;
}

// --------------------------------------------------
// “회전 없는” PNG 초기화 콜백: 원본 크기를 얻고 스케일·오프셋 계산
// --------------------------------------------------
static void png_init_simple(pngle_t *pngle, uint32_t width, uint32_t height)
{
    origW = (int)width;
    origH = (int)height;
    // scrW, scrH에는 이미 “MADCTL 적용 후 회전된” 화면 크기가 들어있음
    float ratioW = (float)scrW / (float)origW;
    float ratioH = (float)scrH / (float)origH;
    scaleF = (ratioW < ratioH) ? ratioW : ratioH;
    scaledW = (int)(origW * scaleF);
    scaledH = (int)(origH * scaleF);
    colOffset = (scrW - scaledW) / 2;
    rowOffset = (scrH - scaledH) / 2;
    if (colOffset < 0) colOffset = 0;
    if (rowOffset < 0) rowOffset = 0;
}

// --------------------------------------------------
// “회전 없는” PNG 드로우 콜백: RGBA 블록 → 스케일·오프셋 적용해 LCD 픽셀 그리기
// --------------------------------------------------
static void png_draw_simple(pngle_t *pngle, uint32_t x, uint32_t y,
                            uint32_t w, uint32_t h, unsigned char *rgba)
{
    if (!g_dev) return;
    for (uint32_t row = 0; row < h; row++) {
        for (uint32_t col = 0; col < w; col++) {
            unsigned char *p = rgba + ((row * w + col) * 4);
            uint8_t r = p[0], g = p[1], b = p[2];
            uint16_t color = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);

            int origX = (int)x + (int)col;
            int origY = (int)y + (int)row;
            int dispX = colOffset + (int)(origX * scaleF);
            int dispY = rowOffset + (int)(origY * scaleF);
            lcdDrawPixel(g_dev, dispX, dispY, color);
        }
    }
}

// --------------------------------------------------
// PNG 완료 콜백: 별도 작업 없음
// --------------------------------------------------
static void png_done_simple(pngle_t *pngle)
{
    // 별도 처리 없음
}

// --------------------------------------------------
// PNG 스트리밍 처리: 하드웨어 회전은 MADCTL으로 이미 걸렸으므로,
// 화면 크기(scrW, scrH)만 넘겨주고, 간단히 스케일+렌더링
// --------------------------------------------------
static void PNGDisplaySimple(TFT_t *dev, const char *file)
{
    FILE *fp = fopen(file, "rb");
    if (!fp) {
        ESP_LOGE(TAG, "파일을 찾을 수 없음: %s", file);
        return;
    }

    scrW = scrW;  // 그대로 사용
    scrH = scrH;
    g_dev = dev;

    pngle_t *pngle = pngle_new(scrW, scrH);
    if (!pngle) {
        ESP_LOGE(TAG, "pngle_new 실패");
        fclose(fp);
        return;
    }

    // “회전 없는” 콜백 등록
    pngle_set_init_callback(pngle, png_init_simple);
    pngle_set_draw_callback(pngle, png_draw_simple);
    pngle_set_done_callback(pngle, png_done_simple);
    pngle_set_display_gamma(pngle, 2.2);

    // 화면 클리어
    lcdSetFontDirection(dev, 0);
    lcdFillScreen(dev, BLACK);

    char buf[1024];
    size_t remain = 0;
    while (!feof(fp)) {
        if (remain >= sizeof(buf)) {
            ESP_LOGE(TAG, "버퍼 오버플로우");
            break;
        }
        int len = fread(buf + remain, 1, sizeof(buf) - remain, fp);
        if (len <= 0) break;
        int fed = pngle_feed(pngle, buf, remain + len);
        if (fed < 0) {
            ESP_LOGE(TAG, "pngle_feed 오류: %s", pngle_error(pngle));
            break;
        }
        remain = remain + len - fed;
        if (remain > 0) {
            memmove(buf, buf + fed, remain);
        }
    }
    fclose(fp);

    pngle_destroy(pngle, scrW, scrH);
    lcdDrawFinish(dev);
    vTaskDelay(pdMS_TO_TICKS(10));
}

// --------------------------------------------------
// JPEG 디코딩 처리: 하드웨어 회전은 MADCTL으로 이미 걸렸으므로,
// decode_jpeg() 호출 후, 간단히 픽셀 버퍼 출력
// --------------------------------------------------
static void JPEGDisplaySimple(TFT_t *dev, const char *file)
{
    pixel_jpeg **pixels = NULL;
    int imageW = 0, imageH = 0;

    // decode_jpeg(): scrW, scrH 크기로 디코딩하며 imageW, imageH 반환
    esp_err_t err = decode_jpeg(&pixels, (char *)file, scrW, scrH, &imageW, &imageH);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "JPEG 디코드 실패: %s", file);
        return;
    }

    // 화면 클리어
    lcdSetFontDirection(dev, 0);
    lcdFillScreen(dev, BLACK);

    // 이미지 픽셀 버퍼(pixels[y][x])를 화면 중앙에 스케일 없이 렌더링
    // 이미 decode_jpeg() 단계에서 적절히 스케일링이 적용되었으므로 그 상태 그대로 출력
    colOffset = (scrW - imageW) / 2;
    rowOffset = (scrH - imageH) / 2;
    if (colOffset < 0) colOffset = 0;
    if (rowOffset < 0) rowOffset = 0;

    for (int y = 0; y < imageH; y++) {
        for (int x = 0; x < imageW; x++) {
            int dispX = colOffset + x;
            int dispY = rowOffset + y;
            lcdDrawPixel(dev, dispX, dispY, pixels[y][x]);
        }
    }
    lcdDrawFinish(dev);

    // 메모리 해제
    release_image(&pixels, scrW, scrH);
}

// --------------------------------------------------
// ST7789 태스크: /images에서 첫 번째 이미지 파일 찾아서 계속 갱신
// --------------------------------------------------
void ST7789(void *pvParameters)
{
    // 더미 폰트 초기화 (필요 시)
    FontxFile dummyFx[2];
    InitFontx(dummyFx, "/fonts/ILGH16XB.FNT", "");

    // LCD 초기화
    TFT_t dev;
    spi_master_init(&dev,
                    CONFIG_MOSI_GPIO,
                    CONFIG_SCLK_GPIO,
                    CONFIG_CS_GPIO,
                    CONFIG_DC_GPIO,
                    CONFIG_RESET_GPIO,
                    CONFIG_BL_GPIO);
    lcdInit(&dev,
            CONFIG_WIDTH,
            CONFIG_HEIGHT,
            CONFIG_OFFSETX,
            CONFIG_OFFSETY);
    g_dev = &dev;   
    charging_indicator_init();

    // ① MADCTL: 90° 회전 (MV=1, MX=1, MY=0)
    spi_master_write_command(&dev, 0x36);
    spi_master_write_data_byte(&dev, 0x60);

#if CONFIG_INVERSION
    ESP_LOGI(TAG, "디스플레이 반전 해제");
    lcdInversionOff(&dev);
#endif

    // 회전 후 “가로·세로”를 뒤집어서 scrW/scrH에 할당
    scrW = CONFIG_HEIGHT;  // 예: 320 → 240
    scrH = CONFIG_WIDTH;   // 예: 240 → 320

    const char *images_dir = "/images";
    DIR *dir;
    struct dirent *entry;
    char found_path[256];
    char last_path[256] = {0};

    while (1) {
        charging_indicator_update();  // 충전 중이면 백라이트 OFF

        vTaskDelay(pdMS_TO_TICKS(1000));  // 1초마다 스캔

        ESP_LOGI(TAG, "Update Charging Status");
        dir = opendir(images_dir);
        if (!dir) {
            ESP_LOGE(TAG, "디렉토리 열기 실패: %s", images_dir);
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        bool found = false;
        while ((entry = readdir(dir)) != NULL) {
            if (entry->d_type != DT_REG) continue;
            const char *name = entry->d_name;
            size_t len = strlen(name);
            if (len < 4) continue;
            const char *ext = &name[len - 4];

            // 확장자 판별 (".png", ".jpg", ".jpeg" 대소문자 무시)
            if (strcasecmp(ext, ".png") == 0 ||
                strcasecmp(ext, ".jpg") == 0 ||
                (len >= 5 && strcasecmp(&name[len - 5], ".jpeg") == 0)) {

                size_t dlen = strlen(images_dir);
                if (dlen + 1 + len + 1 <= sizeof(found_path)) {
                    memcpy(found_path, images_dir, dlen);
                    found_path[dlen] = '/';
                    memcpy(found_path + dlen + 1, name, len);
                    found_path[dlen + 1 + len] = '\0';
                    found = true;
                    break;
                }
            }
        }
        closedir(dir);

        if (found) {
            if (strcmp(found_path, last_path) != 0) {
                ESP_LOGI(TAG, "New image detected: %s", found_path);
                strncpy(last_path, found_path, sizeof(last_path));

                // 확장자에 따라 PNG/ JPEG 분기
                const char *ext = strrchr(found_path, '.');
                if (ext) {
                    if (strcasecmp(ext, ".png") == 0) {
                        PNGDisplaySimple(&dev, found_path);
                    } else {
                        // .jpg 또는 .jpeg
                        JPEGDisplaySimple(&dev, found_path);
                    }
                }
            }
        } else {
            ESP_LOGW(TAG, "이미지 파일이 없습니다: %s", images_dir);
        }
        
        vTaskDelay(pdMS_TO_TICKS(2000));  // 2초마다 스캔
    }
}

// --------------------------------------------------
// app_main: SPIFFS 마운트 → Wi-Fi 연결 → 파일 서버 시작 → ST7789 태스크 생성
// --------------------------------------------------
void app_main(void)
{
    power_button_init(on_power_long_press);
    wifi_event_group = xEventGroupCreate();


    ESP_LOGI(TAG, "SPIFFS 초기화 중...");

    ESP_ERROR_CHECK(mountSPIFFS("/fonts",  "storage1", 7));
    listSPIFFS("/fonts/");

    ESP_ERROR_CHECK(mountSPIFFS("/images", "storage2", 7));
    listSPIFFS("/images/");

    
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    // esp_netif_create_default_wifi_sta();

    // IP_EVENT_STA_GOT_IP 이벤트 등록
    ESP_ERROR_CHECK( esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP,
        &on_ip_event, NULL, NULL
    ));


    // 1) 우선 기존 방식으로 일반 Wi-Fi 연결 시도
    esp_err_t conn_ret = example_connect();
    if (conn_ret != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi 연결 실패 (%s), BLE 프로비저닝 모드로 전환",
                 esp_err_to_name(conn_ret));

        /* 2) 강제 프로비저닝 모드 진입 */
        ESP_ERROR_CHECK( wifi_provision_init(true) );

        /* 3) IP_EVENT_STA_GOT_IP가 발생해 비트가 세팅될 때까지 대기 */
        xEventGroupWaitBits(wifi_event_group,
                            WIFI_CONNECTED_BIT,
                            pdTRUE,    // 비트를 클리어
                            pdTRUE,    // 단일 비트만 사용하므로 AND 모드
                            portMAX_DELAY);

        ESP_LOGI(TAG, "프로비저닝으로 Wi-Fi 연결됨");
    }

    /* 파일 저장소 마운트 */
    const char *base_path = "/images";
    ESP_ERROR_CHECK(example_mount_storage(base_path));


    /* 파일 서버 시작 */
    ESP_ERROR_CHECK(example_start_file_server(base_path));
    ESP_LOGI(TAG, "File server started");

    /* ST7789 태스크 생성 (스택: 20 KB, 우선순위: 2) */
    xTaskCreate(ST7789, "ST7789", 20 * 1024, NULL, 2, NULL);
    /* 충전 상태 체크 루프 */
}
