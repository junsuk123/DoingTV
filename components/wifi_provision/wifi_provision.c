#include "wifi_provision.h"
#include <stdbool.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_bt.h"

// provisioning manager
#include "wifi_provisioning/manager.h"
#include "wifi_provisioning/scheme_ble.h"

static const char *TAG = "wifi_provision";
// 중복 호출 안전한 Wi‑Fi 스택 준비
static esp_err_t ensure_wifi_stack_ready(void)
{
    esp_err_t err;

    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    // 이미 있으면 재생성하지 않음
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta == NULL) {
        sta = esp_netif_create_default_wifi_sta();   // 포인터 반환
        if (sta == NULL) {
            ESP_LOGE(TAG, "create_default_wifi_sta failed");
            return ESP_FAIL;
        }
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    return esp_wifi_set_storage(WIFI_STORAGE_FLASH); // NVS 저장 모드
}


static void provision_event_handler(void *arg, esp_event_base_t base,
                                    int32_t id, void *data)
{
    switch (id) {
    case WIFI_PROV_CRED_RECV: {
        wifi_config_t *wifi_cfg = (wifi_config_t *)data;
        ESP_LOGI(TAG, "Received WiFi credentials: SSID=%s", (char *)wifi_cfg->sta.ssid);

        // ★ 이 시점엔 WIFI_STORAGE_FLASH가 설정되어 있어야 함(아래 init에서 설정)
        ESP_ERROR_CHECK( esp_wifi_set_mode(WIFI_MODE_STA) );
        ESP_ERROR_CHECK( esp_wifi_set_config(WIFI_IF_STA, wifi_cfg) ); // ⇒ NVS에 영구 저장
        ESP_ERROR_CHECK( esp_wifi_start() );
        ESP_ERROR_CHECK( esp_wifi_connect() );
        break;
    }
    case WIFI_PROV_CRED_FAIL: {
        wifi_prov_sta_fail_reason_t *reason = (wifi_prov_sta_fail_reason_t *)data;
        ESP_LOGE(TAG, "Provisioning failed! reason=%d", *reason);
        break;
    }
    case WIFI_PROV_CRED_SUCCESS:
        ESP_LOGI(TAG, "Provisioning successful! Stopping service...");
        wifi_prov_mgr_stop_provisioning();
        wifi_prov_mgr_deinit();

        // (선택) BLE 컨트롤러 정리
        if (esp_bt_controller_disable() != ESP_OK) ESP_LOGW(TAG, "BT controller disable failed");
        if (esp_bt_controller_deinit()  != ESP_OK) ESP_LOGW(TAG, "BT controller deinit failed");
        ESP_LOGI(TAG, "BLE controller deinitialized");
        break;

    default:
        break;
    }
}

esp_err_t wifi_provision_init(bool force_prov)
{
    // 1) NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 2) ★ 안전 초기화 (중복 호출 안전)
    ESP_ERROR_CHECK(ensure_wifi_stack_ready());

    // 3) 이벤트 핸들러 등록 (이미 등록돼 있으면 에러 아님)
    ESP_ERROR_CHECK(esp_event_handler_register(
        WIFI_PROV_EVENT, ESP_EVENT_ANY_ID, provision_event_handler, NULL));

    // 4) 프로비저닝 매니저 초기화(BLE) — 중복 init 방지: 실패하면 그대로 에러 처리
    wifi_prov_mgr_config_t mgr_cfg = {
        .scheme               = wifi_prov_scheme_ble,
        .scheme_event_handler = WIFI_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BT
    };
    ESP_ERROR_CHECK(wifi_prov_mgr_init(mgr_cfg));

    // 5) 현재 프로비저닝 여부 확인
    bool provisioned = false;
    ESP_ERROR_CHECK(wifi_prov_mgr_is_provisioned(&provisioned));

    if (provisioned && !force_prov) {
        ESP_LOGI(TAG, "Already provisioned → use saved STA config in NVS");
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_start());   // NVS의 마지막 STA 설정 자동 로드
        ESP_ERROR_CHECK(esp_wifi_connect());
        return ESP_OK;
    }

    // 6) BLE 프로비저닝 시작
    ESP_LOGI(TAG, "Starting BLE provisioning service...");
    ESP_ERROR_CHECK(wifi_prov_mgr_start_provisioning(
        WIFI_PROV_SECURITY_0, NULL, "DoingTV", NULL));
    ESP_LOGI(TAG, "Scan for BLE device 'DoingTV' with Espressif app");
    return ESP_OK;
}


