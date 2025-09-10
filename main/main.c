// Core
#include <stdio.h>
#include <time.h>
#include <stdint.h>
#include <string.h>

// ESP-IDF
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "esp_sntp.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"

// IMPORTANTE: headers para eventos y Wi-Fi
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"

// Project
#include "sensors.h"
#include "firebase.h"
#include "Privado.h"
#include "captive_manager.h"

void geoapify_fetch_once(void);

#define SENSOR_TASK_STACK 10240
#define ENABLE_HTTP_VERBOSE 1
#define LOG_EACH_SAMPLE 1

static const char *TAG = "ESP-WROVER-FB";

static void init_sntp_and_time(void) {
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_set_sync_mode(SNTP_SYNC_MODE_IMMED);
    esp_sntp_init();
    setenv("TZ", "UTC6", 1);
    tzset();
    for (int i = 0; i < 100; ++i) {
        time_t now = 0;
        time(&now);
        if (now > 1609459200) break;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// ------------ SENSOR TASK ------------
void sensor_task(void *pv) {
    SensorData data;

    time_t start_epoch;
    struct tm start_tm_info;
    char inicio_str[20];
    time(&start_epoch);
    localtime_r(&start_epoch, &start_tm_info);
    strftime(inicio_str, sizeof(inicio_str), "%H:%M:%S", &start_tm_info);

    bool first_send = true;
    const uint32_t TOKEN_REFRESH_INTERVAL_SEC = 50 * 60;
    time_t last_token_refresh = time(NULL);

    geoapify_fetch_once();

    if (firebase_init() != 0) {
        ESP_LOGE(TAG, "Error inicializando Firebase");
        vTaskDelete(NULL);
    }
    firebase_delete("/historial_mediciones");

    const int SAMPLES_PER_BATCH = 5;
    const TickType_t SAMPLE_DELAY_TICKS = pdMS_TO_TICKS(60000);
    int sample_count = 0;

    double sum_pm1p0=0, sum_pm2p5=0, sum_pm4p0=0, sum_pm10p0=0, sum_voc=0, sum_nox=0, sum_avg_temp=0, sum_avg_hum=0;
    uint32_t sum_co2 = 0;
    char last_fecha_str[20] = "";

    while (1) {
        if (sensors_read(&data) == ESP_OK) {
            sample_count++;
            sum_pm1p0 += data.pm1p0;
            sum_pm2p5 += data.pm2p5;
            sum_pm4p0 += data.pm4p0;
            sum_pm10p0 += data.pm10p0;
            sum_voc += data.voc;
            sum_nox += data.nox;
            sum_avg_temp += data.avg_temp;
            sum_avg_hum += data.avg_hum;
            sum_co2 += data.co2;
#if LOG_EACH_SAMPLE
            ESP_LOGI(TAG,
                "Muestra %d/%d: PM1.0=%.2f PM2.5=%.2f PM4.0=%.2f PM10=%.2f VOC=%.1f NOx=%.1f CO2=%u Temp=%.2fC Hum=%.2f%%",
                sample_count, SAMPLES_PER_BATCH, data.pm1p0, data.pm2p5, data.pm4p0, data.pm10p0,
                data.voc, data.nox, data.co2, data.avg_temp, data.avg_hum);
#endif
        } else {
            ESP_LOGW(TAG, "Error leyendo sensores (batch %d)", sample_count);
        }

        time_t now_epoch_check = time(NULL);
        if ((now_epoch_check - last_token_refresh) >= TOKEN_REFRESH_INTERVAL_SEC) {
            ESP_LOGI(TAG, "Refrescando token (intervalo 50m)...");
            int r = firebase_refresh_token();
            if (r == 0) ESP_LOGI(TAG, "Token refresh OK"); else ESP_LOGW(TAG, "Fallo refresh token (%d)", r);
            last_token_refresh = now_epoch_check;
        }

        if (sample_count >= SAMPLES_PER_BATCH) {
            time_t now_epoch;
            struct tm tm_info;
            time(&now_epoch);
            localtime_r(&now_epoch, &tm_info);
            char hora_envio[16];
            strftime(hora_envio, sizeof(hora_envio), "%H:%M:%S", &tm_info);
            char fecha_actual[20];
            strftime(fecha_actual, sizeof(fecha_actual), "%d-%m-%y", &tm_info);

            SensorData avg = {0};
            double denom = (double)sample_count;
            avg.pm1p0 = (float)(sum_pm1p0 / denom);
            avg.pm2p5 = (float)(sum_pm2p5 / denom);
            avg.pm4p0 = (float)(sum_pm4p0 / denom);
            avg.pm10p0 = (float)(sum_pm10p0 / denom);
            avg.voc = (float)(sum_voc / denom);
            avg.nox = (float)(sum_nox / denom);
            avg.avg_temp = (float)(sum_avg_temp / denom);
            avg.avg_hum  = (float)(sum_avg_hum / denom);
            avg.co2 = (uint16_t)(sum_co2 / sample_count);
            avg.scd_temp = avg.avg_temp;
            avg.scd_hum = avg.avg_hum;
            avg.sen_temp = avg.avg_temp;
            avg.sen_hum = avg.avg_hum;

            char json[384];
            if (first_send) {
                sensors_format_json(&avg, hora_envio, fecha_actual, inicio_str, json, sizeof(json));
                strncpy(last_fecha_str, fecha_actual, sizeof(last_fecha_str)-1);
                last_fecha_str[sizeof(last_fecha_str)-1] = '\0';
                first_send = false;
            } else {
                if (strncmp(last_fecha_str, fecha_actual, sizeof(last_fecha_str)) != 0) {
                    snprintf(json, sizeof(json),
                        "{\"pm1p0\":%.2f,\"pm2p5\":%.2f,\"pm4p0\":%.2f,\"pm10p0\":%.2f,"
                        "\"voc\":%.1f,\"nox\":%.1f,\"cTe\":%.2f,\"cHu\":%.2f,\"co2\":%u,"
                        "\"fecha\":\"%s\",\"hora\":\"%s\"}",
                        avg.pm1p0, avg.pm2p5, avg.pm4p0, avg.pm10p0,
                        avg.voc, avg.nox, avg.avg_temp, avg.avg_hum,
                        avg.co2, fecha_actual, hora_envio);
                    strncpy(last_fecha_str, fecha_actual, sizeof(last_fecha_str)-1);
                    last_fecha_str[sizeof(last_fecha_str)-1] = '\0';
                } else {
                    snprintf(json, sizeof(json),
                        "{\"pm1p0\":%.2f,\"pm2p5\":%.2f,\"pm4p0\":%.2f,\"pm10p0\":%.2f,"
                        "\"voc\":%.1f,\"nox\":%.1f,\"cTe\":%.2f,\"cHu\":%.2f,\"co2\":%u,"
                        "\"hora\":\"%s\"}",
                        avg.pm1p0, avg.pm2p5, avg.pm4p0, avg.pm10p0,
                        avg.voc, avg.nox, avg.avg_temp, avg.avg_hum,
                        avg.co2, hora_envio);
                }
            }
            ESP_LOGI(TAG, "JSON promedio 30m: %s", json);
            firebase_push("/historial_mediciones", json);

            sample_count = 0;
            sum_pm1p0=sum_pm2p5=sum_pm4p0=sum_pm10p0=sum_voc=sum_nox=sum_avg_temp=sum_avg_hum=0;
            sum_co2 = 0;
        }

        vTaskDelay(SAMPLE_DELAY_TICKS);
    }
}

// ---- Event handler (asegúrate de que coincide con la firma estándar) ----
static void wifi_event_handler(void *arg,
                               esp_event_base_t base,
                               int32_t id,
                               void *data) {
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        captive_manager_notify_sta_got_ip();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t*)data;
        captive_manager_notify_sta_disconnected(disc ? disc->reason : -1);
    }
}

