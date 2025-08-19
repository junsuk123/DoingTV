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

    // 2) 네트워크/이벤트 루프/Wi‑Fi
    ESP_ERROR_CHECK(esp_netif_init());

    esp_err_t evt_ret = esp_event_loop_create_default();
    if (evt_ret != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(evt_ret);
    } else {
        ESP_LOGI(TAG, "event loop already created");
    }

    // ★ STA 기본 인터페이스 생성 (필수)
    ESP_ERROR_CHECK(esp_netif_create_default_wifi_sta());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // ★ NVS에 Wi‑Fi 설정을 저장하도록 스토리지 모드 지정
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));

    // 3) 이벤트 핸들러 등록
    ESP_ERROR_CHECK(esp_event_handler_register(
        WIFI_PROV_EVENT, ESP_EVENT_ANY_ID, provision_event_handler, NULL));

    // 4) 프로비저닝 매니저 초기화(BLE)
    wifi_prov_mgr_config_t mgr_cfg = {
        .scheme               = wifi_prov_scheme_ble,
        .scheme_event_handler = WIFI_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BT
    };
    ESP_ERROR_CHECK(wifi_prov_mgr_init(mgr_cfg));

    // ★ 매니저 init 이후, 현재 프로비저닝 여부 조회
    bool provisioned = false;
    ESP_ERROR_CHECK(wifi_prov_mgr_is_provisioned(&provisioned));

    if (provisioned && !force_prov) {
        ESP_LOGI(TAG, "Already provisioned → use saved STA config in NVS");
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_start());   // NVS에서 마지막 STA 설정 자동 로드
        ESP_ERROR_CHECK(esp_wifi_connect());
        return ESP_OK;
    }

    if (provisioned && force_prov) {
        ESP_LOGW(TAG, "Force provisioning requested; start BLE provisioning again");
        // 필요시 이전 세션 정리를 원하면 여기서 mgr deinit 가능하지만 보통 불필요
        // wifi_prov_mgr_deinit();
    }

    // 5) 프로비저닝 시작(BLE)
    ESP_LOGI(TAG, "Starting BLE provisioning service...");
    ESP_ERROR_CHECK(wifi_prov_mgr_start_provisioning(
        WIFI_PROV_SECURITY_0,   // 보안 Off (필요시 SEC1로 변경)
        NULL,
        "DoingTV",              // BLE 기기 이름
        NULL                    // BLE 스킴은 service_key 사용 안 함
    ));
    ESP_LOGI(TAG, "Scan for BLE device 'DoingTV' with Espressif app");

    return ESP_OK;
}

