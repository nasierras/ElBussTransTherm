#include <stdio.h>
#include <stdlib.h>   // ADDED: needed for free() in the publish block
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/i2c_master.h"   // CHANGED: legacy driver/i2c.h is deprecated on IDF v5.2+
#include "driver/gpio.h"
#include "esp_log.h"
#include "mqtt_client.h"
#include "cJSON.h"
#include "esp_eth.h"      // ADDED: W5500 Ethernet driver (same pattern as other WP2.x nodes)
#include "esp_netif.h"    // ADDED: network interface layer (netif/LWIP)
#include "esp_event.h"    // ADDED: default event loop (needed by netif + MQTT)
#include "esp_mac.h"      // ADDED: esp_read_mac()/ESP_MAC_ETH

static const char *TAG = "WP2_6_i_NODE";

// ==========================================
// Out-of-bounds bounds for Diagnostics (ADDED, matches WP2.1.i pattern --
// this node uses the same MAX31856/MAX31865 chips but previously had no
// out-of-bounds or hardware-fault-register checks at all)
// ==========================================
#define TEMP_MIN_BOUND -20.0f
#define TEMP_MAX_BOUND  60.0f

// ==========================================
// MQTT Configuration
// ==========================================
#define MQTT_BROKER_URI "mqtt://192.168.1.100:1883"
#define MQTT_PUBLISH_TOPIC "bus/env/thermal_dummy"

esp_mqtt_client_handle_t mqtt_client = NULL;
bool is_mqtt_connected = false;

// ==========================================
// Ethernet (W5500) Pin Definitions (SPI3 / FSPI)
// Same LilyGO T-ETH-Lite ESP32-S3 pinout used on the other WP2.x nodes.
// Kept on a SEPARATE SPI host from the sensor SPI2 bus below.
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
// Hardware Pin Definitions
// ==========================================
// SPI Pins (sensor bus, SPI2)
#define PIN_NUM_MISO 6
#define PIN_NUM_MOSI 5
#define PIN_NUM_CLK  15
#define PIN_NUM_CS1  17  // CS for MAX31856 (T-Type Thermocouples)
#define PIN_NUM_CS2  7   // CS for MAX31865 (RTD)

// I2C Pins
#define I2C_MASTER_SCL_IO  40
#define I2C_MASTER_SDA_IO  39
#define I2C_MASTER_NUM     I2C_NUM_0
#define I2C_MASTER_FREQ_HZ 100000

// ==========================================
// Constants & Addresses
// ==========================================
#define PCF8574A_ADDR       0x38    // A0-A2 to GND
#define ADS1115_ADDR_GND    0x48    // ADDR to GND
#define ADS1115_ADDR_VDD    0x49    // ADDR to 3.3V
#define ADS1115_ADDR_SDA    0x4A    // ADDR to SDA

#define ADS1115_VOLTS_PER_BIT  0.0001875f // +/- 6.144V FSR
#define VOLTAGE_DIVIDER_RATIO  1.6666f    // Compensates for 10k/15k divider

#define TC_TYPE_T_CR1_VAL      0x07   // T-Type configuration byte
#define RTD_NOMINAL_RESISTANCE 100.0f
#define RTD_REFERENCE_RESISTOR 430.0f

spi_device_handle_t spi_max31856;
spi_device_handle_t spi_max31865;

static i2c_master_bus_handle_t i2c_bus_handle;
static i2c_master_dev_handle_t pcf8574a_dev;
static i2c_master_dev_handle_t ads_gnd_dev;
static i2c_master_dev_handle_t ads_vdd_dev;
static i2c_master_dev_handle_t ads_sda_dev;

// ==========================================
// Ethernet Event Handlers (same pattern as other WP2.x nodes)
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
// This was previously just a comment ("Initialize Ethernet/WiFi here if not
// handled..."), never actually done -- which would crash MQTT with
// "Invalid mbox" the same way every other node in this project did before
// this was added. Sensor reads never wait on link/IP being up.
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
    // FIXED: removed the unused 'event' variable (-Werror=unused-variable)
    if (event_id == MQTT_EVENT_CONNECTED) {
        ESP_LOGI(TAG, "MQTT Connected");
        is_mqtt_connected = true;
    } else if (event_id == MQTT_EVENT_DISCONNECTED) {
        ESP_LOGW(TAG, "MQTT Disconnected");
        is_mqtt_connected = false;
    }
}

void init_mqtt(void) {
    esp_mqtt_client_config_t mqtt_cfg = { .broker.address.uri = MQTT_BROKER_URI };
    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(mqtt_client);
}

