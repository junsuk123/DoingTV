#include "power_button.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "POWER_BUTTON";

// (New version 기준)
#define SYS_EN_PIN    GPIO_NUM_41  // 전원 유지 제어 핀 (출력)
#define SYS_OUT_PIN   GPIO_NUM_40  // PWR 버튼 입력 핀 (입력, 버튼 누르면 LOW)

// 롱프레스 최소 시간 (밀리초)
#define LONG_PRESS_MS 1000

static power_button_cb_t s_cb = NULL;  // 롱프레스 감지 시 호출할 사용자 콜백

// 버튼 감지 전용 태스크
static void power_button_task(void *arg)
{
    // 내부적으로는 pull-up 상태에서 버튼을 누르면 LOW가 되므로,
    // LOW 상태가 1초 연속 유지되면 롱프레스로 간주.

    while (1) {
        // 1) 버튼이 눌린 상태인지 확인 (LOW)
        if (gpio_get_level(SYS_OUT_PIN) == 0) {
            int64_t start = esp_timer_get_time(); // 마이크로초 단위
            // 2) 1초 동안 계속 눌려있는지 폴링
            while (gpio_get_level(SYS_OUT_PIN) == 0) {
                int64_t now = esp_timer_get_time();
                if (((now - start) / 1000) >= LONG_PRESS_MS) {
                    ESP_LOGI(TAG, "롱프레스(1초) 감지됨");

                    // 3) 사용자 콜백 호출 (있다면)
                    if (s_cb) {
                        s_cb();
                    }

                    // 4) SYS_EN을 LOW로 내려서 전원 OFF (락 해제)
                    gpio_set_level(SYS_EN_PIN, 0);
                    ESP_LOGI(TAG, "SYS_EN = LOW → 전원 OFF");

                    // 전원 OFF 이후 코드가 더 이상 돌지 않으므로,
                    // 딜레이만 두고 무한 루프에 머뭅니다.
                    vTaskDelay(pdMS_TO_TICKS(100));
                    while (1) {
                        vTaskDelay(pdMS_TO_TICKS(1000));
                    }
                }
                vTaskDelay(pdMS_TO_TICKS(10)); // 10ms 간격으로 재확인
            }
            // 버튼을 3초 미만으로 떼어낸 경우 (롱프레스 아님) → 무시
        }
        vTaskDelay(pdMS_TO_TICKS(100)); // 100ms 간격으로 메인 루프 반복
    }
}

void power_button_init(power_button_cb_t on_press_cb)
{
    s_cb = on_press_cb;

    // 1) SYS_EN 핀: OUTPUT, 초기 HIGH 설정
    gpio_config_t out_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << SYS_EN_PIN),
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE
    };
    gpio_config(&out_conf);
    // 전원이 들어오면 제일 먼저 SYS_EN을 HIGH로 락 → 전원 유지
    gpio_set_level(SYS_EN_PIN, 1);
    ESP_LOGI(TAG, "SYS_EN = HIGH (전원 유지 중)");

    // 2) SYS_OUT 핀: INPUT, 내부 풀업(PULL-UP) 활성화
    gpio_config_t in_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << SYS_OUT_PIN),
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en = GPIO_PULLUP_ENABLE
    };
    gpio_config(&in_conf);

    // 3) 롱프레스 감지 태스크 생성
    xTaskCreate(power_button_task,
                "power_button_task",
                4 * 1024,  // 스택 크기 (4KB)
                NULL,
                configMAX_PRIORITIES - 1,  // 높은 우선순위
                NULL);
}
