#include "display.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "DISPLAY";

esp_err_t display_init(void) {
    ESP_LOGI(TAG, "Display disabled in Lite build.");
    return ESP_OK;
}

void display_clear(void) {}

void display_print_line(int line, const char *text) {}

void display_update_status(const char *ip, const char *sip_status, const char *caller) {
    ESP_LOGI(TAG, "IP: %s | SIP: %s", ip ? ip : "none", sip_status ? sip_status : "none");
}