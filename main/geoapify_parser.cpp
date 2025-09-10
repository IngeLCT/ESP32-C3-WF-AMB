#include "json.h"
#include "sensors.h"
#include "esp_log.h"
#include <string>

static const char *TAG_GP = "GEOAPIFY_PARSE";

extern "C" void geoapify_parse_and_set_city_state(const char *json_buf, int len) {
    if (!json_buf || len <= 0) return;
    Json::Value root;
    Json::CharReaderBuilder b;
    std::string errs;
    std::unique_ptr<Json::CharReader> reader(b.newCharReader());
    if (!reader->parse(json_buf, json_buf + len, &root, &errs)) {
        ESP_LOGW(TAG_GP, "Parse error: %s", errs.c_str());
        return;
    }
    std::string city = root["city"]["name"].asString();
    if (city.empty()) city = root["city"]["names"]["en"].asString();
    std::string state = root["state"]["name"].asString();
    if (state.empty() && root["subdivisions"].isArray() && root["subdivisions"].size()>0) {
        state = root["subdivisions"][0]["names"]["en"].asString();
    }
    if (!city.empty() || !state.empty()) {
        char city_state[64];
        if (!city.empty() && !state.empty()) snprintf(city_state, sizeof(city_state), "%s-%s", city.c_str(), state.c_str());
        else if (!city.empty()) snprintf(city_state, sizeof(city_state), "%s", city.c_str());
        else snprintf(city_state, sizeof(city_state), "%s", state.c_str());
        sensors_set_city_state(city_state);
        ESP_LOGI(TAG_GP, "Parsed ciudad=%s", city_state);
    } else {
        ESP_LOGW(TAG_GP, "city/state not found");
    }
}
