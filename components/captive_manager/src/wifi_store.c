#include "wifi_store.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>

#define WIFI_STORE_NS   "wifi_cfg"
#define WIFI_KEY_SSID   "ssid"
#define WIFI_KEY_PASS   "pass"

static esp_err_t ensure_nvs_open(nvs_handle_t *h, nvs_open_mode mode) {
    return nvs_open(WIFI_STORE_NS, mode, h);
}

bool wifi_store_has_credentials(void) {
    nvs_handle_t h;
    if (ensure_nvs_open(&h, NVS_READONLY) != ESP_OK) return false;
    size_t len=0;
    esp_err_t err = nvs_get_str(h, WIFI_KEY_SSID, NULL, &len);
    nvs_close(h);
    return err == ESP_OK && len > 0;
}

esp_err_t wifi_store_load(char *ssid, size_t ssid_len, char *pass, size_t pass_len) {
    if (!ssid || !pass) return ESP_ERR_INVALID_ARG;
    ssid[0]=0; pass[0]=0;
    nvs_handle_t h;
    esp_err_t err = ensure_nvs_open(&h, NVS_READONLY);
    if (err != ESP_OK) return err;
    size_t l1 = ssid_len;
    err = nvs_get_str(h, WIFI_KEY_SSID, ssid, &l1);
    if (err != ESP_OK) { nvs_close(h); return err; }
    size_t l2 = pass_len;
    err = nvs_get_str(h, WIFI_KEY_PASS, pass, &l2);
    if (err == ESP_ERR_NVS_NOT_FOUND) { pass[0]=0; err = ESP_OK; }
    nvs_close(h);
    return err;
}

esp_err_t wifi_store_save(const char *ssid, const char *pass) {
    if (!ssid) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = ensure_nvs_open(&h, NVS_READWRITE);
    if (err != ESP_OK) return err;
    ESP_ERROR_CHECK(nvs_set_str(h, WIFI_KEY_SSID, ssid));
    ESP_ERROR_CHECK(nvs_set_str(h, WIFI_KEY_PASS, pass?pass:""));
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t wifi_store_clear(void) {
    nvs_handle_t h;
    esp_err_t err = ensure_nvs_open(&h, NVS_READWRITE);
    if (err != ESP_OK) return err;
    nvs_erase_key(h, WIFI_KEY_SSID);
    nvs_erase_key(h, WIFI_KEY_PASS);
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}
