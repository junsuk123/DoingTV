// charging_indicator.c
#include <stdio.h>
#include "esp_log.h"
#include "driver/adc.h"
#include "esp_adc_cal.h"
#include <inttypes.h>
#include "st7789.h"                // lcdBacklightOff/On 선언
extern TFT_t *g_dev;
#define TAG             "CHARGE"
#define DEFAULT_VREF    1100       // mV
#define ADC1_CH         ADC1_CHANNEL_0  // GPIO1 ⇒ ADC1_CH0
#define ADC_ATTEN       ADC_ATTEN_DB_11 // 0..3.6V 범위

static esp_adc_cal_characteristics_t adc_chars;

// 1) ADC 초기화
static void init_adc(void)
{
    // UART0 메시지를 다른 핀으로 옮기거나 끄셔야 GPIO1이 ADC 전용이 됩니다.
    adc1_config_width(ADC_WIDTH_BIT_12);
    adc1_config_channel_atten(ADC1_CH, ADC_ATTEN);
    esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN, ADC_WIDTH_BIT_12,
                             DEFAULT_VREF, &adc_chars);
}

// 2) 전압 읽기 함수
static float read_battery_voltage(void)
{   
    uint32_t raw = 0;
    for (int i = 0; i < 64; i++) {
        raw += adc1_get_raw(ADC1_CH);
    }
    raw /= 64;
    uint32_t mv = esp_adc_cal_raw_to_voltage(raw, &adc_chars);
    // 분압비 3:1 가정
    float batt_v = mv * 3.0f / 1000.0f;
    ESP_LOGI(TAG, "ADC=%"PRIu32" raw mV=%"PRIu32" Batt=%.2fV", raw, mv, batt_v);
    return batt_v;
}

// 3) 초기화: ADC 세팅 + 백라이트 기본 OFF
void charging_indicator_init(void)
{
    init_adc();
    // 드라이버 초기화 전에 g_dev가 NULL이면 무시하도록
    if (g_dev) {
        lcdBacklightOff(g_dev);
    }
    float v = read_battery_voltage();
    ESP_LOGI(TAG, "Initial Battery: %.2fV", v);
}

// 4) 주기 갱신: 3.8V 초과면 충전 중 → 백라이트 OFF, 아니면 ON
void charging_indicator_update(void)
{   
    if (!g_dev) return;
    float v = read_battery_voltage();
    if (v > 3.98f) {
        lcdBacklightOff(g_dev);
        spi_master_write_command(g_dev, 0x28);   // Display OFF
        ESP_LOGI(TAG, "Charging detected → BL OFF");
    } else {
        lcdBacklightOn(g_dev);
        spi_master_write_command(g_dev, 0x29);   // Display ON
        ESP_LOGI(TAG, "Discharging      → BL ON");
    }
}
