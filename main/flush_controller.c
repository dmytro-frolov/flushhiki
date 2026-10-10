#include <stdbool.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_zigbee_core.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ha/esp_zigbee_ha_standard.h"
#include "nvs_flash.h"

#define FLUSH_GPIO GPIO_NUM_1
#define USER_LED_GPIO GPIO_NUM_15
#define FLUSH_PULSE_MS 500
#define FLUSH_ENDPOINT 10

#define MANUFACTURER_NAME "\x09" "Flushhiki"
#define MODEL_IDENTIFIER "\x10" "CatToiletFlushV1"
#define PRIMARY_CHANNEL_MASK ESP_ZB_TRANSCEIVER_ALL_CHANNELS_MASK

#define ZB_ROUTER_CONFIG()                                      \
    {                                                           \
        .esp_zb_role = ESP_ZB_DEVICE_TYPE_ROUTER,                \
        .install_code_policy = false,                            \
        .nwk_cfg.zczr_cfg = { .max_children = 10 },              \
    }

static const char *TAG = "flush_controller";
static TaskHandle_t pulse_task_handle;

static void set_output(bool active)
{
    gpio_set_level(FLUSH_GPIO, active);
    gpio_set_level(USER_LED_GPIO, !active); /* XIAO user LED is active-low. */
}

static void report_idle(void)
{
    bool off = false;
    esp_zb_zcl_report_attr_cmd_t report = {
        .address_mode = ESP_ZB_APS_ADDR_MODE_DST_ADDR_ENDP_NOT_PRESENT,
        .attributeID = ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID,
        .direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_CLI,
        .clusterID = ESP_ZB_ZCL_CLUSTER_ID_ON_OFF,
        .zcl_basic_cmd.src_endpoint = FLUSH_ENDPOINT,
    };

    esp_zb_lock_acquire(portMAX_DELAY);
    esp_zb_zcl_status_t status = esp_zb_zcl_set_attribute_val(
        FLUSH_ENDPOINT, ESP_ZB_ZCL_CLUSTER_ID_ON_OFF, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
        ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID, &off, false);
    esp_err_t report_err = esp_zb_zcl_report_attr_cmd_req(&report);
    esp_zb_lock_release();

    if (status != ESP_ZB_ZCL_STATUS_SUCCESS || report_err != ESP_OK) {
        ESP_LOGW(TAG, "Could not report idle state (attribute 0x%x, report %s)",
                 status, esp_err_to_name(report_err));
    }
}

static void pulse_task(void *arg)
{
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        ESP_LOGI(TAG, "Flush pulse started");
        set_output(true);
        vTaskDelay(pdMS_TO_TICKS(FLUSH_PULSE_MS));
        set_output(false);
        ulTaskNotifyTake(pdTRUE, 0); /* Ignore duplicate commands while a pulse is active. */
        report_idle();
        ESP_LOGI(TAG, "Flush pulse finished");
    }
}

static esp_err_t attribute_handler(const esp_zb_zcl_set_attr_value_message_t *message)
{
    ESP_RETURN_ON_FALSE(message, ESP_ERR_INVALID_ARG, TAG, "Empty Zigbee message");
    ESP_RETURN_ON_FALSE(message->info.status == ESP_ZB_ZCL_STATUS_SUCCESS, ESP_FAIL, TAG,
                        "Zigbee command failed with status %d", message->info.status);

    if (message->info.dst_endpoint != FLUSH_ENDPOINT ||
        message->info.cluster != ESP_ZB_ZCL_CLUSTER_ID_ON_OFF ||
        message->attribute.id != ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID ||
        message->attribute.data.type != ESP_ZB_ZCL_ATTR_TYPE_BOOL ||
        message->attribute.data.value == NULL) {
        return ESP_OK;
    }

    if (*(bool *)message->attribute.data.value) {
        xTaskNotifyGive(pulse_task_handle);
    }
    return ESP_OK;
}

static esp_err_t action_handler(esp_zb_core_action_callback_id_t callback_id, const void *message)
{
    if (callback_id == ESP_ZB_CORE_SET_ATTR_VALUE_CB_ID) {
        return attribute_handler(message);
    }
    return ESP_OK;
}

