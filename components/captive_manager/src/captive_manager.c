#include "captive_manager.h"
#include "wifi_store.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "esp_netif_net_stack.h"
#include "lwip/netif.h"
#include "lwip/ip4_addr.h"
#include "nvs_flash.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_client.h"

#include "mdns.h"

#include <string.h>
#include <stdlib.h>

//#define FORCE_AP_NAT_MODE // Comenta esta línea para desactivar el modo forzado

static const char *TAG = "captive_mgr";

static captive_manager_cfg_t g_cfg;
static captive_state_t g_state = CAP_STATE_IDLE;
static httpd_handle_t g_server = NULL;
static httpd_handle_t g_sta_server = NULL; // servidor mínimo en STA
static esp_netif_t *g_ap_netif = NULL;
static esp_netif_t *g_sta_netif = NULL;

static bool g_sta_have_ip = false;
static int  g_verify_success = 0;
static bool g_nat_enabled = false;
static bool g_using_saved = false;
static int  g_connect_attempts = 0;

static bool g_connect_post_pending_save = false; // guardaremos al obtener IP
static char g_pending_ssid[33];
static char g_pending_pass[65];

// Task para reiniciar tras responder HTTP
static void restart_later_task(void *arg) {
    // pequeña espera para permitir que el cliente reciba la respuesta
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
}

// Forward declarations
static void set_state(captive_state_t st);
static esp_err_t start_ap(void);
static esp_err_t start_http(void);
static void scan_start(void);
static void periodic_check_task(void *arg);
static void connect_sta(const char *ssid, const char *pass, bool from_saved);
static void shutdown_ap(void);
// static void maybe_start_captive_after_saved_ip(void); // eliminada por no usarse
// HTTP STA-min server controls
static esp_err_t start_http_sta_minimal(void);
static void stop_http_sta_minimal(void);
static void start_mdns_service(void);

// State helpers
const char* captive_manager_state_str(captive_state_t st) {
    switch(st){
        case CAP_STATE_IDLE: return "IDLE";
        case CAP_STATE_PREP: return "PREP";
        case CAP_STATE_SCAN: return "SCAN";
        case CAP_STATE_CONNECTING: return "CONNECTING";
        case CAP_STATE_WAIT_LOGIN: return "WAIT_LOGIN";
        case CAP_STATE_VERIFY: return "VERIFY";
        case CAP_STATE_OPERATIONAL: return "OPERATIONAL";
        case CAP_STATE_RECAPTIVE: return "RECAPTIVE";
        default: return "UNKNOWN";
    }
}

captive_state_t captive_manager_get_state(void){
    return g_state;
}

// Implementación faltante: cambia el estado y registra transición
static void set_state(captive_state_t st) {
    if (g_state != st) {
        ESP_LOGI(TAG, "STATE: %s -> %s", captive_manager_state_str(g_state), captive_manager_state_str(st));
        g_state = st;
    }
}

bool captive_manager_using_saved(void) {
    return g_using_saved;
}

// Public API
esp_err_t captive_manager_init(const captive_manager_cfg_t *cfg) {
    if (!cfg) return ESP_ERR_INVALID_ARG;
    g_cfg = *cfg;

    // NVS
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    g_ap_netif  = esp_netif_create_default_wifi_ap();
    g_sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t wic = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wic));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_NULL));
    ESP_ERROR_CHECK(esp_wifi_start());

    set_state(CAP_STATE_IDLE);
    return ESP_OK;
}

