#include "servo_controller.h"
#include "ble_server.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

static const char *TAG = "SERVO_CTRL";

#define LEDC_TIMER              LEDC_TIMER_0
#define LEDC_MODE               LEDC_LOW_SPEED_MODE
#define LEDC_CHANNEL            LEDC_CHANNEL_0
#define LEDC_DUTY_RES           LEDC_TIMER_14_BIT
#define LEDC_FREQUENCY_HZ       50
#define LEDC_PERIOD_US          20000
#define LEDC_MAX_DUTY           16383

#define AUTO_IDLE_TIMEOUT_US    3000000

typedef struct {
    int angle;
    int move_ms;
    int wait_ms;
} servo_step_t;

static const char *DEFAULT_ACTIONS[ACTION_NUM_SLOTS] = {
    "Center 90\xC2\xB0|90,500,0",
    "Min 0\xC2\xB0|0,500,0",
    "Max 180\xC2\xB0|180,500,0",
    "Full Sweep|0,600,200;180,800,200;90,500,0",
    "Nod Cycle|60,350,150;120,350,150;60,350,150;90,400,0",
    "Step Scan|0,300,250;45,300,250;90,300,250;135,300,250;180,300,250;90,500,0",
    "Wiggle|75,200,100;105,200,100;75,200,100;105,200,100;90,300,0",
    "Rest Pose|0,600,0"
};

static portMUX_TYPE s_servo_mux = portMUX_INITIALIZER_UNLOCKED;

static int  s_angle = SERVO_DEFAULT_ANGLE;
static int  s_offset = 0;
static bool s_enabled = false;
static bool s_auto_idle = false;
static int64_t s_last_activity_us = 0;

static char s_action_slots[ACTION_NUM_SLOTS][ACTION_MAX_LEN];

static TaskHandle_t s_seq_task_handle = NULL;
static volatile bool s_seq_running = false;
static volatile bool s_seq_looping = false;
static volatile int  s_seq_active_slot = 0;
static volatile bool s_seq_stop_req = false;

static servo_step_t s_active_steps[ACTION_MAX_STEPS];
static size_t       s_active_step_count = 0;

static int clamp_angle(int angle) {
    if (angle < SERVO_MIN_ANGLE) return SERVO_MIN_ANGLE;
    if (angle > SERVO_MAX_ANGLE) return SERVO_MAX_ANGLE;
    return angle;
}

static int clamp_offset(int offset) {
    if (offset < SERVO_OFFSET_MIN) return SERVO_OFFSET_MIN;
    if (offset > SERVO_OFFSET_MAX) return SERVO_OFFSET_MAX;
    return offset;
}

static uint32_t calc_duty(int angle, int offset) {
    int eff = clamp_angle(angle + offset);
    uint32_t pulse_us = SERVO_MIN_US + ((uint32_t)eff * (SERVO_MAX_US - SERVO_MIN_US)) / 180;
    if (pulse_us < SERVO_MIN_US) pulse_us = SERVO_MIN_US;
    if (pulse_us > SERVO_MAX_US) pulse_us = SERVO_MAX_US;
    return (pulse_us * LEDC_MAX_DUTY) / LEDC_PERIOD_US;
}

static void apply_hardware_pwm(int angle, int offset, bool enable) {
    if (enable) {
        uint32_t duty = calc_duty(angle, offset);
        ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, duty);
        ledc_update_duty(LEDC_MODE, LEDC_CHANNEL);
    } else {
        ledc_stop(LEDC_MODE, LEDC_CHANNEL, 0);
    }
}

static void notify_angle(void) {
    char msg[32];
    snprintf(msg, sizeof(msg), "ANG:%d\n", servo_get_angle());
    ble_server_send_notify(msg);
}

static void notify_enable_state(void) {
    char msg[32];
    snprintf(msg, sizeof(msg), "SERVO_EN:%d\n", servo_is_enabled() ? 1 : 0);
    ble_server_send_notify(msg);
}

