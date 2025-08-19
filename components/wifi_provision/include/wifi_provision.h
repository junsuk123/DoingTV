#pragma once

#include <stdbool.h>    // bool 정의
#include "esp_err.h"

/**
 * @brief Wi‑Fi provisioning 초기화
 * @param force_prov  true면, NVS에 기존 정보가 있어도 BLE 프로비저닝 강제 진입
 * @note  WIFI_STORAGE_FLASH 모드로 저장되어 재부팅 후에도 SSID/PW가 유지됨
 */
esp_err_t wifi_provision_init(bool force_prov);