static void start_with_saved_or_captive(void) {
    #if defined(FORCE_AP_NAT_MODE)
        ESP_LOGI(TAG, "[PRUEBA] AP+NAT router; arrancando AP y conectando STA si hay credenciales");
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
        ESP_ERROR_CHECK(start_ap());
        start_http_sta_minimal(); // solo /wifi/clear
        // Intentar conectar STA con credenciales guardadas si existen
        if (wifi_store_has_credentials()) {
            char ssid[33], pass[65];
            if (wifi_store_load(ssid, sizeof(ssid), pass, sizeof(pass)) == ESP_OK) {
                connect_sta(ssid, pass, true);
            }
        }
        set_state(CAP_STATE_WAIT_LOGIN);
    #else
        if (wifi_store_has_credentials()) {
            char ssid[33], pass[65];
            if (wifi_store_load(ssid, sizeof(ssid), pass, sizeof(pass)) == ESP_OK) {
                ESP_LOGI(TAG,"Credenciales guardadas encontradas: SSID=%s (len pass=%d)", ssid, (int)strlen(pass));
                connect_sta(ssid, pass, true);
                return;
            }
        }
        // No saved
        set_state(CAP_STATE_PREP);
        ESP_ERROR_CHECK(start_ap());
        ESP_ERROR_CHECK(start_http());
        set_state(CAP_STATE_SCAN);
        scan_start();
    #endif
}

esp_err_t captive_manager_start(void) {
    if (g_state != CAP_STATE_IDLE) return ESP_ERR_INVALID_STATE;
    start_with_saved_or_captive();
    // Verificación periódica de internet: solo en modo normal
    #if defined(FORCE_AP_NAT_MODE)
        ESP_LOGI(TAG, "[PRUEBA] periodic_check_task desactivada por modo AP+NAT forzado");
    #else
        xTaskCreate(periodic_check_task, "cap_chk", 2048, NULL, 5, NULL);
    #endif
    return ESP_OK;
}

// WiFi / AP / STA
static esp_err_t start_ap(void) {
    // Si hay servidor mínimo en STA, detenerlo al pasar a AP/recaptive
    stop_http_sta_minimal();
    wifi_config_t ap_cfg = {0};
    snprintf((char*)ap_cfg.ap.ssid, sizeof(ap_cfg.ap.ssid), "%s", g_cfg.ap_ssid);
    snprintf((char*)ap_cfg.ap.password, sizeof(ap_cfg.ap.password), "%s", g_cfg.ap_pass);
    ap_cfg.ap.ssid_len = strlen((char*)ap_cfg.ap.ssid);
    ap_cfg.ap.channel = 1;
    ap_cfg.ap.authmode = strlen(g_cfg.ap_pass) >= 8 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    ap_cfg.ap.max_connection = 4;
    ap_cfg.ap.beacon_interval = 100;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_LOGI(TAG,"AP started SSID=%s", ap_cfg.ap.ssid);
    ESP_LOGI(TAG,"AP started Password=%s", g_cfg.ap_pass);
    start_mdns_service(); // mDNS también en modo AP
    return ESP_OK;
}

static void connect_sta(const char *ssid, const char *pass, bool from_saved) {
    wifi_config_t sta_cfg = {0};
    snprintf((char*)sta_cfg.sta.ssid, sizeof(sta_cfg.sta.ssid), "%s", ssid);
    snprintf((char*)sta_cfg.sta.password, sizeof(sta_cfg.sta.password), "%s", pass?pass:"");
    sta_cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;

    // Mantener AP activo para modo router: usar APSTA
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    set_state(CAP_STATE_CONNECTING);
    g_sta_have_ip = false;
    g_verify_success = 0;
    g_using_saved = from_saved;
    g_connect_attempts = 0;
    g_connect_post_pending_save = !from_saved; // si es nueva, guardaremos al tener IP
    ESP_LOGI(TAG,"(STA) Connecting to SSID=%s saved=%d", sta_cfg.sta.ssid, from_saved);
    esp_wifi_connect();
    start_mdns_service(); // mDNS también en modo STA
}