static void notify_action_state(bool running, int slot, bool loop) {
    char msg[64];
    snprintf(msg, sizeof(msg), "ACT_STATE:running=%d,slot=%d,loop=%d\n",
             running ? 1 : 0, slot, loop ? 1 : 0);
    ble_server_send_notify(msg);
}

static void load_nvs_configuration(void) {
    nvs_handle_t h;
    if (nvs_open("servo_cal", NVS_READONLY, &h) == ESP_OK) {
        int32_t off = 0, idle = 0;
        if (nvs_get_i32(h, "off_0", &off) == ESP_OK) s_offset = clamp_offset((int)off);
        if (nvs_get_i32(h, "auto_idle", &idle) == ESP_OK) s_auto_idle = (idle != 0);
        nvs_close(h);
    }

    if (nvs_open("arm_acts", NVS_READONLY, &h) == ESP_OK) {
        for (int i = 0; i < ACTION_NUM_SLOTS; i++) {
            char key[16];
            snprintf(key, sizeof(key), "act_%d", i + 1);
            size_t req_len = sizeof(s_action_slots[i]);
            if (nvs_get_str(h, key, s_action_slots[i], &req_len) != ESP_OK || req_len == 0) {
                strncpy(s_action_slots[i], DEFAULT_ACTIONS[i], sizeof(s_action_slots[i]) - 1);
                s_action_slots[i][sizeof(s_action_slots[i]) - 1] = '\0';
            }
        }
        nvs_close(h);
    } else {
        for (int i = 0; i < ACTION_NUM_SLOTS; i++) {
            strncpy(s_action_slots[i], DEFAULT_ACTIONS[i], sizeof(s_action_slots[i]) - 1);
            s_action_slots[i][sizeof(s_action_slots[i]) - 1] = '\0';
        }
    }
}

static size_t parse_sequence_steps(const char *steps_str, servo_step_t *out_steps, size_t max_steps) {
    if (!steps_str || !out_steps || max_steps == 0) return 0;

    size_t count = 0;
    const char *p = steps_str;

    while (*p && count < max_steps) {
        while (*p == ' ' || *p == ';') p++;
        if (!*p) break;

        int v1 = 90, v2 = 400, v3 = 0, v4 = 400, v5 = 0;
        int matched = sscanf(p, "%d,%d,%d,%d,%d", &v1, &v2, &v3, &v4, &v5);
        if (matched >= 1) {
            int angle = v1;
            int move = 400;
            int wait = 0;
            if (matched >= 5) {
                move = v4;
                wait = v5;
            } else {
                if (matched >= 2) move = v2;
                if (matched >= 3) wait = v3;
            }
            out_steps[count].angle = clamp_angle(angle);
            out_steps[count].move_ms = (move < 20) ? 20 : (move > 10000 ? 10000 : move);
            out_steps[count].wait_ms = (wait > 0) ? (wait > 30000 ? 30000 : wait) : 0;
            count++;
        }

        const char *semi = strchr(p, ';');
        if (!semi) break;
        p = semi + 1;
    }

    return count;
}

