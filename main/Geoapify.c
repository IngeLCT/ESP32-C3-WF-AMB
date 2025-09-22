// ubicacion.c  -> Unwired Labs (IP-only)
// - Usa únicamente geolocalización por IP (WiFi deshabilitado en cuenta).
// - Deja comentado el flujo WiFi para reactivarlo cuando lo habiliten.
// - Evita overflow de stack usando buffers estáticos.
// - Devuelve city-state en sensors_set_city_state() si viene address_detail,
//   si no, usa "lat,lon (acc=..m)" como respaldo.

#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_timer.h"
//#include "esp_wifi.h" // <- NO se usa ahora (IP only)
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "sensors.h"     // sensors_set_city_state(...)
#include "Privado.h"     // UNWIRED_TOKEN

#ifndef UNWIRED_URL
#define UNWIRED_URL "https://us2.unwiredlabs.com/v2/process.php"
#endif

#define GEO_TAG            "UnwiredLabs"
#define REQ_MAX            1024
#define RESP_MAX           2048
#define HTTP_TIMEOUT_MS    10000

// ---------------- Buffers estáticos (evita pila) ----------------
static char g_req_body[REQ_MAX];     // JSON request
typedef struct {
    char buf[RESP_MAX];
    int  len;
} geo_accum_t;
static geo_accum_t g_acc;            // acumulador de respuesta

// ---------------- Utilidades de log/parse ----------------
static void log_json_chunks(const char *buf, int len) {
    const int chunk = 384;
    for (int i = 0; i < len; i += chunk) {
        int n = (i + chunk <= len) ? chunk : (len - i);
        ESP_LOGI(GEO_TAG, "Resp(%d..%d): %.*s", i, i + n, n, buf + i);
    }
}

static bool sniff_kv(const char *json, const char *key, char *out, size_t outlen) {
    const char *p = strstr(json, key);
    if (!p) return false;
    p = strchr(p, ':'); if (!p) return false; p++;
    while (*p == ' ' || *p == '"') p++;
    const char *start = p;
    while (*p && *p != '"' && *p != ',' && *p != '}' && *p != '\n' && *p != '\r') p++;
    size_t n = (size_t)(p - start);
    if (n >= outlen) n = outlen - 1;
    memcpy(out, start, n);
    out[n] = 0;
    return true;
}

static bool parse_unwired_response(const char *json, double *lat, double *lon, int *acc) {
    const char *p; char *endp;
    p = strstr(json, "\"lat\""); if (!p) return false;
    p = strchr(p, ':'); if (!p) return false; p++;
    double vlat = strtod(p, &endp); if (p == endp) return false;

    p = strstr(endp, "\"lon\""); if (!p) return false;
    p = strchr(p, ':'); if (!p) return false; p++;
    double vlon = strtod(p, &endp); if (p == endp) return false;

    int vacc = -1;
    p = strstr(endp, "\"accuracy\"");
    if (p) { p = strchr(p, ':'); if (p) { p++; vacc = (int)strtol(p, &endp, 10); } }

    if (lat) *lat = vlat;
    if (lon) *lon = vlon;
    if (acc) *acc = vacc;
    return true;
}

// Extrae "city"/"state" de address_detail {...}
static bool parse_address_detail_city_state(const char *json,
                                            char *city, size_t city_sz,
                                            char *state, size_t state_sz)
{
    const char *p = strstr(json, "\"address_detail\"");
    if (!p) return false;
    p = strchr(p, '{'); if (!p) return false;
    const char *obj = ++p;
    int depth = 1;
    while (*p && depth > 0) {
        if (*p == '{') depth++;
        else if (*p == '}') depth--;
        p++;
    }
    if (depth != 0) return false;
    size_t len = (size_t)((p - 1) - obj);
    if (len == 0) return false;

    char tmp[256];
    if (len >= sizeof(tmp)) len = sizeof(tmp) - 1;
    memcpy(tmp, obj, len); tmp[len] = 0;

    bool ok = false;
    if (city && city_sz)  ok |= sniff_kv(tmp, "\"city\"",  city,  city_sz);
    if (state && state_sz) ok |= sniff_kv(tmp, "\"state\"", state, state_sz);
    return ok;
}

// Une "city" y "state" como "city-state"
static void make_city_state_hyphen(const char *city, const char *state,
                                   char *out, size_t outlen)
{
    if (!out || outlen == 0) return;
    out[0] = '\0';
    if (city && city[0]) {
        // strlcpy/strlcat están disponibles en ESP-IDF (newlib)
        strlcpy(out, city, outlen);
    }
    if (state && state[0]) {
        if (out[0]) strlcat(out, "-", outlen);
        strlcat(out, state, outlen);
    }
    if (out[0] == '\0') {
        strlcpy(out, "SinCiudad", outlen);
    }
}