void captive_manager_notify_sta_got_ip(void) {
    g_sta_have_ip = true;
    ESP_LOGI(TAG,"STA GOT IP (saved=%d)", g_using_saved);
    if (g_connect_post_pending_save) {
        wifi_store_save(g_pending_ssid, g_pending_pass);
        g_connect_post_pending_save = false;
    }

    #if defined(FORCE_AP_NAT_MODE)
        // Modo forzado: siempre habilita NAT y APSTA, sin comprobación de internet
        if (g_sta_netif) esp_netif_set_default_netif(g_sta_netif);
        if (g_ap_netif && g_sta_netif) {
            uint8_t dhcps_offer_option = 0x02;
            esp_netif_dns_info_t dns;
            if (esp_netif_get_dns_info(g_sta_netif, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK) {
                ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_stop(g_ap_netif));
                ESP_ERROR_CHECK(esp_netif_dhcps_option(g_ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER,
                                                       &dhcps_offer_option, sizeof(dhcps_offer_option)));
                ESP_ERROR_CHECK(esp_netif_set_dns_info(g_ap_netif, ESP_NETIF_DNS_MAIN, &dns));
                ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_start(g_ap_netif));
            }
        }
        captive_manager_enable_nat();
        set_state(CAP_STATE_WAIT_LOGIN);
    #else
        // Modo normal: decide según conectividad
        // Pequeña espera opcional para estabilizar la conexión antes de comprobar
        vTaskDelay(pdMS_TO_TICKS(g_cfg.startup_check_delay_ms));
        if (connectivity_portal_open()) {
            // Hay internet: apaga AP, entra en modo STA, deja endpoint /wifi/clear y mDNS
            set_state(CAP_STATE_VERIFY);
            g_verify_success = 0;
        } else {
            // No hay internet: habilita NAT y APSTA
            if (g_sta_netif) esp_netif_set_default_netif(g_sta_netif);
            if (g_ap_netif && g_sta_netif) {
                uint8_t dhcps_offer_option = 0x02;
                esp_netif_dns_info_t dns;
                if (esp_netif_get_dns_info(g_sta_netif, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK) {
                    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_stop(g_ap_netif));
                    ESP_ERROR_CHECK(esp_netif_dhcps_option(g_ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER,
                                                           &dhcps_offer_option, sizeof(dhcps_offer_option)));
                    ESP_ERROR_CHECK(esp_netif_set_dns_info(g_ap_netif, ESP_NETIF_DNS_MAIN, &dns));
                    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_start(g_ap_netif));
                }
            }
            captive_manager_enable_nat();
            set_state(CAP_STATE_WAIT_LOGIN);
        }
    #endif
}

void captive_manager_notify_sta_disconnected(int reason_code) {
    ESP_LOGW(TAG,"STA disconnected (reason=%d) state=%s saved=%d attempts=%d",
             reason_code, captive_manager_state_str(g_state), g_using_saved, g_connect_attempts);
    g_sta_have_ip = false;

    if (g_state == CAP_STATE_CONNECTING && g_using_saved) {
        g_connect_attempts++;
        if (g_connect_attempts < g_cfg.conn_max_attempts) {
            vTaskDelay(pdMS_TO_TICKS(g_cfg.conn_retry_delay_ms));
            esp_wifi_connect();
        } else {
            ESP_LOGE(TAG,"Max reintentos alcanzado, limpiando credenciales y entrando a modo portal");
            wifi_store_clear();
            captive_manager_enter_recaptive();
        }
    } else if (g_state == CAP_STATE_OPERATIONAL) {
        // Podrías decidir volver a recaptive; por ahora solo log
        ESP_LOGW(TAG,"Desconexión en modo operativo (no se implementó recuperación automática)");
    }
}

esp_err_t captive_manager_enter_recaptive(void) {
    ESP_LOGW(TAG,"Entering recaptive mode");
    // Apagar servidor mínimo STA si estuviese activo
    stop_http_sta_minimal();
    captive_manager_disable_nat();
    ESP_ERROR_CHECK(esp_wifi_disconnect());
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    start_ap();
    if (!g_server) start_http();
    g_sta_have_ip = false;
    g_verify_success = 0;
    g_using_saved = false;
    set_state(CAP_STATE_SCAN);
    scan_start();
    return ESP_OK;
}

// Scanning
static void scan_start(void) {
    wifi_scan_config_t sc = {
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE
    };
    esp_wifi_scan_start(&sc, false);
}

// perform_scan_sync_and_respond eliminado: no se usa (reemplazado por scan_get)