static void interpolate_move(int target_angle, int duration_ms) {
    target_angle = clamp_angle(target_angle);

    int start_angle = 90;
    int off = 0;
    bool was_enabled = false;

    portENTER_CRITICAL(&s_servo_mux);
    start_angle = s_angle;
    off = s_offset;
    was_enabled = s_enabled;
    s_enabled = true;
    s_last_activity_us = esp_timer_get_time();
    portEXIT_CRITICAL(&s_servo_mux);

    if (!was_enabled) {
        notify_enable_state();
    }

    if (duration_ms <= 20) {
        portENTER_CRITICAL(&s_servo_mux);
        s_angle = target_angle;
        s_last_activity_us = esp_timer_get_time();
        portEXIT_CRITICAL(&s_servo_mux);

        apply_hardware_pwm(target_angle, off, true);
        notify_angle();
        return;
    }

    const int step_interval_ms = 20;
    int total_steps = duration_ms / step_interval_ms;
    if (total_steps < 1) total_steps = 1;

    for (int step = 1; step <= total_steps; step++) {
        if (s_seq_stop_req) break;

        float t = (float)step / (float)total_steps;
        float ease = 0.5f * (1.0f - cosf(t * 3.14159265f));
        int cur = (int)lroundf((float)start_angle + ((float)target_angle - (float)start_angle) * ease);

        portENTER_CRITICAL(&s_servo_mux);
        s_angle = cur;
        s_last_activity_us = esp_timer_get_time();
        off = s_offset;
        portEXIT_CRITICAL(&s_servo_mux);

        apply_hardware_pwm(cur, off, true);
        vTaskDelay(pdMS_TO_TICKS(step_interval_ms));
    }

    if (!s_seq_stop_req) {
        portENTER_CRITICAL(&s_servo_mux);
        s_angle = target_angle;
        s_last_activity_us = esp_timer_get_time();
        off = s_offset;
        portEXIT_CRITICAL(&s_servo_mux);

        apply_hardware_pwm(target_angle, off, true);
        notify_angle();
    }
}

static void sequence_runner_task(void *pvParameters) {
    (void)pvParameters;

    do {
        for (size_t i = 0; i < s_active_step_count; i++) {
            if (s_seq_stop_req) break;

            servo_step_t step = s_active_steps[i];
            interpolate_move(step.angle, step.move_ms);

            if (s_seq_stop_req) break;

            if (step.wait_ms > 0) {
                int waited = 0;
                while (waited < step.wait_ms && !s_seq_stop_req) {
                    int chunk = (step.wait_ms - waited > 20) ? 20 : (step.wait_ms - waited);
                    vTaskDelay(pdMS_TO_TICKS(chunk));
                    waited += chunk;
                }
            }
        }
    } while (s_seq_looping && !s_seq_stop_req);

    s_seq_running = false;
    s_seq_looping = false;
    int prev_slot = s_seq_active_slot;
    s_seq_active_slot = 0;
    s_seq_stop_req = false;

    notify_action_state(false, 0, false);
    ESP_LOGI(TAG, "Sequence execution completed (was slot %d).", prev_slot);

    s_seq_task_handle = NULL;
    vTaskDelete(NULL);
}

static void auto_idle_monitor_task(void *pvParameters) {
    (void)pvParameters;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(250));

        if (!s_auto_idle || s_seq_running) continue;

        int64_t now_us = esp_timer_get_time();
        bool enabled = false;
        int64_t last_us = 0;

        portENTER_CRITICAL(&s_servo_mux);
        enabled = s_enabled;
        last_us = s_last_activity_us;
        portEXIT_CRITICAL(&s_servo_mux);

        if (enabled && (now_us - last_us > AUTO_IDLE_TIMEOUT_US)) {
            servo_detach();
            ESP_LOGI(TAG, "Auto-idle triggered: MG996R detached to prevent heat and stall buzz.");
        }
    }
}

void servo_controller_init(void) {
    load_nvs_configuration();

    ledc_timer_config_t ledc_timer = {};
    ledc_timer.speed_mode       = LEDC_MODE;
    ledc_timer.timer_num        = LEDC_TIMER;
    ledc_timer.duty_resolution  = LEDC_DUTY_RES;
    ledc_timer.freq_hz          = LEDC_FREQUENCY_HZ;
    ledc_timer.clk_cfg          = LEDC_AUTO_CLK;
    ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));

    ledc_channel_config_t ledc_ch = {};
    ledc_ch.channel    = LEDC_CHANNEL;
    ledc_ch.duty       = 0;
    ledc_ch.gpio_num   = SERVO_1_PIN;
    ledc_ch.speed_mode = LEDC_MODE;
    ledc_ch.hpoint     = 0;
    ledc_ch.timer_sel  = LEDC_TIMER;
    ESP_ERROR_CHECK(ledc_channel_config(&ledc_ch));

    // Hold GPIO 1 LOW via LEDC idle state without overriding GPIO matrix routing
    ledc_stop(LEDC_MODE, LEDC_CHANNEL, 0);

    s_last_activity_us = esp_timer_get_time();

    xTaskCreatePinnedToCore(auto_idle_monitor_task, "auto_idle_task", 2048, NULL, 2, NULL, 1);
    ESP_LOGI(TAG, "MG996R Servo controller initialized on GPIO %d (starts DETACHED).", (int)SERVO_1_PIN);
}

