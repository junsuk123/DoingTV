idf_component_register(
    SRCS "wifi_provision.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_wifi
        esp_netif
        esp_event
        nvs_flash
        wifi_provisioning
        bt
        esp_system
)