// HTTP Handlers (UI mejorada con selección y auto-disable pass)
static esp_err_t root_get(httpd_req_t *r) {

    // Realizar escaneo de redes Wi-Fi
    // HTML con <select> vacío y JS para renderizar redes
    const char *page =
        "<!DOCTYPE html><html><head><title>WF_MNGR_AMBT</title><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<style>body{text-align:center;margin:0}.card{background:#fff;box-shadow:2px 2px 12px rgba(0,0,0,.2);padding:20px;max-width:400px;margin:auto;border-radius:8px}"
        "select,input{width:80%%;padding:12px;margin:10px 0;border:1px solid #ccc;border-radius:4px}input[type=submit]{background:#034078;color:#fff;padding:15px;border:none;border-radius:4px;cursor:pointer}"
        "input[type=submit]:hover{background:#1282A2}label{display:block;margin-top:10px}</style></head><body>"
        "<div style=\"background:#0A1128;padding:10px\"><h1 style=\"color:white;font-size:25px\">Wi-Fi Manager Ambiente</h1></div>"
        "<div class=\"card\"><form id=\"wifiForm\"><label for=\"ssid\">SSID</label><select id=\"ssid\" name=\"ssid\" required></select>"
        "<label for=\"pass\">Pass</label><input type=\"password\" id=\"pass\" name=\"pass\"><input type=\"submit\" value=\"Guardar\"></form></div>"
        "<script>"
        "function loadNetworks() {"
        "fetch('/scan').then(r=>r.json()).then(j=> {"
        "let s=document.getElementById('ssid'); s.innerHTML='';"
        "j.networks.forEach(n=> {"
        "let opt=document.createElement('option');"
        "opt.value=n.ssid; opt.text=n.ssid + (n.open==1 ? ' (Abierta)' : ''); opt.setAttribute('data-open', n.open ? '1' : '0');"
        "s.appendChild(opt);"
        "});"
        "s.dispatchEvent(new Event('change'));"
        "});"
        "}"
        "const s=document.getElementById('ssid'),p=document.getElementById('pass');"
        "function updatePassField() {"
        "  const o=s.options[s.selectedIndex];"
        "  if(o && o.getAttribute('data-open')==='1') {"
        "    p.disabled=true; p.value='';"
        "  } else {"
        "    p.disabled=false;"
        "  }"
        "}"
        "s.addEventListener('change',updatePassField);"
        "window.onload=()=>{loadNetworks();updatePassField();};"
        // Nuevo: enviar datos como JSON vía fetch
        "document.getElementById('wifiForm').onsubmit=function(e){"
        "  e.preventDefault();"
        "  const ssid=s.value, pass=p.value;"
        "  fetch('/save', {"
        "    method:'POST',"
        "    headers:{'Content-Type':'application/json'},"
        "    body:JSON.stringify({ssid:ssid,pass:pass})"
        "  }).then(r=>r.text()).then(txt=>{"
        "    if (txt.trim()==='OK') {"
        "      alert('Datos enviados correctamente, El dispositivo intentara conectarse.');"
        "    } else {"
        "      alert('Respuesta inesperada: '+txt);"
        "    }"
        "  }).catch(()=>alert('Error al enviar datos'));"
        "};"
        "</script></body></html>";
    httpd_resp_set_type(r, "text/html");
    httpd_resp_send(r, page, strlen(page));
    return ESP_OK;
}

