#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "servo_controller.h"
#include "ble_server.h"
#include "console_serial.h"
#include "command_processor.h"
#include "joystick.h"

static const char *TAG = "APP_MAIN";

extern "C" void app_main(void) {
    // Immediately pull GPIO 1 LOW at power-up to ensure MG996R never jerks while MCU initializes
    gpio_reset_pin(SERVO_1_PIN);
    gpio_set_direction(SERVO_1_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(SERVO_1_PIN, 0);
    gpio_pulldown_en(SERVO_1_PIN);

    vTaskDelay(pdMS_TO_TICKS(200));

    esp_reset_reason_t rst = esp_reset_reason();
    if (rst == ESP_RST_BROWNOUT) {
        ESP_LOGW(TAG, "WARNING: Previous reset was caused by BROWNOUT! Ensure MG996R has dedicated 5V/6V PSU.");
    }

    ESP_LOGI(TAG, "Booting YD-ESP32-S3 MG996R Single Servo Controller (reset=%d)...", (int)rst);

    // 1. Non-Volatile Storage (NVS)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 2. Command Processor
    command_processor_init();

    // 3. Bluetooth Low Energy NUS Server ('ESP32S3-Arm')
    ble_server_init(command_on_ble_received);

    // 4. MG996R Servo Controller on GPIO 1 (starts detached with duty = 0)
    servo_controller_init();

    // 5. PS2 Joystick on VRx=GPIO2, SW=GPIO4
    joystick_init();

    // 6. Serial REPL (UART0 & Native USB Serial/JTAG)
    console_serial_init();

    ESP_LOGI(TAG, "Subsystems active. MG996R ready for Joystick, Serial, and BLE control.");
}