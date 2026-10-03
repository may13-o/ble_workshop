#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs_flash.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#ifdef CONFIG_WORKSHOP_LED_ACTIVE_LOW
#define LED_OFF_LEVEL 1
#else
#define LED_OFF_LEVEL 0
#endif

static const char *TAG = "workshop";
static i2c_master_bus_handle_t sensor_bus;
static i2c_master_dev_handle_t sensor_dev;
static atomic_uint_fast16_t lux_value = ATOMIC_VAR_INIT(UINT16_MAX);
static atomic_bool ble_ready = ATOMIC_VAR_INIT(false);
static uint8_t led_value;
static uint8_t own_addr_type;
static uint16_t lux_handle;

/* On the wire these are e1630000/0001/0002-7cbe-4c7c-9eb3-74769682d091. */
static const ble_uuid128_t service_uuid = BLE_UUID128_INIT(
    0x91, 0xd0, 0x82, 0x96, 0x76, 0x74, 0xb3, 0x9e,
    0x7c, 0x4c, 0xbe, 0x7c, 0x00, 0x00, 0x63, 0xe1);
static const ble_uuid128_t led_uuid = BLE_UUID128_INIT(
    0x91, 0xd0, 0x82, 0x96, 0x76, 0x74, 0xb3, 0x9e,
    0x7c, 0x4c, 0xbe, 0x7c, 0x01, 0x00, 0x63, 0xe1);
static const ble_uuid128_t lux_uuid = BLE_UUID128_INIT(
    0x91, 0xd0, 0x82, 0x96, 0x76, 0x74, 0xb3, 0x9e,
    0x7c, 0x4c, 0xbe, 0x7c, 0x02, 0x00, 0x63, 0xe1);

static int gatt_access(uint16_t conn_handle, uint16_t attr_handle,
                       struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;

    if (ble_uuid_cmp(ctxt->chr->uuid, &led_uuid.u) == 0) {
        if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
            return os_mbuf_append(ctxt->om, &led_value, 1) == 0
                       ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
            uint8_t command;
            if (OS_MBUF_PKTLEN(ctxt->om) != 1) {
                return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            }
            if (ble_hs_mbuf_to_flat(ctxt->om, &command, 1, NULL) != 0) {
                return BLE_ATT_ERR_UNLIKELY;
            }
            if (command > 1) {
                return BLE_ATT_ERR_VALUE_NOT_ALLOWED;
            }
            led_value = command;
            gpio_set_level(CONFIG_WORKSHOP_LED_GPIO,
                           led_value ^ LED_OFF_LEVEL);
            ESP_LOGI(TAG, "LED %s", led_value ? "on" : "off");
            return 0;
        }
    } else if (ble_uuid_cmp(ctxt->chr->uuid, &lux_uuid.u) == 0 &&
               ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        uint16_t reading = (uint16_t)atomic_load(&lux_value);
        uint8_t bytes[] = {(uint8_t)reading, (uint8_t)(reading >> 8)};
        return os_mbuf_append(ctxt->om, bytes, sizeof(bytes)) == 0
                   ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_svc_def services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &service_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &led_uuid.u,
                .access_cb = gatt_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
            },
            {
                .uuid = &lux_uuid.u,
                .access_cb = gatt_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &lux_handle,
            },
            {0},
        },
    },
    {0},
};

static int gap_event(struct ble_gap_event *event, void *arg);

static void start_advertising(void)
{
    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    const char *name = ble_svc_gap_device_name();
    fields.name = (uint8_t *)name;
    fields.name_len = strlen(name);
    fields.name_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "Advertising fields failed: %d", rc);
        return;
    }

    struct ble_gap_adv_params params = {0};
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER,
                           &params, gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "Advertising start failed: %d", rc);
    }
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status != 0) {
            ESP_LOGW(TAG, "Connection failed: %d", event->connect.status);
            start_advertising();
        } else {
            ESP_LOGI(TAG, "Phone connected");
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "Phone disconnected; advertising again");
        start_advertising();
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        start_advertising();
        break;
    default:
        break;
    }
    return 0;
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc == 0) {
        rc = ble_hs_id_infer_auto(0, &own_addr_type);
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "BLE address setup failed: %d", rc);
        return;
    }
    atomic_store(&ble_ready, true);
    start_advertising();
}