// Handler global para /save
static esp_err_t save_post(httpd_req_t *r) {
    char buf[256];
    int len = httpd_req_recv(r, buf, sizeof(buf)-1);
    if (len <= 0) return httpd_resp_send_err(r,400,"empty");
    buf[len]=0;
    cJSON *root = cJSON_Parse(buf);
    if (!root) return httpd_resp_send_err(r,400,"json");
    cJSON *js = cJSON_GetObjectItem(root,"ssid");
    cJSON *jp = cJSON_GetObjectItem(root,"pass");
    if (!cJSON_IsString(js)) { cJSON_Delete(root); return httpd_resp_send_err(r,400,"ssid?"); }
    const char *ssid = js->valuestring;
    const char *pass = (jp && cJSON_IsString(jp)) ? jp->valuestring : "";

    // Guardar directamente en NVS para flujo "guardar y reiniciar"
    wifi_store_save(ssid, pass);
    start_mdns_service(); // mDNS tras cambio de modo

    // Responder inmediatamente para buena UX en el navegador
    httpd_resp_sendstr(r, "OK");

    // Lanzar reinicio diferido
    xTaskCreate(restart_later_task, "rst_later", 2048, NULL, 5, NULL);

    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t scan_get(httpd_req_t *r) {
    // Escaneo de hasta 10 redes y respuesta en JSON
    // Guardar modo actual
    wifi_mode_t old_mode;
    esp_wifi_get_mode(&old_mode);
    if (old_mode != WIFI_MODE_APSTA) {
        esp_wifi_set_mode(WIFI_MODE_APSTA);
    }
    wifi_scan_config_t sc = { .show_hidden = false };
    esp_wifi_scan_start(&sc, true);
    uint16_t ap_num = 0;
    esp_wifi_scan_get_ap_num(&ap_num);
    ESP_LOGI("SCAN", "Redes detectadas: %d", ap_num);
    if (ap_num > 20) ap_num = 20;
    wifi_ap_record_t *list = calloc(ap_num, sizeof(wifi_ap_record_t));
    if (!list) {
        ESP_LOGE("SCAN", "OOM al reservar array");
        httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        // Restaurar modo original si fue cambiado
        if (old_mode != WIFI_MODE_APSTA) esp_wifi_set_mode(old_mode);
        return ESP_FAIL;
    }
    esp_wifi_scan_get_ap_records(&ap_num, list);
    httpd_resp_set_type(r, "application/json");
    httpd_resp_send_chunk(r, "{\"networks\":[", -1);
    for (int i = 0; i < ap_num; i++) {
        char buf[96];
        int open = (list[i].authmode == WIFI_AUTH_OPEN) ? 1 : 0;
        snprintf(buf, sizeof(buf), "{\"ssid\":\"%s\",\"open\":%d}", (char*)list[i].ssid, open);
        httpd_resp_send_chunk(r, buf, -1);
        if (i < ap_num - 1) httpd_resp_send_chunk(r, ",", -1);
    }
    httpd_resp_send_chunk(r, "]}", -1);
    httpd_resp_send_chunk(r, NULL, 0);
    free(list);
    // Restaurar modo original si fue cambiado
    if (old_mode != WIFI_MODE_APSTA) esp_wifi_set_mode(old_mode);
    return ESP_OK;
}

static esp_err_t connect_post(httpd_req_t *r) {
    char buf[256];
    int len = httpd_req_recv(r, buf, sizeof(buf)-1);
    if (len <= 0) return httpd_resp_send_err(r,400,"empty");
    buf[len]=0;
    cJSON *root = cJSON_Parse(buf);
    if (!root) return httpd_resp_send_err(r,400,"json");
    cJSON *js = cJSON_GetObjectItem(root,"ssid");
    cJSON *jp = cJSON_GetObjectItem(root,"pass");
    if (!cJSON_IsString(js)) { cJSON_Delete(root); return httpd_resp_send_err(r,400,"ssid?"); }
    const char *ssid = js->valuestring;
    const char *pass = (jp && cJSON_IsString(jp)) ? jp->valuestring : "";

    // Guardar directamente en NVS y reiniciar, igual que /save
    wifi_store_save(ssid, pass);
    start_mdns_service(); // mDNS tras cambio de modo
    httpd_resp_sendstr(r, "OK");
    xTaskCreate(restart_later_task, "rst_later", 2048, NULL, 5, NULL);

    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t status_get(httpd_req_t *r) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root,"state", captive_manager_state_str(g_state));
    cJSON_AddBoolToObject(root,"sta_ip", g_sta_have_ip);
    cJSON_AddBoolToObject(root,"using_saved", g_using_saved);
    cJSON_AddNumberToObject(root,"verify_success", g_verify_success);
    cJSON_AddNumberToObject(root,"conn_attempts", g_connect_attempts);
    char *out = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(r,"application/json");
    httpd_resp_sendstr(r,out);
    free(out);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t wifi_clear_delete(httpd_req_t *r) {
    ESP_LOGI(TAG, "wifi_clear_delete: endpoint llamado");

    // Responder primero en texto plano (mínimos recursos)
    httpd_resp_set_type(r, "text/plain");
    httpd_resp_sendstr(r, "Credenciales Wi-Fi borradas. El dispositivo se reiniciara en segundos.");

    // Borrar NVS después de responder (evita cortar la conexión antes de tiempo)
    esp_err_t err = wifi_store_clear();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi_clear: fallo al borrar NVS (%d)", (int)err);
    }

    // Reiniciar poco después (respuesta ya enviada)
    xTaskCreate(restart_later_task, "rst_later", 2048, NULL, 5, NULL);
    return ESP_OK;
}