// ==========================================
// Hardware Initialization
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

    i2c_device_config_t pcf_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = PCF8574A_ADDR,
        .scl_speed_hz = I2C_MASTER_FREQ_HZ,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(i2c_bus_handle, &pcf_cfg, &pcf8574a_dev));

    i2c_device_config_t ads_gnd_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ADS1115_ADDR_GND,
        .scl_speed_hz = I2C_MASTER_FREQ_HZ,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(i2c_bus_handle, &ads_gnd_cfg, &ads_gnd_dev));

    i2c_device_config_t ads_vdd_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ADS1115_ADDR_VDD,
        .scl_speed_hz = I2C_MASTER_FREQ_HZ,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(i2c_bus_handle, &ads_vdd_cfg, &ads_vdd_dev));

    i2c_device_config_t ads_sda_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ADS1115_ADDR_SDA,
        .scl_speed_hz = I2C_MASTER_FREQ_HZ,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(i2c_bus_handle, &ads_sda_cfg, &ads_sda_dev));
}

void init_spi_bus(void) {
    spi_bus_config_t buscfg = {
        .miso_io_num = PIN_NUM_MISO,
        .mosi_io_num = PIN_NUM_MOSI,
        .sclk_io_num = PIN_NUM_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 64
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t devcfg_tc = {
        .clock_speed_hz = 1 * 1000 * 1000, .mode = 1, .spics_io_num = PIN_NUM_CS1, .queue_size = 7
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &devcfg_tc, &spi_max31856));

    spi_device_interface_config_t devcfg_rtd = {
        .clock_speed_hz = 1 * 1000 * 1000, .mode = 1, .spics_io_num = PIN_NUM_CS2, .queue_size = 7
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &devcfg_rtd, &spi_max31865));
}

// ==========================================
// SPI Transaction Helpers
// ==========================================
esp_err_t spi_read_registers(spi_device_handle_t spi, uint8_t reg_addr, uint8_t *data_out, size_t len) {
    uint8_t tx_buffer[len + 1];
    uint8_t rx_buffer[len + 1];
    memset(tx_buffer, 0, sizeof(tx_buffer));
    memset(rx_buffer, 0, sizeof(rx_buffer));
    tx_buffer[0] = reg_addr & 0x7F; // MSB = 0 for Read

    spi_transaction_t t = {
        .length = (len + 1) * 8,
        .tx_buffer = tx_buffer,
        .rx_buffer = rx_buffer
    };
    esp_err_t ret = spi_device_polling_transmit(spi, &t);
    if (ret == ESP_OK) memcpy(data_out, &rx_buffer[1], len);
    return ret;
}

esp_err_t spi_write_register(spi_device_handle_t spi, uint8_t reg_addr, uint8_t data_in) {
    uint8_t tx_buffer[2] = {reg_addr | 0x80, data_in}; // MSB = 1 for Write
    spi_transaction_t t = { .length = 16, .tx_buffer = tx_buffer };
    return spi_device_polling_transmit(spi, &t);
}

// ADDED: hardware fault register checks -- this node uses the identical
// MAX31856/MAX31865 chips as WP2.1.i but was previously missing these
// entirely (only I2C faults were tracked).
uint8_t check_thermocouple_hardware_fault(void) {
    uint8_t fault_reg = 0;
    spi_read_registers(spi_max31856, 0x0F, &fault_reg, 1);
    return fault_reg;
}

uint8_t check_rtd_hardware_fault(void) {
    uint8_t fault_reg = 0;
    spi_read_registers(spi_max31865, 0x07, &fault_reg, 1);
    return fault_reg;
}

bool is_temp_out_of_bounds(float temp) {
    return (temp < TEMP_MIN_BOUND || temp > TEMP_MAX_BOUND || isnan(temp));
}

// ==========================================
// Sensor Data Extraction Functions
// ==========================================
void set_multiplexer_channel(uint8_t channel) {
    // Select channel (P0-P3) and enable ADG726 (P4 = High)
    uint8_t pcf_data = (channel & 0x0F) | 0x10;
    i2c_master_transmit(pcf8574a_dev, &pcf_data, 1, pdMS_TO_TICKS(10));
    vTaskDelay(pdMS_TO_TICKS(5)); // Allow analog signal to settle
}