static void start_commissioning(uint8_t mode_mask)
{
    esp_err_t err = esp_zb_bdb_start_top_level_commissioning(mode_mask);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Could not start Zigbee commissioning: %s", esp_err_to_name(err));
    }
}

void esp_zb_app_signal_handler(esp_zb_app_signal_t *signal)
{
    esp_zb_app_signal_type_t type = *signal->p_app_signal;

    switch (type) {
    case ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP:
        start_commissioning(ESP_ZB_BDB_MODE_INITIALIZATION);
        break;
    case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT:
        if (signal->esp_err_status == ESP_OK) {
            ESP_LOGI(TAG, "Zigbee stack started%s",
                     esp_zb_bdb_is_factory_new() ? "; looking for a network" : "");
            if (esp_zb_bdb_is_factory_new()) {
                start_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);
            }
        } else {
            ESP_LOGW(TAG, "Zigbee startup failed: %s", esp_err_to_name(signal->esp_err_status));
            esp_zb_scheduler_alarm(start_commissioning, ESP_ZB_BDB_MODE_INITIALIZATION, 1000);
        }
        break;
    case ESP_ZB_BDB_SIGNAL_STEERING:
        if (signal->esp_err_status == ESP_OK) {
            ESP_LOGI(TAG, "Joined Zigbee network on channel %d, short address 0x%04x",
                     esp_zb_get_current_channel(), esp_zb_get_short_address());
        } else {
            ESP_LOGW(TAG, "Network join failed; retrying");
            esp_zb_scheduler_alarm(start_commissioning, ESP_ZB_BDB_MODE_NETWORK_STEERING, 1000);
        }
        break;
    default:
        ESP_LOGD(TAG, "Zigbee signal %s, status %s", esp_zb_zdo_signal_to_string(type),
                 esp_err_to_name(signal->esp_err_status));
        break;
    }
}

static void add_device_identity(esp_zb_ep_list_t *endpoints)
{
    esp_zb_cluster_list_t *clusters = esp_zb_ep_list_get_ep(endpoints, FLUSH_ENDPOINT);
    esp_zb_attribute_list_t *basic = esp_zb_cluster_list_get_cluster(
        clusters, ESP_ZB_ZCL_CLUSTER_ID_BASIC, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    ESP_ERROR_CHECK(esp_zb_basic_cluster_add_attr(
        basic, ESP_ZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID, MANUFACTURER_NAME));
    ESP_ERROR_CHECK(esp_zb_basic_cluster_add_attr(
        basic, ESP_ZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID, MODEL_IDENTIFIER));
}

static void zigbee_task(void *arg)
{
    esp_zb_cfg_t config = ZB_ROUTER_CONFIG();
    esp_zb_init(&config);

    esp_zb_on_off_light_cfg_t light_config = ESP_ZB_DEFAULT_ON_OFF_LIGHT_CONFIG();
    esp_zb_ep_list_t *endpoints = esp_zb_on_off_light_ep_create(FLUSH_ENDPOINT, &light_config);
    add_device_identity(endpoints);

    ESP_ERROR_CHECK(esp_zb_device_register(endpoints));
    esp_zb_core_action_handler_register(action_handler);
    esp_zb_set_primary_network_channel_set(PRIMARY_CHANNEL_MASK);
    ESP_ERROR_CHECK(esp_zb_start(false));
    esp_zb_stack_main_loop();
}

void app_main(void)
{
    gpio_config_t output_config = {
        .pin_bit_mask = (1ULL << FLUSH_GPIO) | (1ULL << USER_LED_GPIO),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&output_config));
    set_output(false);

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    esp_zb_platform_config_t platform = {
        .radio_config = { .radio_mode = ZB_RADIO_MODE_NATIVE },
        .host_config = { .host_connection_mode = ZB_HOST_CONNECTION_MODE_NONE },
    };
    ESP_ERROR_CHECK(esp_zb_platform_config(&platform));

    ESP_ERROR_CHECK(xTaskCreate(pulse_task, "flush_pulse", 3072, NULL, 5, &pulse_task_handle) == pdPASS
                        ? ESP_OK
                        : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(xTaskCreate(zigbee_task, "zigbee", 4096, NULL, 5, NULL) == pdPASS
                        ? ESP_OK
                        : ESP_ERR_NO_MEM);
}