void servo_set_angle(int angle) {
    angle = clamp_angle(angle);

    int off = 0;
    bool was_enabled = false;
    portENTER_CRITICAL(&s_servo_mux);
    s_angle = angle;
    was_enabled = s_enabled;
    s_enabled = true;
    s_last_activity_us = esp_timer_get_time();
    off = s_offset;
    portEXIT_CRITICAL(&s_servo_mux);

    apply_hardware_pwm(angle, off, true);
    if (!was_enabled) {
        notify_enable_state();
    }
}

void servo_set_angle_idx(int idx, int angle) {
    if (idx == 0) {
        servo_set_angle(angle);
    }
}

void servo_move_smooth(int angle, int duration_ms) {
    servo_stop_motion();
    interpolate_move(angle, duration_ms);
}

int servo_get_angle(void) {
    portENTER_CRITICAL(&s_servo_mux);
    int a = s_angle;
    portEXIT_CRITICAL(&s_servo_mux);
    return a;
}

int servo_get_angle_idx(int idx) {
    (void)idx;
    return servo_get_angle();
}

void servo_stop_motion(void) {
    if (s_seq_running) {
        s_seq_stop_req = true;
        for (int i = 0; i < 25 && s_seq_running; i++) {
            vTaskDelay(pdMS_TO_TICKS(15));
        }
    }
}

void servo_detach(void) {
    servo_stop_motion();
    int angle = 90, off = 0;
    portENTER_CRITICAL(&s_servo_mux);
    s_enabled = false;
    angle = s_angle;
    off = s_offset;
    portEXIT_CRITICAL(&s_servo_mux);

    apply_hardware_pwm(angle, off, false);
    notify_enable_state();
}

void servo_detach_idx(int idx) {
    if (idx == 0) {
        servo_detach();
    }
}

void servo_attach(void) {
    int angle = 90, off = 0;
    portENTER_CRITICAL(&s_servo_mux);
    s_enabled = true;
    s_last_activity_us = esp_timer_get_time();
    angle = s_angle;
    off = s_offset;
    portEXIT_CRITICAL(&s_servo_mux);

    apply_hardware_pwm(angle, off, true);
    notify_enable_state();
}

bool servo_is_enabled(void) {
    portENTER_CRITICAL(&s_servo_mux);
    bool en = s_enabled;
    portEXIT_CRITICAL(&s_servo_mux);
    return en;
}