// ----------- APP MAIN -----------
void app_main(void) {

#if ENABLE_HTTP_VERBOSE
    esp_log_level_set("HTTP_CLIENT", ESP_LOG_VERBOSE);
    esp_log_level_set("esp-tls", ESP_LOG_VERBOSE);
    esp_log_level_set("tls", ESP_LOG_VERBOSE);
    esp_log_level_set("FirebaseApp", ESP_LOG_VERBOSE);
    esp_log_level_set("RTDB", ESP_LOG_VERBOSE);
#endif

    captive_manager_cfg_t cfg = {
        .ap_ssid = CONFIG_CAPTIVE_MANAGER_AP_SSID,
        .ap_pass = CONFIG_CAPTIVE_MANAGER_AP_PASS,
        .connectivity_url = CONFIG_CAPTIVE_MANAGER_CONNECTIVITY_URL,
        .check_interval_ms = CONFIG_CAPTIVE_MANAGER_CHECK_INTERVAL_MS,
        .verify_success_needed = CONFIG_CAPTIVE_MANAGER_VERIFY_SUCCESS_N,
        .max_scan_aps = CONFIG_CAPTIVE_MANAGER_MAX_SCAN_APS,
        .conn_max_attempts = CONFIG_CAPTIVE_MANAGER_CONN_MAX_ATTEMPTS,
        .conn_retry_delay_ms = CONFIG_CAPTIVE_MANAGER_CONN_RETRY_DELAY_MS,
        .startup_check_delay_ms = CONFIG_CAPTIVE_MANAGER_STARTUP_CHECK_DELAY_MS
    };

    ESP_ERROR_CHECK(captive_manager_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(captive_manager_start());

    bool operational_started = false;

    // Sensores y Firebase solo se inicializan cuando el portal ya no está activo
    while (true) {
        if (!operational_started && captive_manager_get_state() == CAP_STATE_OPERATIONAL) {
            operational_started = true;
            ESP_LOGI(TAG,"Red lista con internet. Iniciando SNTP, sensores y Firebase...");
            init_sntp_and_time();
            esp_err_t ret = sensors_init_all();
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Fallo al inicializar sensores: %s", esp_err_to_name(ret));
            } else {
                xTaskCreate(sensor_task, "sensor_task", SENSOR_TASK_STACK, NULL, 5, NULL);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}