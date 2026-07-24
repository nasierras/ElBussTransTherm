#include <stdio.h>
#include <stdlib.h>   // ADDED: needed for free() in the publish block
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"   // CHANGED: legacy driver/i2c.h is deprecated on IDF v5.2+;
                                 // migrated to the new handle-based i2c_master API
#include "esp_log.h"
#include "mqtt_client.h"
#include "cJSON.h"
#include "esp_eth.h"      // ADDED: W5500 Ethernet driver (same pattern as WP2.1.i)
#include "esp_netif.h"    // ADDED: network interface layer (netif/LWIP)
#include "esp_event.h"    // ADDED: default event loop (needed by netif + MQTT)
#include "esp_mac.h"      // ADDED: esp_read_mac()/ESP_MAC_ETH
#include "driver/gpio.h"  // ADDED: gpio_install_isr_service()
#include "driver/spi_master.h"

static const char *TAG = "WP2_2_i_NODE";

// ==========================================
// MQTT Configuration
// ==========================================
#define MQTT_BROKER_URI "mqtt://192.168.1.100:1883" // Raspberry pi MQTT broker IP-address
// FIXED: all three lines previously read "wp221_front" (copy-paste artifact).
// Uncomment exactly one to match this physical node's mounting position.
#define MQTT_PUBLISH_TOPIC "bus/env/wp221_front"
//#define MQTT_PUBLISH_TOPIC "bus/env/wp222_right"
//#define MQTT_PUBLISH_TOPIC "bus/env/wp223_left"

esp_mqtt_client_handle_t mqtt_client = NULL;
bool is_mqtt_connected = false;

// ==========================================
// Ethernet (W5500) Pin Definitions (SPI3 / FSPI)
// Same LilyGO T-ETH-Lite ESP32-S3 pinout used on WP2.1.i.
// ==========================================
#define ETH_SPI_HOST      SPI3_HOST
#define ETH_PIN_SCLK      10
#define ETH_PIN_MISO      11
#define ETH_PIN_MOSI      12
#define ETH_PIN_CS        9
#define ETH_PIN_INT       13
#define ETH_PIN_RST       14
#define ETH_SPI_CLOCK_MHZ 20
#define ETH_PHY_ADDR      1

static esp_eth_handle_t eth_handle = NULL;
static volatile bool is_eth_connected = false;

// ==========================================
// Hardware Pin Definitions & I2C Config
// ==========================================
#define I2C_MASTER_SCL_IO           40
#define I2C_MASTER_SDA_IO           39
#define I2C_MASTER_NUM              I2C_NUM_0
#define I2C_MASTER_FREQ_HZ          100000  // Standard 100kHz for compatibility

// I2C Device Addresses
#define ADS1115_ADDR                0x48    // ADDR pin to GND
#define SCD41_ADDR                  0x62
#define SPS30_ADDR                  0x69

// Calibration Constants (Placeholders based on datasheets)
#define VOLTAGE_DIVIDER_RATIO       1.6666f // (10k + 15k) / 15k
#define ADS1115_VOLTS_PER_BIT       0.0001875f // Using +/- 6.144V FSR

static i2c_master_bus_handle_t i2c_bus_handle;
static i2c_master_dev_handle_t ads1115_dev;
static i2c_master_dev_handle_t scd41_dev;
static i2c_master_dev_handle_t sps30_dev;

// ==========================================
// Ethernet Event Handlers (same pattern as WP2.1.i)
// ==========================================
static void eth_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    switch (event_id) {
        case ETHERNET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "Ethernet Link Up");
            break;
        case ETHERNET_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "Ethernet Link Down");
            is_eth_connected = false;
            break;
        default:
            break;
    }
}

static void got_ip_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    ESP_LOGI(TAG, "Ethernet Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
    is_eth_connected = true;
}