static void on_reset(int reason)
{
    atomic_store(&ble_ready, false);
    ESP_LOGW(TAG, "BLE host reset: %d", reason);
}

static void host_task(void *arg)
{
    (void)arg;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void sensor_task(void *arg)
{
    (void)arg;
    bool configured = false;
    while (true) {
        uint16_t reading = UINT16_MAX;
        if (!configured) {
            const uint8_t mode = 0x10; /* BH1750 continuous high-resolution mode. */
            if (i2c_master_probe(sensor_bus, CONFIG_WORKSHOP_BH1750_ADDRESS, 50) == ESP_OK &&
                i2c_master_transmit(sensor_dev, &mode, 1, 100) == ESP_OK) {
                configured = true;
                vTaskDelay(pdMS_TO_TICKS(180));
            }
        }
        if (configured) {
            uint8_t sample[2];
            if (i2c_master_receive(sensor_dev, sample, sizeof(sample), 100) == ESP_OK) {
                uint32_t raw = ((uint16_t)sample[0] << 8) | sample[1];
                uint32_t lux = raw * 5 / 6; /* Datasheet: raw / 1.2 lux. */
                reading = (uint16_t)(lux >= UINT16_MAX ? UINT16_MAX - 1 : lux);
                ESP_LOGI(TAG, "BH1750: %u lux", reading);
            } else {
                configured = false;
            }
        }
        if (reading == UINT16_MAX) {
            ESP_LOGW(TAG, "BH1750 unavailable at 0x%02x", CONFIG_WORKSHOP_BH1750_ADDRESS);
        }
        atomic_store(&lux_value, reading);
        if (atomic_load(&ble_ready)) {
            ble_gatts_chr_updated(lux_handle);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void app_main(void)
{
    if (CONFIG_WORKSHOP_I2C_SDA == CONFIG_WORKSHOP_I2C_SCL ||
        CONFIG_WORKSHOP_LED_GPIO == CONFIG_WORKSHOP_I2C_SDA ||
        CONFIG_WORKSHOP_LED_GPIO == CONFIG_WORKSHOP_I2C_SCL) {
        ESP_LOGE(TAG, "LED, SDA and SCL must use different pins");
        return;
    }

    gpio_config_t led_config = {
        .pin_bit_mask = 1ULL << CONFIG_WORKSHOP_LED_GPIO,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&led_config));
    ESP_ERROR_CHECK(gpio_set_level(CONFIG_WORKSHOP_LED_GPIO,
                                   LED_OFF_LEVEL));

    i2c_master_bus_config_t bus_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_NUM_0,
        .sda_io_num = CONFIG_WORKSHOP_I2C_SDA,
        .scl_io_num = CONFIG_WORKSHOP_I2C_SCL,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &sensor_bus));
    for (int address = 0x08; address <= 0x77; ++address) {
        if (i2c_master_probe(sensor_bus, address, 20) == ESP_OK) {
            ESP_LOGI(TAG, "I2C device at 0x%02x", address);
        }
    }
    i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = CONFIG_WORKSHOP_BH1750_ADDRESS,
        .scl_speed_hz = 100000,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(sensor_bus, &device_config, &sensor_dev));

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ESP_ERROR_CHECK(ble_gatts_count_cfg(services));
    ESP_ERROR_CHECK(ble_gatts_add_svcs(services));

    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_BT));
    char name[16];
    snprintf(name, sizeof(name), "WS-%02X%02X%02X", mac[3], mac[4], mac[5]);
    ESP_ERROR_CHECK(ble_svc_gap_device_name_set(name));
    ESP_LOGI(TAG, "Find %s in nRF Connect", name);
    nimble_port_freertos_init(host_task);
    BaseType_t created = xTaskCreate(sensor_task, "sensor", 3072, NULL, 5, NULL);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "Could not start sensor task");
    }
}