float read_ads1115_voltage(i2c_master_dev_handle_t dev, uint8_t mux_channel, uint8_t *hw_fault) {
    uint16_t config = 0x8183;
    if (mux_channel == 0) config = 0xC183; // AIN0 to GND
    if (mux_channel == 1) config = 0xD183; // AIN1 to GND
    if (mux_channel == 2) config = 0xE183; // AIN2 to GND
    if (mux_channel == 3) config = 0xF183; // AIN3 to GND

    uint8_t write_buf[3] = {0x01, (uint8_t)(config >> 8), (uint8_t)(config & 0xFF)};
    if (i2c_master_transmit(dev, write_buf, 3, pdMS_TO_TICKS(50)) != ESP_OK) {
        *hw_fault = 1; return 0.0f;
    }

    vTaskDelay(pdMS_TO_TICKS(10)); // Conversion time limit

    uint8_t read_cmd = 0x00;
    uint8_t read_buf[2] = {0};
    if (i2c_master_transmit_receive(dev, &read_cmd, 1, read_buf, 2, pdMS_TO_TICKS(50)) != ESP_OK) {
        *hw_fault = 1; return 0.0f;
    }

    int16_t raw_adc = (read_buf[0] << 8) | read_buf[1];
    return raw_adc * ADS1115_VOLTS_PER_BIT;
}

float read_max31856_temp(void) {
    spi_write_register(spi_max31856, 0x00, 0x80); // Auto mode
    spi_write_register(spi_max31856, 0x01, TC_TYPE_T_CR1_VAL);

    vTaskDelay(pdMS_TO_TICKS(160)); // Conversion time

    uint8_t rx_data[3];
    spi_read_registers(spi_max31856, 0x0C, rx_data, 3);

    int32_t temp_raw = (rx_data[0] << 16) | (rx_data[1] << 8) | rx_data[2];
    temp_raw >>= 5;
    if (temp_raw & 0x40000) temp_raw |= 0xFFF80000;

    return (float)temp_raw * 0.0078125f;
}

float read_max31865_temp(void) {
    spi_write_register(spi_max31865, 0x00, 0xD2); // VBIAS ON, Auto, 3-wire, Clear fault
    vTaskDelay(pdMS_TO_TICKS(100));

    uint8_t rx_data[2];
    spi_read_registers(spi_max31865, 0x01, rx_data, 2);

    uint16_t rtd_raw = (rx_data[0] << 8) | rx_data[1];
    rtd_raw >>= 1; // Drop fault bit

    float rtd_resistance = ((float)rtd_raw * RTD_REFERENCE_RESISTOR) / 32768.0f;
    return (rtd_resistance - RTD_NOMINAL_RESISTANCE) / (RTD_NOMINAL_RESISTANCE * 0.00385f);
}

