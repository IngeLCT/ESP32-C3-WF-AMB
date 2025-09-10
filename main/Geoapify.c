#include <string.h>
#include <stdint.h>
#include <netdb.h>
#include <sys/socket.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_timer.h"
#include "sensors.h"
#include "Privado.h"

static const char *GEO_TAG = "GEOAPIFY";
#define GEOAPIFY_URL_BASE "https://api.geoapify.com/v1/ipinfo?apiKey="
#define GEOAPIFY_BODY_MAX 1536
static char geo_body[GEOAPIFY_BODY_MAX];
static bool geo_ready = false;

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

static void geo_parse_city_state(const char *json) {
    if (!json) return;
    char city[32] = ""; char state[32] = "";
#define EXTRACT(after_key_ptr, outbuf) do { \
    const char *p = (after_key_ptr); \
    if (p) { \
        const char *nm = strstr(p, "\"name\":\""); \
        if (nm) { \
            nm += 8; \
            const char *endq = strchr(nm, '"'); \
            if (endq) { \
                size_t span = (size_t)(endq - nm); \
                if (span >= sizeof(outbuf)) span = sizeof(outbuf)-1; \
                memcpy(outbuf, nm, span); \
                outbuf[span] = '\0'; \
            } \
        } \
    } \
} while(0)
    const char *p_city = strstr(json, "\"city\""); EXTRACT(p_city, city);
    const char *p_state = strstr(json, "\"state\""); EXTRACT(p_state, state);
#undef EXTRACT
    if (city[0] || state[0]) {
        char city_state[64]; size_t pos = 0;
        if (city[0]) { size_t c = strlen(city); if (c>63) c=63; memcpy(city_state, city, c); pos=c; }
        if (state[0]) { if (pos && pos<63) city_state[pos++]='-'; size_t s=strlen(state); if (s>63-pos) s=63-pos; memcpy(city_state+pos, state, s); pos+=s; }
        city_state[pos]='\0';
        sensors_set_city_state(city_state);
        ESP_LOGI(GEO_TAG, "ciudad=%s", city_state);
    } else {
        ESP_LOGW(GEO_TAG, "city/state no encontrados");
    }
}

void geoapify_fetch_once(void) {
    if (geo_ready) return; // ya obtenido previamente
    char url[256];
    snprintf(url, sizeof(url), "%s%s", GEOAPIFY_URL_BASE, GEOAPIFY_API_KEY);

    const int MAX_ATTEMPTS = 5;
    bool success = false;
    for (int attempt = 1; attempt <= MAX_ATTEMPTS && !success; ++attempt) {
        ESP_LOGI(GEO_TAG, "Intento %d/%d Geoapify", attempt, MAX_ATTEMPTS);
        // DNS
        struct addrinfo hints = {0}; hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM; struct addrinfo *res=NULL;
        int dns_err = getaddrinfo("api.geoapify.com", NULL, &hints, &res);
        if (dns_err!=0 || !res) {
            ESP_LOGW(GEO_TAG, "DNS fallo (%d)", dns_err);
        } else {
            int count=0; for (struct addrinfo *p=res; p&&count<2; p=p->ai_next,++count){ char ip[64]; inet_ntop(p->ai_family,&((struct sockaddr_in*)p->ai_addr)->sin_addr,ip,sizeof ip); ESP_LOGI(GEO_TAG,"DNS IP[%d]=%s",count,ip);} freeaddrinfo(res);
        }

        memset(geo_body,0,sizeof(geo_body)); geo_accum_t acc={ .buf=geo_body, .max=GEOAPIFY_BODY_MAX, .len=0};
        esp_http_client_config_t cfg = { .url=url, .crt_bundle_attach=esp_crt_bundle_attach, .timeout_ms=6000, .user_data=&acc, .event_handler=geo_http_evt, .buffer_size=1024, .buffer_size_tx=512};
        esp_http_client_handle_t client = esp_http_client_init(&cfg);
        if (!client) {
            ESP_LOGW(GEO_TAG, "no client");
            continue; // retry
        }
        int64_t t0=esp_timer_get_time(); esp_err_t err = esp_http_client_perform(client); int64_t t1=esp_timer_get_time();
        if (err==ESP_OK) {
            int status=esp_http_client_get_status_code(client); int cl=esp_http_client_get_content_length(client);
            ESP_LOGI(GEO_TAG,"OK status=%d cl=%d ms=%lld len=%d", status, cl, (long long)((t1-t0)/1000), acc.len);
            if (acc.len>0) {
                geo_ready = true;
                geo_parse_city_state(acc.buf);
                success = true;
            } else {
                ESP_LOGW(GEO_TAG, "cuerpo vacio");
            }
        } else {
            ESP_LOGW(GEO_TAG, "HTTP fallo %s", esp_err_to_name(err));
        }
        esp_http_client_cleanup(client);
        if (!success) {
            // pequeña espera progresiva
            vTaskDelay(pdMS_TO_TICKS(300 * attempt));
        }
    }
    if (!success) {
        sensors_set_city_state("Error-Error");
        ESP_LOGW(GEO_TAG, "Geoapify agotó reintentos, ciudad=Error-Error");
    }
}
