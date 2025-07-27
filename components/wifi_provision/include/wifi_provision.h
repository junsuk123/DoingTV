#pragma once

#include <stdbool.h>    // bool 정의
#include "esp_err.h"

/**
 * @brief  Wi-Fi provisioning 초기화
 * @param  force_prov  true 면, NVS에 이미 프로비저닝 정보가 있어도 BLE 프로비저닝 모드로 진입
 * @return ESP_OK on success, otherwise an error code
 */
esp_err_t wifi_provision_init(bool force_prov);