// ==========================================
// Main Application Loop
// ==========================================
void app_main(void) {
    ESP_LOGI(TAG, "Starting WP2.6.i Node (Thermal Dummy ISO 15831)...");

    init_ethernet();   // Must run first: brings up netif/event loop that MQTT needs
    init_i2c_bus();
    init_spi_bus();
    init_mqtt();

    TickType_t xLastWakeTime = xTaskGetTickCount();
    const TickType_t xFrequency = pdMS_TO_TICKS(1000); // 1 Hz Sync frequency

    while (1) {
        ESP_LOGI(TAG, "--- Sensor Reading Cycle Start ---");

        uint8_t i2c_faults = 0;
        uint8_t tc_hw_faults[16] = {0};

        // --- 1. Read 16 Multiplexed Thermocouples + hardware fault register ---
        float tc_temps[16] = {0};
        for (uint8_t i = 0; i < 16; i++) {
            set_multiplexer_channel(i);
            tc_temps[i] = read_max31856_temp();
            tc_hw_faults[i] = check_thermocouple_hardware_fault();
        }

        // --- 2. Read RTD + hardware fault register ---
        float rtd_internal = read_max31865_temp();
        uint8_t rtd_hw_fault = check_rtd_hardware_fault();

        // --- 3. Read Analog Sensors (ADS1115 via I2C) ---
        float v_globe1 = read_ads1115_voltage(ads_gnd_dev, 0, &i2c_faults) * VOLTAGE_DIVIDER_RATIO;
        float v_globe2 = read_ads1115_voltage(ads_gnd_dev, 1, &i2c_faults) * VOLTAGE_DIVIDER_RATIO;
        float v_globe3 = read_ads1115_voltage(ads_gnd_dev, 2, &i2c_faults) * VOLTAGE_DIVIDER_RATIO;
        float v_rh     = read_ads1115_voltage(ads_gnd_dev, 3, &i2c_faults) * VOLTAGE_DIVIDER_RATIO;

        float v_iav1 = read_ads1115_voltage(ads_vdd_dev, 0, &i2c_faults) * VOLTAGE_DIVIDER_RATIO;
        float v_iav2 = read_ads1115_voltage(ads_vdd_dev, 1, &i2c_faults) * VOLTAGE_DIVIDER_RATIO;
        float v_iav3 = read_ads1115_voltage(ads_vdd_dev, 2, &i2c_faults) * VOLTAGE_DIVIDER_RATIO;
        float v_iav4 = read_ads1115_voltage(ads_vdd_dev, 3, &i2c_faults) * VOLTAGE_DIVIDER_RATIO;

        float v_iav5 = read_ads1115_voltage(ads_sda_dev, 0, &i2c_faults) * VOLTAGE_DIVIDER_RATIO;
        float v_iav6 = read_ads1115_voltage(ads_sda_dev, 1, &i2c_faults) * VOLTAGE_DIVIDER_RATIO;
        float v_iav7 = read_ads1115_voltage(ads_sda_dev, 2, &i2c_faults) * VOLTAGE_DIVIDER_RATIO;
        float v_iav8 = read_ads1115_voltage(ads_sda_dev, 3, &i2c_faults) * VOLTAGE_DIVIDER_RATIO;

        // --- 4. Apply Transfer Functions ---
        float globe1_c = v_globe1 * 10.0f;
        float globe2_c = v_globe2 * 10.0f;
        float globe3_c = v_globe3 * 10.0f;
        float rh_val   = (v_rh - 0.8f) / 0.031f;

        if (rh_val < 0.0f) rh_val = 0.0f;
        if (rh_val > 100.0f) rh_val = 100.0f;

        // --- 5. Log faults locally, REGARDLESS of MQTT connectivity ---
        // (same pattern established on WP2.1.i/.2.i/.3.i)
        for (int i = 0; i < 16; i++) {
            if (tc_hw_faults[i] != 0) {
                ESP_LOGW(TAG, "TC%d HARDWARE FAULT code 0x%02X", i + 1, tc_hw_faults[i]);
            } else if (is_temp_out_of_bounds(tc_temps[i])) {
                ESP_LOGW(TAG, "TC%d OUT OF BOUNDS: %.1f C", i + 1, tc_temps[i]);
            }
        }
        if (rtd_hw_fault != 0) {
            ESP_LOGW(TAG, "RTD HARDWARE FAULT code 0x%02X", rtd_hw_fault);
        } else if (is_temp_out_of_bounds(rtd_internal)) {
            ESP_LOGW(TAG, "RTD OUT OF BOUNDS: %.1f C", rtd_internal);
        }
        if (i2c_faults) ESP_LOGW(TAG, "One or more ADS1115 I2C faults this cycle");

        // --- 6. Generate JSON Payload & Publish ---
        // NOTE: payload structure below is UNCHANGED from your original file --
        // see my message for why this needs a decision before I touch it.
        if (is_mqtt_connected) {
            cJSON *root = cJSON_CreateObject();
            cJSON *dummy = cJSON_AddObjectToObject(root, "thermal_dummy");

            cJSON *surface = cJSON_AddObjectToObject(dummy, "surface");

            cJSON *head = cJSON_AddObjectToObject(surface, "head");
            cJSON_AddNumberToObject(head, "temp_c", tc_temps[0]);
            cJSON_AddNumberToObject(head, "rh_percent", rh_val);
            cJSON_AddNumberToObject(head, "iav_m_per_s", v_iav1 * 2.0f);
            cJSON_AddNumberToObject(head, "globle_temp_c", globe1_c);

            cJSON *torso = cJSON_AddObjectToObject(surface, "torso");
            cJSON_AddNumberToObject(torso, "temp_c_chest", tc_temps[1]);
            cJSON_AddNumberToObject(torso, "rh_percent_chest", 0.0f);
            cJSON_AddNumberToObject(torso, "iav_m_per_s_chest", v_iav2 * 2.0f);
            cJSON_AddNumberToObject(torso, "globle_temp_c_chest", globe2_c);
            cJSON_AddNumberToObject(torso, "temp_c_left_arm", tc_temps[2]);
            cJSON_AddNumberToObject(torso, "temp_c_right_arm", tc_temps[3]);

            cJSON *abdomen = cJSON_AddObjectToObject(surface, "abdomen");
            cJSON_AddNumberToObject(abdomen, "temp_c", tc_temps[6]);
            cJSON_AddNumberToObject(abdomen, "rh_percent", 0.0f);
            cJSON_AddNumberToObject(abdomen, "globle_temp_c", 0.0f);

            cJSON *bottom = cJSON_AddObjectToObject(surface, "bottom");
            cJSON_AddNumberToObject(bottom, "temp_c_left_thigh", tc_temps[7]);
            cJSON_AddNumberToObject(bottom, "temp_c_right_thigh", tc_temps[8]);
            cJSON_AddNumberToObject(bottom, "temp_c_left_knee", tc_temps[9]);
            cJSON_AddNumberToObject(bottom, "temp_c_right_knee", tc_temps[10]);
            cJSON_AddNumberToObject(bottom, "temp_c_left_calf", tc_temps[11]);
            cJSON_AddNumberToObject(bottom, "temp_c_right_calf", tc_temps[12]);
            cJSON_AddNumberToObject(bottom, "temp_c_left_foot", tc_temps[13]);
            cJSON_AddNumberToObject(bottom, "temp_c_right_foot", tc_temps[14]);

            cJSON *internal = cJSON_AddObjectToObject(dummy, "internal");

            cJSON *ihead = cJSON_AddObjectToObject(internal, "head");
            cJSON_AddNumberToObject(ihead, "temp_c_pad", rtd_internal);
            cJSON_AddBoolToObject(ihead, "on_status_pad", true);

            cJSON *itorso = cJSON_AddObjectToObject(internal, "torso");
            cJSON_AddNumberToObject(itorso, "temp_c_pad_left", rtd_internal);
            cJSON_AddBoolToObject(itorso, "on_status_pad_left", true);
            cJSON_AddNumberToObject(itorso, "temp_c_pad_right", rtd_internal);
            cJSON_AddBoolToObject(itorso, "on_status_pad_right", true);

            cJSON *iabdomen = cJSON_AddObjectToObject(internal, "abdomen");
            cJSON_AddNumberToObject(iabdomen, "temp_c_pad_left", rtd_internal);
            cJSON_AddBoolToObject(iabdomen, "on_status_pad_left", true);
            cJSON_AddNumberToObject(iabdomen, "temp_c_pad_right", rtd_internal);
            cJSON_AddBoolToObject(iabdomen, "on_status_pad_right", true);

            cJSON *ilegs = cJSON_AddObjectToObject(internal, "legs");
            cJSON_AddNumberToObject(ilegs, "temp_c_pad_left", rtd_internal);
            cJSON_AddBoolToObject(ilegs, "on_status_pad_left", true);
            cJSON_AddNumberToObject(ilegs, "temp_c_pad_right", rtd_internal);
            cJSON_AddBoolToObject(ilegs, "on_status_pad_right", true);

            cJSON *diag = cJSON_AddObjectToObject(dummy, "diagnostics");
            cJSON_AddBoolToObject(diag, "mqtt_connected", true);
            cJSON_AddBoolToObject(diag, "watchdog_triggered", false);
            cJSON *faults = cJSON_AddArrayToObject(diag, "sensor_faults");

            if (i2c_faults) {
                cJSON_AddItemToArray(faults, cJSON_CreateString("ADS1115_I2C_TIMEOUT"));
            }
            char fault_msg[50];
            for (int i = 0; i < 16; i++) {
                if (tc_hw_faults[i] != 0) {
                    snprintf(fault_msg, sizeof(fault_msg), "TC%d_HARDWARE_FAULT_CODE_0x%02X", i + 1, tc_hw_faults[i]);
                    cJSON_AddItemToArray(faults, cJSON_CreateString(fault_msg));
                }
            }
            if (rtd_hw_fault != 0) {
                snprintf(fault_msg, sizeof(fault_msg), "RTD_HARDWARE_FAULT_CODE_0x%02X", rtd_hw_fault);
                cJSON_AddItemToArray(faults, cJSON_CreateString(fault_msg));
            }

            char *json_str = cJSON_PrintUnformatted(root);
            esp_mqtt_client_publish(mqtt_client, MQTT_PUBLISH_TOPIC, json_str, 0, 1, 0);

            cJSON_Delete(root);
            free(json_str);
        } else {
            ESP_LOGW(TAG, "MQTT not connected. Sensors still read locally; publish skipped.");
        }

        // Wait to guarantee absolute 1 Hz frequency
        vTaskDelayUntil(&xLastWakeTime, xFrequency);
    }
}
