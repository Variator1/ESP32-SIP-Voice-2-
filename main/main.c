#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_sntp.h"
#include "nvs_flash.h"
#include <time.h>
#include <stdlib.h>

#include "wifi_manager.h"
#include "sip_client.h"
#include "audio_pipeline.h"
#include "app_config.h"
#include "ui_controller.h"
#include "config_manager.h"
#include "display.h"
#include "phonebook.h"
#include "keypad.h"
#include "display_tft.h"
#include "touch_driver.h"
#include "ui_lvgl.h"

typedef enum {
    APP_EVENT_BUTTON_PRESSED,
} app_event_t;

static const char *TAG = "MAIN";

EventGroupHandle_t app_event_group;
QueueHandle_t app_event_queue = NULL; // Очередь для безопасных событий UI

sip_client_handle_t g_sip_client = NULL;
audio_pipeline_handle_t g_audio_pipeline = NULL;
app_settings_t g_app_settings;

typedef enum {
    APP_STATE_INIT,
    APP_STATE_WIFI_CONNECTING,
    APP_STATE_SIP_REGISTERING,
    APP_STATE_IDLE,
    APP_STATE_IN_CALL,
    APP_STATE_ERROR
} app_state_t;

volatile app_state_t current_app_state = APP_STATE_INIT;

// Фоновая задача с безопасным контекстом и стеком для сетевых запросов
void app_control_task(void *pvParameters) {
    ESP_LOGI(TAG, "Application control task started.");
    current_app_state = APP_STATE_WIFI_CONNECTING;
    app_event_t event;

    while(1) {
        // Проверяем очередь событий (например, нажатие кнопки)
        if (app_event_queue && xQueueReceive(app_event_queue, &event, 0) == pdTRUE) {
            if (event == APP_EVENT_BUTTON_PRESSED) {
                ESP_LOGI(TAG, "Processing button press -> initiating SIP call...");
                if (g_sip_client) {
                    sip_client_initiate_call(g_sip_client, SIP_TARGET_URI);
                }
            }
        }

        EventBits_t bits = xEventGroupWaitBits(app_event_group,
                                   WIFI_CONNECTED_BIT | SIP_REGISTERED_BIT,
                                   pdFALSE,
                                   pdFALSE,
                                   pdMS_TO_TICKS(100));

        if ((bits & WIFI_CONNECTED_BIT) && current_app_state < APP_STATE_SIP_REGISTERING) {
             ESP_LOGI(TAG, "Wi-Fi Connected. Starting SIP Registration.");
             current_app_state = APP_STATE_SIP_REGISTERING;
             if (g_sip_client) sip_client_start_registration(g_sip_client);
        } else if (!(bits & WIFI_CONNECTED_BIT) && current_app_state >= APP_STATE_SIP_REGISTERING) {
             ESP_LOGW(TAG, "Wi-Fi Lost. Downgrading state.");
             current_app_state = APP_STATE_WIFI_CONNECTING;
             xEventGroupClearBits(app_event_group, SIP_REGISTERED_BIT);
        }

        if ((bits & SIP_REGISTERED_BIT) && current_app_state < APP_STATE_IDLE) {
            ESP_LOGI(TAG, "SIP Registered. Application Idle.");
            current_app_state = APP_STATE_IDLE;
        }
        
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

static void ui_action_answer(void) { if (g_sip_client) sip_client_answer_call(g_sip_client); }
static void ui_action_hangup(void) { if (g_sip_client) sip_client_terminate_call(g_sip_client); }

static void on_incoming_call(const char *caller_uri, const char *call_id) {
    (void)call_id;
    ui_lvgl_switch_screen(SCREEN_INCOMING, caller_uri);
}
static void on_call_answered(const char *call_id) { (void)call_id; ui_lvgl_switch_screen(SCREEN_ACTIVE, NULL); }
static void on_call_ended(const char *call_id)    { (void)call_id; ui_lvgl_switch_screen(SCREEN_IDLE, NULL); }
static void on_registration_status(bool registered) { ui_lvgl_set_registered(registered); }

static void init_sntp(void) {
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, NTP_SERVER);
    esp_sntp_init();
    setenv("TZ", TIMEZONE, 1);
    tzset();
    ESP_LOGI(TAG, "SNTP started (%s, TZ=%s)", NTP_SERVER, TIMEZONE);
}

void app_main(void) {
    ESP_LOGI(TAG, "Starting ESP32 SIP Client Application");

    config_manager_init();
    config_manager_load(&g_app_settings);
    
    phonebook_init();
    
    keypad_init();
    display_init();
    display_update_status("Starting...", "Init", "");

    app_event_group = xEventGroupCreate();
    if (app_event_group == NULL) {
        ESP_LOGE(TAG, "Failed to create event group");
        return;
    }

    // Создаем очередь для безопасных событий с кнопками (емкость 5 сообщений)
    app_event_queue = xQueueCreate(5, sizeof(app_event_t));
    if (app_event_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create app event queue!");
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_sta(app_event_group, WIFI_CONNECTED_BIT, IP_ACQUIRED_BIT, &g_app_settings); 

    if (wifi_is_ap_mode()) {
        ESP_LOGI(TAG, "Running in AP Mode (Captive Portal). SIP disabled.");
        ui_controller_init(NULL, &g_app_settings);
        return;
    }

    g_audio_pipeline = audio_pipeline_init();
    if (!g_audio_pipeline) {
        ESP_LOGE(TAG, "Failed to initialize audio pipeline");
    }

    g_sip_client = sip_client_init(app_event_group, SIP_REGISTERED_BIT, g_audio_pipeline, &g_app_settings);
     if (!g_sip_client) {
        ESP_LOGE(TAG, "Failed to initialize SIP client");
    } else {
         if (g_audio_pipeline) {
            audio_pipeline_set_sip_handle(g_audio_pipeline, g_sip_client);
         }
    }

    if (g_sip_client) {
        ui_controller_init(g_sip_client, &g_app_settings);

        sip_callbacks_t cbs = {
            .on_incoming_call       = on_incoming_call,
            .on_call_answered       = on_call_answered,
            .on_call_ended          = on_call_ended,
            .on_registration_status = on_registration_status,
        };
        sip_client_register_callbacks(g_sip_client, &cbs);
        ui_lvgl_set_action_cb(ui_action_answer, ui_action_hangup);
    }

    if (g_audio_pipeline) {
        audio_pipeline_set_wake_word_cb(g_audio_pipeline, ui_controller_wake_word_trigger);
    }

    init_sntp();

    xTaskCreate(app_control_task, "app_ctrl", 4096, NULL, 6, NULL);

    ESP_LOGI(TAG, "Initialization Complete. Waiting for Wi-Fi connection...");
}