// ==========================================
// Ethernet Initialization (W5500 over SPI3) -- non-blocking.
// Without this, MQTT init crashes with "Invalid mbox" because no
// netif/event loop/tcpip task exists yet for it to attach to.
// Sensor reads must NOT depend on link/IP being up (see app_main loop).
// ==========================================
void init_ethernet(void) {
    ESP_LOGI(TAG, "Initializing W5500 Ethernet...");

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(gpio_install_isr_service(0));

    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *eth_netif = esp_netif_new(&netif_cfg);

    spi_bus_config_t eth_buscfg = {
        .miso_io_num = ETH_PIN_MISO,
        .mosi_io_num = ETH_PIN_MOSI,
        .sclk_io_num = ETH_PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(ETH_SPI_HOST, &eth_buscfg, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t eth_spi_devcfg = {
        .mode = 0,
        .clock_speed_hz = ETH_SPI_CLOCK_MHZ * 1000 * 1000,
        .queue_size = 20,
        .spics_io_num = ETH_PIN_CS,
    };

    eth_w5500_config_t w5500_config = ETH_W5500_DEFAULT_CONFIG(ETH_SPI_HOST, &eth_spi_devcfg);
    w5500_config.int_gpio_num = ETH_PIN_INT;

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = ETH_PHY_ADDR;
    phy_config.reset_gpio_num = ETH_PIN_RST;

    esp_eth_mac_t *mac = esp_eth_mac_new_w5500(&w5500_config, &mac_config);
    esp_eth_phy_t *phy = esp_eth_phy_new_w5500(&phy_config);

    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    ESP_ERROR_CHECK(esp_eth_driver_install(&eth_config, &eth_handle));

    uint8_t eth_mac[6];
    ESP_ERROR_CHECK(esp_read_mac(eth_mac, ESP_MAC_ETH));
    ESP_ERROR_CHECK(esp_eth_ioctl(eth_handle, ETH_CMD_S_MAC_ADDR, eth_mac));

    ESP_ERROR_CHECK(esp_netif_attach(eth_netif, esp_eth_new_netif_glue(eth_handle)));

    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &eth_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &got_ip_event_handler, NULL));
    ESP_ERROR_CHECK(esp_eth_start(eth_handle));

    ESP_LOGI(TAG, "Ethernet init complete. Link negotiation continues in background.");
}

// ==========================================
// MQTT Event Handler
// ==========================================
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    // FIXED: removed the unused 'event' variable that triggered
    // -Werror=unused-variable; event_data isn't needed for this logic.
    if (event_id == MQTT_EVENT_CONNECTED) {
        is_mqtt_connected = true;
        ESP_LOGI(TAG, "MQTT Connected to Broker!");
    } else if (event_id == MQTT_EVENT_DISCONNECTED) {
        is_mqtt_connected = false;
        ESP_LOGW(TAG, "MQTT Disconnected.");
    }
}

void init_mqtt(void) {
    esp_mqtt_client_config_t mqtt_cfg = { .broker.address.uri = MQTT_BROKER_URI };
    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(mqtt_client);
}

// ==========================================
// I2C Initialization (new i2c_master driver)
// ==========================================
void init_i2c_bus(void) {
    i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_MASTER_NUM,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &i2c_bus_handle));

    i2c_device_config_t ads_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ADS1115_ADDR,
        .scl_speed_hz = I2C_MASTER_FREQ_HZ,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(i2c_bus_handle, &ads_cfg, &ads1115_dev));

    i2c_device_config_t scd_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = SCD41_ADDR,
        .scl_speed_hz = I2C_MASTER_FREQ_HZ,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(i2c_bus_handle, &scd_cfg, &scd41_dev));

    i2c_device_config_t sps_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = SPS30_ADDR,
        .scl_speed_hz = I2C_MASTER_FREQ_HZ,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(i2c_bus_handle, &sps_cfg, &sps30_dev));
}