// Alias GET para facilitar pruebas desde navegador
static esp_err_t wifi_clear_get(httpd_req_t *r) {
    return wifi_clear_delete(r);
}

// CM_UNUSED y captive_get_netif_num eliminados: no se usan

static esp_err_t save_post(httpd_req_t *r); // Prototipo
static httpd_uri_t uri_root      = { .uri="/",            .method=HTTP_GET,    .handler=root_get };
static httpd_uri_t uri_scan      = { .uri="/scan",        .method=HTTP_GET,    .handler=scan_get };
static httpd_uri_t uri_connect   = { .uri="/connect",     .method=HTTP_POST,   .handler=connect_post };
static httpd_uri_t uri_save      = { .uri="/save",        .method=HTTP_POST,   .handler=save_post };
static httpd_uri_t uri_status    = { .uri="/status",      .method=HTTP_GET,    .handler=status_get };
static httpd_uri_t uri_wifi_clr  = { .uri="/wifi/clear",  .method=HTTP_DELETE, .handler=wifi_clear_delete };
static httpd_uri_t uri_wifi_clr_get = { .uri="/wifi/clear", .method=HTTP_GET,    .handler=wifi_clear_get };

static esp_err_t start_http(void) {
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_open_sockets = 2; // Limitar a 2 conexiones simultáneas
    cfg.stack_size = 8192; // Aumentar stack para httpd y evitar overflow
    cfg.lru_purge_enable = true;
    if (httpd_start(&g_server, &cfg) == ESP_OK) {
    httpd_register_uri_handler(g_server,&uri_root);
    httpd_register_uri_handler(g_server,&uri_scan);
    httpd_register_uri_handler(g_server,&uri_connect);
    httpd_register_uri_handler(g_server,&uri_save);
    httpd_register_uri_handler(g_server,&uri_status);
    httpd_register_uri_handler(g_server,&uri_wifi_clr);
    httpd_register_uri_handler(g_server,&uri_wifi_clr_get);
        return ESP_OK;
    }
    return ESP_FAIL;
}

// Servidor mínimo en STA solo para /wifi/clear
static esp_err_t start_http_sta_minimal(void) {
    ESP_LOGI(TAG, "Intentando iniciar STA-min HTTP server...");
    if (g_sta_server) {
        ESP_LOGI(TAG, "STA-min HTTP server ya estaba iniciado");
        return ESP_OK;
    }
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_open_sockets = 1;
    cfg.stack_size = 4096;
    cfg.lru_purge_enable = true;
    if (httpd_start(&g_sta_server, &cfg) == ESP_OK) {
        httpd_register_uri_handler(g_sta_server, &uri_wifi_clr);
        httpd_register_uri_handler(g_sta_server, &uri_wifi_clr_get);
        ESP_LOGI(TAG, "STA-min HTTP server started (/wifi/clear)");
        return ESP_OK;
    }
    ESP_LOGE(TAG, "STA-min HTTP server NO se pudo iniciar");
    return ESP_FAIL;
}

static void stop_http_sta_minimal(void) {
    if (g_sta_server) {
        ESP_LOGI(TAG, "Deteniendo STA-min HTTP server...");
        httpd_stop(g_sta_server);
        g_sta_server = NULL;
        ESP_LOGI(TAG, "STA-min HTTP server stopped");
    }
}