void servo_set_auto_idle(bool enabled) {
    s_auto_idle = enabled;
    s_last_activity_us = esp_timer_get_time();

    nvs_handle_t h;
    if (nvs_open("servo_cal", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, "auto_idle", enabled ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
}

bool servo_get_auto_idle(void) {
    return s_auto_idle;
}

void servo_set_offset(int offset) {
    offset = clamp_offset(offset);

    int angle = 90;
    bool was_enabled = false;
    portENTER_CRITICAL(&s_servo_mux);
    s_offset = offset;
    was_enabled = s_enabled;
    s_enabled = true;
    s_last_activity_us = esp_timer_get_time();
    angle = s_angle;
    portEXIT_CRITICAL(&s_servo_mux);

    apply_hardware_pwm(angle, offset, true);
    if (!was_enabled) {
        notify_enable_state();
    }
}

int servo_get_offset(void) {
    portENTER_CRITICAL(&s_servo_mux);
    int o = s_offset;
    portEXIT_CRITICAL(&s_servo_mux);
    return o;
}

void servo_save_offset(int offset) {
    offset = clamp_offset(offset);

    int angle = 90;
    bool en = false;
    portENTER_CRITICAL(&s_servo_mux);
    s_offset = offset;
    angle = s_angle;
    en = s_enabled;
    portEXIT_CRITICAL(&s_servo_mux);

    if (en) {
        apply_hardware_pwm(angle, offset, true);
    }

    nvs_handle_t h;
    if (nvs_open("servo_cal", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, "off_0", (int32_t)offset);
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGI(TAG, "Trim offset saved to NVS: %d", offset);
    }
}

bool servo_action_set(int slot, const char *def_str) {
    if (slot < 1 || slot > ACTION_NUM_SLOTS || !def_str || strlen(def_str) == 0) return false;

    strncpy(s_action_slots[slot - 1], def_str, sizeof(s_action_slots[slot - 1]) - 1);
    s_action_slots[slot - 1][sizeof(s_action_slots[slot - 1]) - 1] = '\0';

    nvs_handle_t h;
    if (nvs_open("arm_acts", NVS_READWRITE, &h) == ESP_OK) {
        char key[16];
        snprintf(key, sizeof(key), "act_%d", slot);
        nvs_set_str(h, key, s_action_slots[slot - 1]);
        nvs_commit(h);
        nvs_close(h);
    }
    return true;
}

const char* servo_action_get(int slot) {
    if (slot < 1 || slot > ACTION_NUM_SLOTS) return "";
    return s_action_slots[slot - 1];
}

void servo_action_reset_defaults(void) {
    nvs_handle_t h;
    bool nvs_ok = (nvs_open("arm_acts", NVS_READWRITE, &h) == ESP_OK);

    for (int i = 0; i < ACTION_NUM_SLOTS; i++) {
        strncpy(s_action_slots[i], DEFAULT_ACTIONS[i], sizeof(s_action_slots[i]) - 1);
        s_action_slots[i][sizeof(s_action_slots[i]) - 1] = '\0';
        if (nvs_ok) {
            char key[16];
            snprintf(key, sizeof(key), "act_%d", i + 1);
            nvs_set_str(h, key, s_action_slots[i]);
        }
    }
    if (nvs_ok) {
        nvs_commit(h);
        nvs_close(h);
    }
}

bool servo_action_run(int slot, bool loop) {
    if (slot < 1 || slot > ACTION_NUM_SLOTS) return false;

    servo_stop_motion();

    const char *def = s_action_slots[slot - 1];
    const char *pipe = strchr(def, '|');
    const char *steps_str = pipe ? (pipe + 1) : def;

    s_active_step_count = parse_sequence_steps(steps_str, s_active_steps, ACTION_MAX_STEPS);
    if (s_active_step_count == 0) return false;

    s_seq_running = true;
    s_seq_looping = loop;
    s_seq_active_slot = slot;
    s_seq_stop_req = false;

    notify_action_state(true, slot, loop);
    xTaskCreatePinnedToCore(sequence_runner_task, "seq_run_task", 4096, NULL, 5, &s_seq_task_handle, 1);
    return true;
}

bool servo_run_sequence_str(const char *steps_str, bool loop) {
    if (!steps_str || strlen(steps_str) == 0) return false;

    servo_stop_motion();

    s_active_step_count = parse_sequence_steps(steps_str, s_active_steps, ACTION_MAX_STEPS);
    if (s_active_step_count == 0) return false;

    s_seq_running = true;
    s_seq_looping = loop;
    s_seq_active_slot = 0;
    s_seq_stop_req = false;

    notify_action_state(true, 0, loop);
    xTaskCreatePinnedToCore(sequence_runner_task, "seq_run_task", 4096, NULL, 5, &s_seq_task_handle, 1);
    return true;
}

bool servo_is_sequence_running(void) {
    return s_seq_running;
}

int servo_get_active_slot(void) {
    return s_seq_active_slot;
}

bool servo_is_sequence_looping(void) {
    return s_seq_looping;
}