// ---------------- HTTP event ----------------
static esp_err_t http_event_handler(esp_http_client_event_t *evt) {
    geo_accum_t *acc = (geo_accum_t*)evt->user_data;
    if (!acc) return ESP_OK;
    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (evt->data && evt->data_len > 0 && acc->len < (int)sizeof(acc->buf)) {
                int can = sizeof(acc->buf) - acc->len - 1; if (can < 0) can = 0;
                int n = (evt->data_len < can) ? evt->data_len : can;
                if (n > 0) {
                    memcpy(acc->buf + acc->len, evt->data, n);
                    acc->len += n;
                    acc->buf[acc->len] = 0;
                }
            }
            break;
        default: break;
    }
    return ESP_OK;
}

// ---------------- (Comentado) Escaneo WiFi y armado de body ----------------
#if 0
#define WIFI_SCAN_MAX      32
#define AP_SEND_MAX        10

static int channel_to_freq_mhz(int ch) {
    if (ch >= 1 && ch <= 14) return 2407 + 5 * ch;
    if (ch >= 32 && ch <= 196) return 5000 + 5 * ch;
    return 0;
}
static int cmp_rssi_desc(const void *a, const void *b) {
    const wifi_ap_record_t *x = (const wifi_ap_record_t*)a;
    const wifi_ap_record_t *y = (const wifi_ap_record_t*)b;
    return (y->rssi - x->rssi);
}
static void mac_to_lower_colon(char *dst, size_t dstlen, const uint8_t mac[6]) {
    snprintf(dst, dstlen, "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}
static int scan_wifi(wifi_ap_record_t *out, int cap) { /* ... */ return 0; }
static int build_unwired_wifi_body_inplace(wifi_ap_record_t *aps, int n,
                                           const char *token,
                                           char *out, size_t outlen)
{
    // ... construir body con "wifi":[{bssid,channel,frequency,signal,age}]
    return 0;
}
#endif
// ---------------------------------------------------------------------------

// Punto de entrada público (IP-only)
void geoapify_fetch_once_wifi_unwired(void) {
#ifndef UNWIREDLABS_TOKEN
    #error "Define UNWIREDLABS_TOKEN en Privado.h"
#endif
    const char *token = UNWIREDLABS_TOKEN;

    // Debounce simple para evitar doble llamada consecutiva
    static int64_t s_last_call_us = 0;
    int64_t now_us = esp_timer_get_time();
    if (now_us - s_last_call_us < 3000000) {  // 3 s
        ESP_LOGW(GEO_TAG, "Llamada ignorada (debounce)");
        return;
    }
    s_last_call_us = now_us;

    // --- Body para IP-only ---
    // Nota: si omites "ip", UL usa la IP pública de la conexión automáticamente.
    int body_len = snprintf(g_req_body, sizeof(g_req_body),
                            "{"
                              "\"token\":\"%s\","
                              "\"address\":2,"
                              "\"fallbacks\":{\"ipf\":1},"
                              "\"accept-language\":\"es\""
                              // Si quieres forzar a usar una IP fija para pruebas en dashboard:
                              // ,\"ip\":\"1.2.3.4\"
                            "}",
                            token);

    ESP_LOGI(GEO_TAG, "Request JSON (IP only): %.*s", body_len, g_req_body);

    // HTTP client
    memset(&g_acc, 0, sizeof(g_acc));

    esp_http_client_config_t cfg = {
        .url = UNWIRED_URL,
        .method = HTTP_METHOD_POST,
        .event_handler = http_event_handler,
        .user_data = &g_acc,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGW(GEO_TAG, "No se pudo crear HTTP client");
        sensors_set_city_state("Unwired-NoClient");
        return;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, g_req_body, body_len);

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        ESP_LOGI(GEO_TAG, "HTTP status=%d, response len=%d", status, g_acc.len);

        if (g_acc.len > 0) {
            ESP_LOGI(GEO_TAG, "Resp cruda (len=%d):", g_acc.len);
            log_json_chunks(g_acc.buf, g_acc.len);

            // Logs útiles
            char st[16] = {0}, msg[160] = {0};
            if (sniff_kv(g_acc.buf, "\"status\"", st, sizeof(st))) ESP_LOGI(GEO_TAG, "status=%s", st);
            if (sniff_kv(g_acc.buf, "\"message\"", msg, sizeof(msg))) ESP_LOGW(GEO_TAG, "message=%s", msg);

            // Parseo principal
            double lat=0, lon=0; int accm=-1;
            if (parse_unwired_response(g_acc.buf, &lat, &lon, &accm)) {
                // City/State si viene address_detail
                char city[64] = {0}, state[64] = {0}, ciudad[160] = {0};
                if (parse_address_detail_city_state(g_acc.buf, city, sizeof(city),
                                                    state, sizeof(state)) &&
                    (city[0] || state[0])) {
                    make_city_state_hyphen(city, state, ciudad, sizeof(ciudad));
                    sensors_set_city_state(ciudad);
                    ESP_LOGI(GEO_TAG, "Ciudad (city-state) = %s", ciudad);
                } else {
                    char coords[64];
                    if (accm >= 0) snprintf(coords, sizeof(coords), "%.6f,%.6f (acc=%dm)", lat, lon, accm);
                    else           snprintf(coords, sizeof(coords), "%.6f,%.6f", lat, lon);
                    sensors_set_city_state(coords);
                    ESP_LOGW(GEO_TAG, "Sin address_detail: usando coords %s", coords);
                }
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