static void shutdown_ap(void) {
    captive_manager_disable_nat();
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    if (g_server) {
        ESP_LOGI(TAG, "Deteniendo HTTP server principal (AP)...");
        httpd_stop(g_server);
        g_server = NULL;
        ESP_LOGI(TAG,"AP & HTTP server stopped");
    }
    ESP_LOGI(TAG, "Llamando a start_http_sta_minimal() tras apagar AP");
    start_http_sta_minimal();
}

// Periodic task
#if !defined(FORCE_AP_NAT_MODE)
static void periodic_check_task(void *arg) {
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(g_cfg.check_interval_ms));
        if (g_state == CAP_STATE_WAIT_LOGIN || g_state == CAP_STATE_VERIFY) {
            bool open = connectivity_portal_open();
            if (open) {
                if (g_state == CAP_STATE_WAIT_LOGIN) {
                    set_state(CAP_STATE_VERIFY);
                    g_verify_success = 0;
                }
                g_verify_success++;
                if (g_verify_success >= g_cfg.verify_success_needed) {
                    shutdown_ap();
                    set_state(CAP_STATE_OPERATIONAL);
                    // shutdown_ap() ya intenta iniciar STA-min, pero por seguridad
                    start_http_sta_minimal();
                }
            } else {
                g_verify_success = 0;
            }
        }
    }
}
#endif

// mDNS service
// Iniciar mDNS si está habilitado en sdkconfig
static void start_mdns_service(void) {

    static bool mdns_started = false;
    if (mdns_started) return;
    if (mdns_init() == ESP_OK) {
        mdns_hostname_set("LCTMedAmb");
        mdns_instance_name_set("ESP32 Ambiente");
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
        mdns_started = true;
        ESP_LOGI(TAG, "mDNS started as LCTMedAmb.local");
    } else {
        ESP_LOGW(TAG, "mDNS init failed");
    }

}

// Implementación NAT (NAPT) local
void captive_manager_enable_nat(void) {
    if (g_nat_enabled) {
        ESP_LOGI(TAG, "NAPT ya estaba habilitado");
        return;
    }
    if (!g_ap_netif) {
        ESP_LOGW(TAG, "No se puede habilitar NAPT: g_ap_netif nulo");
        return;
    }
    esp_err_t err = esp_netif_napt_enable(g_ap_netif);
    if (err == ESP_OK) {
        g_nat_enabled = true;
        ESP_LOGI(TAG, "NAPT habilitado en AP");
    } else {
        ESP_LOGE(TAG, "Fallo al habilitar NAPT: %s", esp_err_to_name(err));
    }
}

void captive_manager_disable_nat(void) {
    if (!g_nat_enabled) return;
    if (!g_ap_netif) return;
    esp_err_t err = esp_netif_napt_disable(g_ap_netif);
    if (err == ESP_OK) {
        g_nat_enabled = false;
        ESP_LOGI(TAG, "NAPT deshabilitado en AP");
    } else {
        ESP_LOGW(TAG, "Fallo al deshabilitar NAPT: %s", esp_err_to_name(err));
    }
}

// Stub de verificación de portal/conectividad. Ajusta según tu lógica real.
bool connectivity_portal_open(void) {
    // Verifica conectividad saliente intentando acceder a un endpoint conocido
    // que devuelve HTTP 204 sin contenido cuando hay internet sin portal cautivo.
    // Evitamos seguir redirecciones para detectar portales cautivos (3xx/200 con contenido).
    esp_http_client_config_t cfg = {
        .url = "http://connectivitycheck.gstatic.com/generate_204",
        .timeout_ms = 2000,
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGW(TAG, "connectivity_check: init failed");
        return false;
    }
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "connectivity_check: open err=%s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return false;
    }
    int64_t hdrs = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    // Cerrar y limpiar
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    ESP_LOGI(TAG, "connectivity_check: http status=%d (hdrs=%lld)", status, (long long)hdrs);
    // Considera 204 como OK, y 200 como parcialmente OK (algunas redes devuelven 200)
    if (status == 204 || status == 200) {
        return true;
    }
    return false;
}

// maybe_start_captive_after_saved_ip eliminado: no se usa