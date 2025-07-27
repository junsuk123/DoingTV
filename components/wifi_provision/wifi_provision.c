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

static void provision_event_handler(void *arg, esp_event_base_t base,
                                    int32_t id, void *data)
{
    switch (id) {
    case WIFI_PROV_CRED_RECV: {
        wifi_config_t *wifi_cfg = (wifi_config_t *)data;
        ESP_LOGI(TAG, "Received WiFi credentials: SSID=%s, PW=%s",
                 (char *)wifi_cfg->sta.ssid,
                 (char *)wifi_cfg->sta.password);
        ESP_ERROR_CHECK( esp_wifi_set_mode(WIFI_MODE_STA) );
        ESP_ERROR_CHECK( esp_wifi_set_config(WIFI_IF_STA, wifi_cfg) );
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
        // 1) 중단
        wifi_prov_mgr_stop_provisioning();
        // 2) 매니저 해제
        wifi_prov_mgr_deinit();
        ESP_LOGI(TAG, "Provisioning manager deinitialized");
        // 3) BLE 컨트롤러 해제
        if (esp_bt_controller_disable() != ESP_OK) {
            ESP_LOGW(TAG, "BT controller disable failed");
        }
        if (esp_bt_controller_deinit() != ESP_OK) {
            ESP_LOGW(TAG, "BT controller deinit failed");
        }
        ESP_LOGI(TAG, "BLE controller deinitialized");
        break;
    default:
        break;
    }
}

esp_err_t wifi_provision_init(bool force_prov)
{
    // 1) NVS 초기화
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 2) TCP/IP, 이벤트 루프, Wi-Fi 초기화
    ESP_ERROR_CHECK(esp_netif_init());
    // ESP_ERROR_CHECK(esp_netif_create_default_wifi_sta());

    esp_err_t evt_ret = esp_event_loop_create_default();
    if (evt_ret == ESP_ERR_INVALID_STATE) {
        ESP_LOGI(TAG, "이벤트 루프 이미 생성됨, 넘어갑니다.");
    } else {
        ESP_ERROR_CHECK(evt_ret);
    }
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // 3) 프로비저닝 여부 확인
    bool provisioned = false;
    ESP_ERROR_CHECK(wifi_prov_mgr_is_provisioned(&provisioned));

    // 3-A) 이미 프로비저닝되어 있고, 강제모드가 아니면 STA 모드로 바로 연결
    if (provisioned && !force_prov) {
        ESP_LOGI(TAG, "Already provisioned → STA 모드 시작");
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_start());
        return ESP_OK;
    }

    // 3-B) 이미 프로비저닝되었지만, 강제 프로비저닝 모드이면
    if (provisioned && force_prov) {
        ESP_LOGW(TAG, "Force provisioning → 기존 manager deinit");
        // wifi_prov_mgr_deinit()는 void 반환이므로 그냥 호출
        wifi_prov_mgr_deinit();
    }

    // 4) 이벤트 핸들러 등록
    ESP_ERROR_CHECK(esp_event_handler_register(
        WIFI_PROV_EVENT, ESP_EVENT_ANY_ID,
        provision_event_handler, NULL));

    // 5) manager 초기화 (BLE 스킴)
    wifi_prov_mgr_config_t mgr_cfg = {
        .scheme                = wifi_prov_scheme_ble,
        .scheme_event_handler  = WIFI_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BT
    };
    ESP_ERROR_CHECK(wifi_prov_mgr_init(mgr_cfg));

    // 6) provisioning 서비스 시작 (BLE)
    ESP_LOGI(TAG, "Starting BLE provisioning service...");
    ESP_ERROR_CHECK(wifi_prov_mgr_start_provisioning(
        WIFI_PROV_SECURITY_0,  // no security
        NULL,                  // no security params
        "DoingTV",          // BLE 장치 이름
        NULL                   // BLE 모드에선 무시
    ));
    ESP_LOGI(TAG, "Scan for BLE device 'DoingTV' with Espressif app");

    return ESP_OK;
}
