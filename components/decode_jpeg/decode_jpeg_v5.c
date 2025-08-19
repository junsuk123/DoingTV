#include <stdio.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "decode_jpeg.h"
#include "esp_rom_caps.h"
#include "esp_log.h"

#if CONFIG_JD_USE_ROM
  #if defined(ESP_ROM_HAS_JPEG_DECODE)
    #include "rom/tjpgd.h"
    #define JPEG "rom tjpgd"
    typedef unsigned int jpeg_decode_out_t;
  #else
    #error Using JPEG decoder from ROM is not supported for selected target. Please select external code in menuconfig.
  #endif
#else
  #include "tjpgd.h"
  #define JPEG "external tjpgd"
  typedef int jpeg_decode_out_t;
#endif

#define TAG __FUNCTION__

// 작업 버퍼 크기: 필요에 따라 조정 가능 (예: 8192, 16384 등)
#define JD_WORKSZ 50000
static uint8_t jd_workbuf[JD_WORKSZ];

// 디코더에 전달할 컨텍스트 구조체
typedef struct {
    pixel_jpeg **outData;   // 화면 높이 만큼의 포인터 배열
    int screenWidth;
    int screenHeight;
    FILE *fp;
} JpegDev;

// 입력 콜백: 파일에서 len 바이트 읽거나 건너뛰기
static unsigned int infunc(JDEC *decoder, uint8_t *buf, unsigned int len) {
    JpegDev *jd = (JpegDev *)decoder->device;
    ESP_LOGD(TAG, "infunc len=%u fp=%p", len, jd->fp);
    if (buf) {
        return fread(buf, 1, len, jd->fp);
    } else {
        fseek(jd->fp, len, SEEK_CUR);
        return len;
    }
}

// RGB888 → RGB565 변환 매크로
#define rgb565(r,g,b) (((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3))

// 출력 콜백: 디코딩된 블록을 화면 버퍼에 복사
static jpeg_decode_out_t outfunc(JDEC *decoder, void *bitmap, JRECT *rect) {
    JpegDev *jd = (JpegDev *)decoder->device;
    uint8_t *in = (uint8_t *)bitmap;
    for (int y = rect->top; y <= rect->bottom; y++) {
        for (int x = rect->left; x <= rect->right; x++) {
            if (y < jd->screenHeight && x < jd->screenWidth) {
                jd->outData[y][x] = rgb565(in[0], in[1], in[2]);
            }
            in += 3;
        }
    }
    return 1;
}

// 스케일 계산 (1/2^N)
static uint8_t getScale(int sw, int sh, uint16_t dw, uint16_t dh) {
    if (sw >= dw && sh >= dh) return 0;
    double sx = (double)dw / sw, sy = (double)dh / sh;
    double s = sx > sy ? sx : sy;
    if (s <= 2.0) return 1;
    if (s <= 4.0) return 2;
    return 3;
}

esp_err_t decode_jpeg(pixel_jpeg ***pixels, char *file, int screenWidth, int screenHeight, int *imageWidth, int *imageHeight) {
    char *work = (char*)jd_workbuf;
    uint32_t work_size = JD_WORKSZ;
    JDEC decoder;
    JpegDev jd = { 0 };
    esp_err_t ret = ESP_OK;

    ESP_LOGW(TAG, "v5 version. JPEG Decoder is %s", JPEG);

    // 1) 화면 높이만큼 포인터 배열 할당
    *pixels = calloc(screenHeight, sizeof(pixel_jpeg*));
    if (!*pixels) {
        ESP_LOGE(TAG, "Memory alloc for pixel lines failed");
        return ESP_ERR_NO_MEM;
    }
    for (int i = 0; i < screenHeight; i++) {
        (*pixels)[i] = malloc(screenWidth * sizeof(pixel_jpeg));
        if (!(*pixels)[i]) {
            ESP_LOGE(TAG, "Memory alloc for line %d failed", i);
            ret = ESP_ERR_NO_MEM;
            goto err;
        }
    }

    // 2) JpegDev 초기화
    jd.outData     = *pixels;
    jd.screenWidth = screenWidth;
    jd.screenHeight= screenHeight;
    jd.fp = fopen(file, "rb");
    if (!jd.fp) {
        ESP_LOGW(TAG, "JPEG file not found [%s]", file);
        ret = ESP_ERR_NOT_FOUND;
        goto err;
    }

    // 3) 디코더 준비
    JRESULT res = jd_prepare(&decoder, infunc, work, work_size, &jd);
    if (res != JDR_OK) {
        ESP_LOGE(TAG, "jd_prepare failed (%d)", res);
        ret = ESP_ERR_NOT_SUPPORTED;
        goto err;
    }

    // 4) 스케일 및 이미지 크기 계산
    uint8_t scale = getScale(screenWidth, screenHeight, decoder.width, decoder.height);
    double factor = 1.0 / (1 << scale);
    *imageWidth  = (int)(decoder.width  * factor);
    *imageHeight = (int)(decoder.height * factor);

    // 5) 디코딩 실행
    res = jd_decomp(&decoder, outfunc, scale);
    if (res != JDR_OK) {
        ESP_LOGE(TAG, "jd_decomp failed (%d)", res);
        ret = ESP_ERR_NOT_SUPPORTED;
        goto err;
    }

    fclose(jd.fp);
    return ESP_OK;

err:
    if (jd.fp) fclose(jd.fp);
    if (*pixels) {
        for (int i = 0; i < screenHeight; i++) {
            free((*pixels)[i]);
        }
        free(*pixels);
    }
    return ret;
}

esp_err_t release_image(pixel_jpeg ***pixels, int screenWidth, int screenHeight) {
    if (*pixels) {
        for (int i = 0; i < screenHeight; i++) {
            free((*pixels)[i]);
        }
        free(*pixels);
    }
    return ESP_OK;
}
