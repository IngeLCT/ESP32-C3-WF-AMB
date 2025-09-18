// Geoapify.c  -> ahora: WiFi positioning con Unwired Labs
#include <string.h>
#include <stdint.h>
#include <netdb.h>
#include <sys/socket.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sensors.h"
#include "Privado.h"   // aquí defines UNWIRED_TOKEN

static const char *GEO_TAG = "GEO_UNWIRED";
#define UNWIRED_URL "https://us1.unwiredlabs.com/v2/process.php"
#define WIFI_MAX_APS 6          // cuántos APs enviar (ajusta si quieres)
#define REQ_BODY_MAX 1536
static char req_body[REQ_BODY_MAX];
static char resp_body[2048];

// helper: convierte bssid a string xx:xx:...
static void mac_to_str(const uint8_t *mac, char *out, size_t outlen) {
    if (!mac || !out) return;
    snprintf(out, outlen, "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// accumulador HTTP (similar a tu implementacion previa)
typedef struct {
    char *buf; int max; int len;
} geo_accum_t;

static esp_err_t geo_http_evt(esp_http_client_event_t *evt) {
    geo_accum_t *acc = (geo_accum_t*)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && acc && evt->data_len > 0 && acc->len < acc->max - 1) {
        int copy = evt->data_len;
        if (copy > (acc->max - 1 - acc->len)) copy = acc->max - 1 - acc->len;
        memcpy(acc->buf + acc->len, evt->data, copy);
        acc->len += copy;
        acc->buf[acc->len] = '\0';
    }
    return ESP_OK;
}

// parse simple json: busca "lat": number, "lon": number, "accuracy": number
static bool parse_unwired_response(const char *json, double *out_lat, double *out_lon, int *out_acc) {
    if (!json) return false;
    const char *p_lat = strstr(json, "\"lat\":");
    const char *p_lon = strstr(json, "\"lon\":");
    const char *p_acc = strstr(json, "\"accuracy\":");
    if (!p_lat || !p_lon) return false;
    *out_lat = atof(p_lat + 6);
    *out_lon = atof(p_lon + 6);
    if (p_acc) *out_acc = atoi(p_acc + 11);
    else *out_acc = -1;
    return true;
}

// públic function -> realiza escaneo wifi + POST a Unwired Labs + guarda resultado en sensors
void geoapify_fetch_once_wifi_unwired(void) {
    ESP_LOGI(GEO_TAG, "Iniciando escaneo WiFi para Unwired Labs...");

    // 1) Hacer escaneo (modo STA debe estar inicializado antes)
    wifi_scan_config_t scan_conf = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = true
    };
    esp_err_t r = esp_wifi_scan_start(&scan_conf, true); // bloqueante
    if (r != ESP_OK) {
        ESP_LOGW(GEO_TAG, "esp_wifi_scan_start fallo: %s", esp_err_to_name(r));
        sensors_set_city_state("WiFiScan-Error");
        return;
    }

    uint16_t ap_num = WIFI_MAX_APS;
    wifi_ap_record_t ap_records[WIFI_MAX_APS];
    r = esp_wifi_scan_get_ap_records(&ap_num, ap_records);
    if (r != ESP_OK) {
        ESP_LOGW(GEO_TAG, "esp_wifi_scan_get_ap_records fallo: %s", esp_err_to_name(r));
        sensors_set_city_state("WiFiScan-Error2");
        return;
    }
    if (ap_num == 0) {
        ESP_LOGW(GEO_TAG, "No se encontraron APs");
        sensors_set_city_state("NoAPs");
        return;
    }
    ESP_LOGI(GEO_TAG, "APs encontrados: %d", ap_num);

    // 2) Construir JSON para Unwired Labs
    // formato: { "token":"...","wifi":[{"bssid":"aa:bb:...","signal":-65}, ...] }
    size_t pos = 0;
    pos += snprintf(req_body + pos, sizeof(req_body) - pos, "{\"token\":\"%s\",\"wifi\":[", UNWIRED_TOKEN);

    for (int i = 0; i < ap_num && pos < (sizeof(req_body) - 128); ++i) {
        char mac_str[18];
        mac_to_str(ap_records[i].bssid, mac_str, sizeof(mac_str));
        // esp_wifi_scan returns RSSI as int8_t
        int signal = ap_records[i].rssi;
        pos += snprintf(req_body + pos, sizeof(req_body) - pos,
                        "{\"bssid\":\"%s\",\"signal\":%d}%s",
                        mac_str, signal, (i == (ap_num - 1)) ? "" : ",");
    }
    pos += snprintf(req_body + pos, sizeof(req_body) - pos, "]}");
    ESP_LOGD(GEO_TAG, "Request JSON: %s", req_body);

    // 3) HTTP POST
    memset(resp_body, 0, sizeof(resp_body));
    geo_accum_t acc = { .buf = resp_body, .max = sizeof(resp_body), .len = 0 };
    esp_http_client_config_t cfg = {
        .url = UNWIRED_URL,
        .event_handler = geo_http_evt,
        .user_data = &acc,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 8000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGW(GEO_TAG, "esp_http_client_init fallo");
        sensors_set_city_state("HTTP-Init-Err");
        return;
    }
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, req_body, strlen(req_body));

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        ESP_LOGI(GEO_TAG, "HTTP status=%d, response len=%d", status, acc.len);
        if (acc.len > 0) {
            ESP_LOGD(GEO_TAG, "Resp: %s", acc.buf);
            double lat=0, lon=0; int accm=-1;
            if (parse_unwired_response(acc.buf, &lat, &lon, &accm)) {
                char city_state[64];
                if (accm >= 0)
                    snprintf(city_state, sizeof(city_state), "%.6f,%.6f (acc=%dm)", lat, lon, accm);
                else
                    snprintf(city_state, sizeof(city_state), "%.6f,%.6f", lat, lon);
                sensors_set_city_state(city_state);
                ESP_LOGI(GEO_TAG, "Location = %s", city_state);
            } else {
                ESP_LOGW(GEO_TAG, "No se pudo parsear respuesta Unwired");
                sensors_set_city_state("Unwired-NoParse");
            }
        } else {
            ESP_LOGW(GEO_TAG, "Respuesta vacía");
            sensors_set_city_state("Unwired-EmptyResp");
        }
    } else {
        ESP_LOGW(GEO_TAG, "esp_http_client_perform fallo: %s", esp_err_to_name(err));
        sensors_set_city_state("Unwired-HTTPfail");
    }

    esp_http_client_cleanup(client);
}
