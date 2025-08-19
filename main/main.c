/*
 * HTTP File Server + ST7789 + Provision + Charging Indicator
 * - SPIFFS 업/다운로드
 * - MADCTL 회전 후 스케일 출력
 * - PNG/JPEG 자동 디코딩
 * - BLE Wi-Fi 프로비저닝(실패 시 진입)
 * - 충전상태(EMA+dV/dt) 기반 LCD BL 제어
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>    // strcasecmp
#include <inttypes.h>
#include <dirent.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_err.h"
#include "esp_system.h"
#include "esp_vfs.h"
#include "esp_spiffs.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"

#include "protocol_examples_common.h"

#include "st7789.h"
#include "fontx.h"
#include "pngle.h"
#include "decode_png.h"
#include "decode_jpeg.h"

#include "wifi_provisioning/manager.h"
#include "wifi_provisioning/scheme_ble.h"

#include "file_serving_example_common.h"
#include "power_button.h"
#include "wifi_provision.h"
#include "charging_indicator.h"

// ---- 이벤트 그룹
static EventGroupHandle_t wifi_event_group;
#define WIFI_CONNECTED_BIT   BIT0

static void on_ip_event(void* arg, esp_event_base_t event_base,
                        int32_t event_id, void* event_data)
{
    xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
}

static const char *TAG       = "MAIN";
static const char *TAG_POWER = "POWER_BUTTON";

// ---- 전역
static int origW, origH;
static int scrW, scrH;
static float scaleF;
static int scaledW, scaledH;
static int colOffset, rowOffset;
TFT_t *g_dev = NULL;

static void on_power_long_press(void)
{
    ESP_LOGW(TAG_POWER, "전원 OFF 콜백: 저장 등 마무리");
}

// ---- SPIFFS
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
            ESP_LOGE(TAG, "FS 마운트/포맷 실패");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "SPIFFS 파티션 없음");
        } else {
            ESP_LOGE(TAG, "SPIFFS init 실패 (%s)", esp_err_to_name(ret));
        }
        return ret;
    }
    size_t total = 0, used = 0;
    ret = esp_spiffs_info(conf.partition_label, &total, &used);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Mounted %s as %s (total:%u used:%u)",
                 path, label, (unsigned)total, (unsigned)used);
    } else {
        ESP_LOGE(TAG, "SPIFFS info 실패 (%s)", esp_err_to_name(ret));
    }
    return ret;
}

// ---- PNG(회전 없음, 스케일만)
static void png_init_simple(pngle_t *pngle, uint32_t width, uint32_t height)
{
    origW = (int)width;  origH = (int)height;
    float ratioW = (float)scrW / (float)origW;
    float ratioH = (float)scrH / (float)origH;
    scaleF  = (ratioW < ratioH) ? ratioW : ratioH;
    scaledW = (int)(origW * scaleF);
    scaledH = (int)(origH * scaleF);
    colOffset = (scrW - scaledW) / 2;
    rowOffset = (scrH - scaledH) / 2;
    if (colOffset < 0) colOffset = 0;
    if (rowOffset < 0) rowOffset = 0;
}

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
static void png_done_simple(pngle_t *pngle) {}

static void PNGDisplaySimple(TFT_t *dev, const char *file)
{
    FILE *fp = fopen(file, "rb");
    if (!fp) { ESP_LOGE(TAG, "파일 없음: %s", file); return; }

    g_dev = dev;

    pngle_t *pngle = pngle_new(scrW, scrH);
    if (!pngle) { ESP_LOGE(TAG, "pngle_new 실패"); fclose(fp); return; }

    pngle_set_init_callback(pngle, png_init_simple);
    pngle_set_draw_callback(pngle, png_draw_simple);
    pngle_set_done_callback(pngle, png_done_simple);
    pngle_set_display_gamma(pngle, 2.2);

    lcdSetFontDirection(dev, 0);
    lcdFillScreen(dev, BLACK);

    char buf[1024];
    size_t remain = 0;
    while (!feof(fp)) {
        if (remain >= sizeof(buf)) { ESP_LOGE(TAG, "버퍼 오버플로우"); break; }
        int len = fread(buf + remain, 1, sizeof(buf) - remain, fp);
        if (len <= 0) break;
        int fed = pngle_feed(pngle, buf, remain + len);
        if (fed < 0) { ESP_LOGE(TAG, "pngle_feed 오류: %s", pngle_error(pngle)); break; }
        remain = remain + len - fed;
        if (remain > 0) memmove(buf, buf + fed, remain);
    }
    fclose(fp);

    pngle_destroy(pngle, scrW, scrH);
    lcdDrawFinish(dev);
    vTaskDelay(pdMS_TO_TICKS(10));
}

// ---- JPEG
static void JPEGDisplaySimple(TFT_t *dev, const char *file)
{
    pixel_jpeg **pixels = NULL;
    int imageW = 0, imageH = 0;

    esp_err_t err = decode_jpeg(&pixels, (char *)file, scrW, scrH, &imageW, &imageH);
    if (err != ESP_OK) { ESP_LOGE(TAG, "JPEG 디코드 실패: %s", file); return; }

    lcdSetFontDirection(dev, 0);
    lcdFillScreen(dev, BLACK);

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
    release_image(&pixels, scrW, scrH);
}

// ---- ST7789 태스크
void ST7789(void *pvParameters)
{
    FontxFile dummyFx[2];
    InitFontx(dummyFx, "/fonts/ILGH16XB.FNT", "");

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

    // MADCTL: 90° 회전
    spi_master_write_command(&dev, 0x36);
    spi_master_write_data_byte(&dev, 0x60);
#if CONFIG_INVERSION
    lcdInversionOff(&dev);
#endif

    scrW = CONFIG_HEIGHT; // 회전 반영
    scrH = CONFIG_WIDTH;

    const char *images_dir = "/images";
    DIR *dir; struct dirent *entry;
    char found_path[256];
    char last_path[256] = {0};

    uint32_t scan_acc_ms = 0; // 2초마다 스캔

    while (1) {
        // 1) 1초마다 충전 상태 갱신 (정확한 dt 전달)
        vTaskDelay(pdMS_TO_TICKS(1000));
        charging_indicator_update(1000);

        // 2) 2초마다 이미지 스캔
        scan_acc_ms += 1000;
        if (scan_acc_ms < 2000) continue;
        scan_acc_ms = 0;

        ESP_LOGI(TAG, "Update Charging Status / Scan images");

        dir = opendir(images_dir);
        if (!dir) { ESP_LOGE(TAG, "디렉토리 열기 실패: %s", images_dir); continue; }

        bool found = false;
        while ((entry = readdir(dir)) != NULL) {
            if (entry->d_type != DT_REG) continue;
            const char *name = entry->d_name;
            size_t len = strlen(name);
            if (len < 4) continue;

            if (strcasecmp(&name[len - 4], ".png") == 0 ||
                strcasecmp(&name[len - 4], ".jpg") == 0 ||
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

                const char *ext = strrchr(found_path, '.');
                if (ext && strcasecmp(ext, ".png") == 0) {
                    PNGDisplaySimple(&dev, found_path);
                } else {
                    JPEGDisplaySimple(&dev, found_path);
                }
            }
        } else {
            ESP_LOGW(TAG, "이미지 파일이 없습니다: %s", images_dir);
        }
    }
}

// ---- app_main
void app_main(void)
{
    power_button_init(on_power_long_press);
    wifi_event_group = xEventGroupCreate();

    ESP_LOGI(TAG, "SPIFFS 초기화...");
    ESP_ERROR_CHECK(mountSPIFFS("/fonts",  "storage1", 7));
    listSPIFFS("/fonts/");
    ESP_ERROR_CHECK(mountSPIFFS("/images", "storage2", 7));
    listSPIFFS("/images/");

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_ERROR_CHECK( esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &on_ip_event, NULL, NULL));

    // 1) 기존 STA 연결 시도
    esp_err_t conn_ret = example_connect();
    if (conn_ret != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi 실패(%s), BLE 프로비저닝 진입", esp_err_to_name(conn_ret));
        ESP_ERROR_CHECK( wifi_provision_init(true) );
        xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT,
                            pdTRUE, pdTRUE, portMAX_DELAY);
        ESP_LOGI(TAG, "프로비저닝으로 Wi-Fi 연결됨");
    }

    const char *base_path = "/images";
    ESP_ERROR_CHECK(example_mount_storage(base_path));
    ESP_ERROR_CHECK(example_start_file_server(base_path));
    ESP_LOGI(TAG, "File server started");

    xTaskCreate(ST7789, "ST7789", 20 * 1024, NULL, 2, NULL);
}