// ==========================================
// Sensirion CRC-8 (poly 0x31, init 0xFF) -- used by both SCD41 and SPS30
// to validate each 2-byte word before trusting it.
// ==========================================
uint8_t sensirion_crc8(const uint8_t *data, size_t len) {
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

// ==========================================
// Sensor Specific Functions
// ==========================================
void start_scd41(void) {
    uint8_t cmd[2] = {0x21, 0xb1}; // Start periodic measurement
    i2c_master_transmit(scd41_dev, cmd, 2, pdMS_TO_TICKS(1000));
}

void start_sps30(void) {
    uint8_t cmd[4] = {0x00, 0x10, 0x03, 0x00}; // Start measurement, float format
    i2c_master_transmit(sps30_dev, cmd, 4, pdMS_TO_TICKS(1000));
}

float read_ads1115_voltage(uint8_t mux_channel, uint8_t *hw_fault) {
    uint16_t config = 0x8183; // Default
    if (mux_channel == 0) config = 0xC183; // AIN0 to GND
    if (mux_channel == 1) config = 0xD183; // AIN1 to GND
    if (mux_channel == 2) config = 0xE183; // AIN2 to GND

    uint8_t write_buf[3] = {0x01, (uint8_t)(config >> 8), (uint8_t)(config & 0xFF)};
    esp_err_t err = i2c_master_transmit(ads1115_dev, write_buf, 3, pdMS_TO_TICKS(100));

    if (err != ESP_OK) {
        *hw_fault = 1;
        return 0.0f;
    }

    vTaskDelay(pdMS_TO_TICKS(10)); // Wait for conversion (8ms min)

    uint8_t read_cmd = 0x00; // Conversion register
    uint8_t read_buf[2] = {0};
    err = i2c_master_transmit_receive(ads1115_dev, &read_cmd, 1, read_buf, 2, pdMS_TO_TICKS(100));
    if (err != ESP_OK) {
        *hw_fault = 1;
        return 0.0f;
    }

    int16_t raw_adc = (read_buf[0] << 8) | read_buf[1];
    float true_sensor_voltage = (raw_adc * ADS1115_VOLTS_PER_BIT) * VOLTAGE_DIVIDER_RATIO;
    return true_sensor_voltage;
}

// Reconstructs one big-endian IEEE754 float from a 6-byte SPS30 chunk:
// [b0, b1, crc01, b2, b3, crc23]. Returns false (and flags fault) on CRC mismatch.
bool sps30_parse_float(const uint8_t *chunk, float *out_value) {
    if (sensirion_crc8(&chunk[0], 2) != chunk[2]) return false;
    if (sensirion_crc8(&chunk[3], 2) != chunk[5]) return false;

    uint8_t float_bytes[4] = {chunk[0], chunk[1], chunk[3], chunk[4]};
    uint32_t raw;
    memcpy(&raw, float_bytes, 4);
    raw = __builtin_bswap32(raw); // sensor is big-endian, ESP32 is little-endian
    memcpy(out_value, &raw, 4);
    return true;
}

// ==========================================
// Main Application Loop
// ==========================================
void app_main(void) {
    ESP_LOGI(TAG, "Starting WP2.2.i Node (I2C IAQ Sensors)...");

    init_ethernet();   // Must run first: brings up netif/event loop that MQTT needs
    init_i2c_bus();
    init_mqtt();

    // Wake up and initialize digital sensors
    start_scd41();
    start_sps30();

    TickType_t xLastWakeTime = xTaskGetTickCount();
    const TickType_t xFrequency = pdMS_TO_TICKS(1000); // 1 Hz Sync

    while (1) {
        ESP_LOGI(TAG, "--- Sensor Reading Cycle Start ---");

        uint8_t ads_fault = 0;
        uint8_t scd_fault = 0;
        uint8_t sps_fault = 0;

        // 1. Read ADS1115 Channels (Analog 5V sensors via divider)
        float v_sps215 = read_ads1115_voltage(0, &ads_fault); // AIN0
        float v_pav_x  = read_ads1115_voltage(1, &ads_fault); // AIN1
        float v_pav_y  = read_ads1115_voltage(2, &ads_fault); // AIN2

        float solar_irradiance = v_sps215 * 1000.0f; // Placeholder conversion W/m2
        float iav_x = v_pav_x * 2.0f;                // Placeholder conversion m/s
        float iav_y = v_pav_y * 2.0f;                // Placeholder conversion m/s

        // 2. Read SCD41 (CO2, Temp, RH) - Command 0xec05, with CRC check per word
        uint8_t scd_cmd[2] = {0xec, 0x05};
        uint8_t scd_data[9] = {0};
        float co2_ppm = 0.0f, scd_temp = 0.0f, scd_rh = 0.0f;

        esp_err_t scd_err = i2c_master_transmit_receive(scd41_dev, scd_cmd, 2, scd_data, 9, pdMS_TO_TICKS(100));
        if (scd_err != ESP_OK) {
            scd_fault = 1;
            ESP_LOGW(TAG, "SCD41 I2C transaction failed (err=%d)", scd_err);
        } else {
            bool crc_ok = (sensirion_crc8(&scd_data[0], 2) == scd_data[2]) &&
                          (sensirion_crc8(&scd_data[3], 2) == scd_data[5]) &&
                          (sensirion_crc8(&scd_data[6], 2) == scd_data[8]);
            if (!crc_ok) {
                scd_fault = 1;
                ESP_LOGW(TAG, "SCD41 CRC check failed -- discarding reading");
            } else {
                co2_ppm  = (float)((scd_data[0] << 8) | scd_data[1]);
                scd_temp = -45.0f + 175.0f * (float)((scd_data[3] << 8) | scd_data[4]) / 65535.0f;
                scd_rh   = 100.0f * (float)((scd_data[6] << 8) | scd_data[7]) / 65535.0f;
            }
        }

        // 3. Read SPS30 (PM) - Command 0x0300, float format.
        // First 4 values (PM1.0, PM2.5, PM4.0, PM10) = 4 * 6 bytes = 24 bytes.
        uint8_t sps_cmd[2] = {0x03, 0x00};
        uint8_t sps_data[24] = {0};
        float pm1_0 = 0.0f, pm2_5 = 0.0f, pm4_0 = 0.0f, pm10 = 0.0f;

        esp_err_t sps_err = i2c_master_transmit_receive(sps30_dev, sps_cmd, 2, sps_data, 24, pdMS_TO_TICKS(100));
        if (sps_err != ESP_OK) {
            sps_fault = 1;
            ESP_LOGW(TAG, "SPS30 I2C transaction failed (err=%d)", sps_err);
        } else {
            bool ok = true;
            ok &= sps30_parse_float(&sps_data[0],  &pm1_0);
            ok &= sps30_parse_float(&sps_data[6],  &pm2_5);
            ok &= sps30_parse_float(&sps_data[12], &pm4_0);
            ok &= sps30_parse_float(&sps_data[18], &pm10);
            if (!ok) {
                sps_fault = 1;
                ESP_LOGW(TAG, "SPS30 CRC check failed on one or more values -- discarding reading");
                pm1_0 = pm2_5 = pm4_0 = pm10 = 0.0f;
            }
        }

        // 4. Log faults locally regardless of MQTT state, same pattern as WP2.1.i
        if (ads_fault) ESP_LOGW(TAG, "ADS1115 I2C fault/timeout");
        if (scd_fault) ESP_LOGW(TAG, "SCD41 fault (see above)");
        if (sps_fault) ESP_LOGW(TAG, "SPS30 fault (see above)");

        // 5. Generate JSON Payload
        if (is_mqtt_connected) {
            cJSON *root = cJSON_CreateObject();

            cJSON_AddNumberToObject(root, "a1_iav_x_m_per_s", iav_x);
            cJSON_AddNumberToObject(root, "a1_iav_y_m_per_s", iav_y);
            cJSON_AddNumberToObject(root, "a2_co2_ppm", co2_ppm);
            cJSON_AddNumberToObject(root, "a2_temp_C", scd_temp);
            cJSON_AddNumberToObject(root, "a2_RH_percent", scd_rh);
            cJSON_AddNumberToObject(root, "b2_pm001_mum", pm1_0);
            cJSON_AddNumberToObject(root, "b2_pm025_mum", pm2_5);
            cJSON_AddNumberToObject(root, "b2_pm040_mum", pm4_0);
            cJSON_AddNumberToObject(root, "b2_pm100_mum", pm10);
            cJSON_AddNumberToObject(root, "a3_solar_irradiance_w_per_m2", solar_irradiance);

            // Diagnostics
            cJSON *diag = cJSON_AddObjectToObject(root, "diagnostics");
            cJSON_AddBoolToObject(diag, "mqtt_connected", true);
            cJSON *faults = cJSON_AddArrayToObject(diag, "sensor_faults");
            if (ads_fault) cJSON_AddItemToArray(faults, cJSON_CreateString("ADS1115_I2C_FAULT"));
            if (scd_fault) cJSON_AddItemToArray(faults, cJSON_CreateString("SCD41_FAULT"));
            if (sps_fault) cJSON_AddItemToArray(faults, cJSON_CreateString("SPS30_FAULT"));

            char *json_str = cJSON_PrintUnformatted(root);
            esp_mqtt_client_publish(mqtt_client, MQTT_PUBLISH_TOPIC, json_str, 0, 1, 0);

            cJSON_Delete(root);
            free(json_str);
        } else {
            ESP_LOGW(TAG, "MQTT not connected. Sensors still read locally; publish skipped.");
        }

        // Wait exact remaining time to hit 1000ms
        vTaskDelayUntil(&xLastWakeTime, xFrequency);
    }
}
