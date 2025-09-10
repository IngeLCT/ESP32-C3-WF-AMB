#pragma once
#include "stdbool.h"
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif

bool wifi_store_has_credentials(void);
esp_err_t wifi_store_load(char *ssid, size_t ssid_len, char *pass, size_t pass_len);
esp_err_t wifi_store_save(const char *ssid, const char *pass);
esp_err_t wifi_store_clear(void);

#ifdef __cplusplus
}
#endif