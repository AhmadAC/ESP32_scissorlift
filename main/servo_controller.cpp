#include "servo_controller.h"
#include "ble_server.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>

static const char *TAG = "SERVO_MG996R";

#define LEDC_TIMER          LEDC_TIMER_0
#define LEDC_MODE           LEDC_LOW_SPEED_MODE
#define LEDC_DUTY_RES       LEDC_TIMER_14_BIT
#define LEDC_FREQUENCY      50
#define MOTION_TICK_MS      20
#define SERVO_MAX_STEP_DEG  10.0f  // Max 10 deg per 20ms tick (500 deg/s slew cap for mechanical protection)
#define AUTO_IDLE_DELAY_MS  3000   // Relax PWM 3.0s after movement stops when Auto-Idle is enabled
#define NVS_SAVE_DELAY_MS   2500   // Save settled angle 2.5s after movement stops

static const gpio_num_t SERVO_PIN = SERVO_1_PIN;
static const ledc_channel_t SERVO_CHANNEL = LEDC_CHANNEL_0;

// Default sequences for 1 MG996R servo: "Name|angle,move_ms,wait_ms;..."
static const char *DEFAULT_ACTIONS[ACTION_NUM_SLOTS] = {
    "Center 90°|90,500,0",
    "Min 0°|0,500,0",
    "Max 180°|180,500,0",
    "Full Sweep|0,600,200;180,800,200;90,500,0",
    "Nod Cycle|60,350,150;120,350,150;60,350,150;90,400,0",
    "Step Scan|0,300,250;45,300,250;90,300,250;135,300,250;180,300,250;90,500,0",
    "Wiggle|75,200,100;105,200,100;75,200,100;105,200,100;90,300,0",
    "Rest Pose|0,600,0"
};

typedef struct {
    int angle;
    int move_ms;
    int wait_ms;
} seq_step_t;

static SemaphoreHandle_t s_mutex = NULL;

static float s_current_f        = (float)SERVO_DEFAULT_ANGLE;
static float s_target_f         = (float)SERVO_DEFAULT_ANGLE;
static int   s_offset           = 0;
static int   s_nvs_saved_angle  = SERVO_DEFAULT_ANGLE;
static bool  s_pos_dirty        = false;

// Starts DETACHED (duty = 0) at boot so MG996R never jerks or draws inrush current on startup
static bool  s_servo_enabled    = false;
static bool  s_force_pwm_sync   = false;
static bool  s_auto_idle        = false;
static TickType_t s_last_activity_tick = 0;

static char s_actions[ACTION_NUM_SLOTS][ACTION_MAX_LEN];

// Sequence execution state
static bool s_seq_running    = false;
static bool s_seq_loop       = false;
static bool s_seq_stop_req   = false;
static int  s_active_slot    = 0;
static int  s_seq_step_count = 0;
static int  s_seq_step_idx   = 0;
static seq_step_t s_seq_steps[ACTION_MAX_STEPS];

typedef enum {
    PHASE_MOVE = 0,
    PHASE_WAIT = 1
} seq_phase_t;

static seq_phase_t s_seq_phase = PHASE_MOVE;
static float s_step_start_f    = 90.0f;
static int   s_phase_elapsed_ms = 0;
static int   s_notify_counter   = 0;

static int clamp_int(int val, int min_v, int max_v) {
    if (val < min_v) return min_v;
    if (val > max_v) return max_v;
    return val;
}

static float clamp_float(float val, float min_v, float max_v) {
    if (val < min_v) return min_v;
    if (val > max_v) return max_v;
    return val;
}

static uint32_t angle_to_duty(float logical_angle, int offset) {
    float eff = clamp_float(logical_angle + (float)offset, (float)SERVO_MIN_ANGLE, (float)SERVO_MAX_ANGLE);
    float pulse_us = (float)SERVO_MIN_US + (((float)(SERVO_MAX_US - SERVO_MIN_US) * eff) / 180.0f);
    uint32_t duty = (uint32_t)((pulse_us * (float)((1 << 14) - 1)) / 20000.0f);
    return duty;
}

static void apply_hardware_pwm(void) {
    uint32_t duty = s_servo_enabled ? angle_to_duty(s_current_f, s_offset) : 0;
    ledc_set_duty(LEDC_MODE, SERVO_CHANNEL, duty);
    ledc_update_duty(LEDC_MODE, SERVO_CHANNEL);
}

static void save_last_angle_nvs(int a) {
    nvs_handle_t handle;
    if (nvs_open("arm_cfg", NVS_READWRITE, &handle) == ESP_OK) {
        nvs_set_i32(handle, "last_s1", (int32_t)a);
        nvs_commit(handle);
        nvs_close(handle);
    }
}

static void load_nvs_config(void) {
    nvs_handle_t handle;
    if (nvs_open("arm_cfg", NVS_READONLY, &handle) == ESP_OK) {
        int32_t val = 0;
        if (nvs_get_i32(handle, "cal_o1", &val) == ESP_OK) {
            s_offset = clamp_int((int)val, SERVO_OFFSET_MIN, SERVO_OFFSET_MAX);
        }
        if (nvs_get_i32(handle, "auto_idle", &val) == ESP_OK) {
            s_auto_idle = (val != 0);
        }

        int32_t ls = SERVO_DEFAULT_ANGLE;
        if (nvs_get_i32(handle, "last_s1", &ls) == ESP_OK) {
            int c = clamp_int((int)ls, SERVO_MIN_ANGLE, SERVO_MAX_ANGLE);
            s_current_f = s_target_f = (float)c;
            s_nvs_saved_angle = c;
        }
        nvs_close(handle);
    }

    if (nvs_open("arm_acts", NVS_READWRITE, &handle) == ESP_OK) {
        bool need_commit = false;
        for (int i = 0; i < ACTION_NUM_SLOTS; i++) {
            char key[8];
            snprintf(key, sizeof(key), "act%d", i + 1);
            size_t len = sizeof(s_actions[i]);
            esp_err_t err = nvs_get_str(handle, key, s_actions[i], &len);
            if (err != ESP_OK || len <= 1 || strchr(s_actions[i], '|') == NULL) {
                strncpy(s_actions[i], DEFAULT_ACTIONS[i], ACTION_MAX_LEN - 1);
                s_actions[i][ACTION_MAX_LEN - 1] = '\0';
                nvs_set_str(handle, key, s_actions[i]);
                need_commit = true;
            }
        }
        if (need_commit) nvs_commit(handle);
        nvs_close(handle);
    } else {
        for (int i = 0; i < ACTION_NUM_SLOTS; i++) {
            strncpy(s_actions[i], DEFAULT_ACTIONS[i], ACTION_MAX_LEN - 1);
            s_actions[i][ACTION_MAX_LEN - 1] = '\0';
        }
    }
}

static int parse_steps_internal(const char *steps_str, seq_step_t *out_steps, int max_steps) {
    if (!steps_str || !*steps_str || max_steps <= 0) return 0;

    char buf[ACTION_MAX_LEN];
    strncpy(buf, steps_str, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    int count = 0;
    char *saveptr = NULL;
    char *tok = strtok_r(buf, ";", &saveptr);
    while (tok != NULL && count < max_steps) {
        while (*tok == ' ' || *tok == '\t') tok++;
        if (*tok != '\0') {
            int angle = 90, move_ms = 400, wait_ms = 0;
            // Supports both single-servo format "angle,move,wait" and legacy "s1,s2,s3,move,wait"
            int p1 = 0, p2 = 0, p3 = 0, p4 = 0, p5 = 0;
            int n = sscanf(tok, "%d,%d,%d,%d,%d", &p1, &p2, &p3, &p4, &p5);
            if (n >= 5) {
                angle = p1;
                move_ms = p4;
                wait_ms = p5;
            } else if (n >= 3) {
                angle = p1;
                move_ms = p2;
                wait_ms = p3;
            } else if (n >= 1) {
                angle = p1;
            }
            out_steps[count].angle = clamp_int(angle, SERVO_MIN_ANGLE, SERVO_MAX_ANGLE);
            out_steps[count].move_ms = clamp_int(move_ms, 0, 10000);
            out_steps[count].wait_ms = clamp_int(wait_ms, 0, 30000);
            count++;
        }
        tok = strtok_r(NULL, ";", &saveptr);
    }
    return count;
}

static void notify_angle_now(int a) {
    if (!ble_server_is_connected()) return;
    char msg[32];
    snprintf(msg, sizeof(msg), "ANG:%d\n", a);
    ble_server_send_notify(msg);
}

static void notify_enable_now(bool en) {
    if (!ble_server_is_connected()) return;
    char msg[32];
    snprintf(msg, sizeof(msg), "SERVO_EN:%d\n", en ? 1 : 0);
    ble_server_send_notify(msg);
}

static void notify_action_state(bool running, int slot, bool loop) {
    if (!ble_server_is_connected()) return;
    char msg[64];
    snprintf(msg, sizeof(msg), "ACT_STATE:running=%d,slot=%d,loop=%d\n",
             running ? 1 : 0, slot, loop ? 1 : 0);
    ble_server_send_notify(msg);
}

static void servo_motion_task(void *pvParameters) {
    (void)pvParameters;
    TickType_t last_wake = xTaskGetTickCount();
    bool was_slewing = false;

    while (1) {
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(MOTION_TICK_MS));

        bool emit_ang = false;
        bool emit_done = false;
        bool emit_idle_en = false;
        bool do_nvs_save = false;
        int out_a = 90;
        int save_a = 90;

        xSemaphoreTake(s_mutex, portMAX_DELAY);

        if (s_seq_running) {
            was_slewing = false;
            if (s_seq_stop_req || s_seq_step_count <= 0) {
                s_seq_running = false;
                s_seq_loop = false;
                s_seq_stop_req = false;
                s_active_slot = 0;
                s_target_f = s_current_f;
                s_last_activity_tick = xTaskGetTickCount();
                s_pos_dirty = true;
                emit_done = true;
            } else {
                seq_step_t *st = &s_seq_steps[s_seq_step_idx];

                if (s_seq_phase == PHASE_MOVE) {
                    s_phase_elapsed_ms += MOTION_TICK_MS;
                    if (st->move_ms <= 0 || s_phase_elapsed_ms >= st->move_ms) {
                        s_current_f = (float)st->angle;
                        s_target_f = (float)st->angle;
                        s_force_pwm_sync = false;
                        apply_hardware_pwm();
                        s_seq_phase = PHASE_WAIT;
                        s_phase_elapsed_ms = 0;
                    } else {
                        float t = (float)s_phase_elapsed_ms / (float)st->move_ms;
                        float ease = (st->move_ms >= 100) ? (0.5f * (1.0f - cosf((float)M_PI * t))) : t;
                        s_current_f = s_step_start_f + (((float)st->angle - s_step_start_f) * ease);
                        s_target_f = s_current_f;
                        s_force_pwm_sync = false;
                        apply_hardware_pwm();
                    }
                    s_pos_dirty = true;
                } else {
                    s_phase_elapsed_ms += MOTION_TICK_MS;
                    if (s_phase_elapsed_ms >= st->wait_ms) {
                        s_seq_step_idx++;
                        if (s_seq_step_idx >= s_seq_step_count) {
                            if (s_seq_loop) {
                                s_seq_step_idx = 0;
                                s_step_start_f = s_current_f;
                                s_seq_phase = PHASE_MOVE;
                                s_phase_elapsed_ms = 0;
                            } else {
                                s_seq_running = false;
                                s_active_slot = 0;
                                s_last_activity_tick = xTaskGetTickCount();
                                s_pos_dirty = true;
                                emit_done = true;
                                emit_ang = true;
                            }
                        } else {
                            s_step_start_f = s_current_f;
                            s_seq_phase = PHASE_MOVE;
                            s_phase_elapsed_ms = 0;
                        }
                    }
                }

                s_notify_counter++;
                if (s_notify_counter >= 5) {
                    s_notify_counter = 0;
                    emit_ang = true;
                }
            }

            out_a = (int)lroundf(s_current_f);
        } else {
            // Manual slew / smooth follow
            if (s_servo_enabled) {
                float diff = s_target_f - s_current_f;
                if (fabsf(diff) > 0.05f) {
                    was_slewing = true;
                    if (fabsf(diff) <= SERVO_MAX_STEP_DEG) {
                        s_current_f = s_target_f;
                    } else {
                        s_current_f += (diff > 0.0f ? SERVO_MAX_STEP_DEG : -SERVO_MAX_STEP_DEG);
                    }
                    s_force_pwm_sync = false;
                    s_pos_dirty = true;
                    s_last_activity_tick = xTaskGetTickCount();
                    apply_hardware_pwm();
                } else if (s_force_pwm_sync) {
                    s_current_f = s_target_f;
                    s_force_pwm_sync = false;
                    apply_hardware_pwm();
                } else if (was_slewing) {
                    was_slewing = false;
                    emit_ang = true;
                    out_a = (int)lroundf(s_current_f);
                }
            } else {
                s_force_pwm_sync = false;
                was_slewing = false;
            }

            TickType_t idle_elapsed = xTaskGetTickCount() - s_last_activity_tick;

            if (s_pos_dirty && idle_elapsed >= pdMS_TO_TICKS(NVS_SAVE_DELAY_MS)) {
                s_pos_dirty = false;
                save_a = clamp_int((int)lroundf(s_current_f), SERVO_MIN_ANGLE, SERVO_MAX_ANGLE);
                if (save_a != s_nvs_saved_angle) {
                    s_nvs_saved_angle = save_a;
                    do_nvs_save = true;
                }
            }

            if (s_auto_idle && s_servo_enabled && idle_elapsed >= pdMS_TO_TICKS(AUTO_IDLE_DELAY_MS)) {
                s_servo_enabled = false;
                s_force_pwm_sync = false;
                apply_hardware_pwm();
                emit_idle_en = true;
            }
        }

        xSemaphoreGive(s_mutex);

        if (do_nvs_save) {
            save_last_angle_nvs(save_a);
        }
        if (emit_ang) {
            notify_angle_now(out_a);
        }
        if (emit_done) {
            notify_action_state(false, 0, false);
        }
        if (emit_idle_en) {
            notify_enable_now(false);
        }
    }
}

void servo_controller_init(void) {
    s_mutex = xSemaphoreCreateMutex();
    load_nvs_config();
    s_last_activity_tick = xTaskGetTickCount();

    ledc_timer_config_t ledc_timer = {};
    ledc_timer.speed_mode       = LEDC_MODE;
    ledc_timer.duty_resolution  = LEDC_DUTY_RES;
    ledc_timer.timer_num        = LEDC_TIMER;
    ledc_timer.freq_hz          = LEDC_FREQUENCY;
    ledc_timer.clk_cfg          = LEDC_AUTO_CLK;
    ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));

    // Initialize with duty = 0 (detached) so MG996R does not jerk at boot
    s_servo_enabled  = false;
    s_force_pwm_sync = false;
    ledc_channel_config_t ledc_channel = {};
    ledc_channel.gpio_num       = SERVO_PIN;
    ledc_channel.speed_mode     = LEDC_MODE;
    ledc_channel.channel        = SERVO_CHANNEL;
    ledc_channel.intr_type      = LEDC_INTR_DISABLE;
    ledc_channel.timer_sel      = LEDC_TIMER;
    ledc_channel.duty           = 0;
    ledc_channel.hpoint         = 0;
    ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel));

    xTaskCreatePinnedToCore(servo_motion_task, "servo_motion_task", 4096, NULL, 5, NULL, 1);

    ESP_LOGI(TAG, "MG996R Servo Controller initialized on GPIO 1 (Detached at boot, Angle: %d deg, Trim: %d deg)",
             (int)lroundf(s_current_f), s_offset);
}

void servo_set_angle(int angle) {
    angle = clamp_int(angle, SERVO_MIN_ANGLE, SERVO_MAX_ANGLE);

    bool was_running = false;
    bool newly_enabled = false;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_seq_running) {
        s_seq_running = false;
        s_seq_loop = false;
        s_seq_stop_req = false;
        s_active_slot = 0;
        was_running = true;
    }
    if (!s_servo_enabled) {
        s_servo_enabled = true;
        s_force_pwm_sync = true;
        newly_enabled = true;
    }

    s_target_f = (float)angle;
    s_last_activity_tick = xTaskGetTickCount();
    xSemaphoreGive(s_mutex);

    if (was_running) {
        notify_action_state(false, 0, false);
    }
    if (newly_enabled) {
        notify_enable_now(true);
    }
}

void servo_set_angle_idx(int idx, int angle) {
    (void)idx;
    servo_set_angle(angle);
}

void servo_move_smooth(int angle, int duration_ms) {
    angle = clamp_int(angle, SERVO_MIN_ANGLE, SERVO_MAX_ANGLE);
    duration_ms = clamp_int(duration_ms, 0, 10000);

    if (duration_ms == 0) duration_ms = 300;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_servo_enabled = true;
    s_seq_steps[0].angle = angle;
    s_seq_steps[0].move_ms = duration_ms;
    s_seq_steps[0].wait_ms = 0;
    s_seq_step_count = 1;
    s_seq_step_idx = 0;
    s_seq_loop = false;
    s_seq_stop_req = false;
    s_active_slot = 0;
    s_seq_phase = PHASE_MOVE;
    s_phase_elapsed_ms = 0;
    s_notify_counter = 0;
    s_last_activity_tick = xTaskGetTickCount();
    s_step_start_f = s_current_f;
    s_seq_running = true;
    xSemaphoreGive(s_mutex);

    notify_enable_now(true);
    notify_action_state(true, 0, false);
}

int servo_get_angle(void) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int a = (int)lroundf(s_target_f);
    xSemaphoreGive(s_mutex);
    return a;
}

int servo_get_angle_idx(int idx) {
    (void)idx;
    return servo_get_angle();
}

void servo_detach(void) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_seq_running = false;
    s_seq_loop = false;
    s_seq_stop_req = false;
    s_active_slot = 0;
    s_servo_enabled = false;
    s_force_pwm_sync = false;
    s_target_f = s_current_f;
    apply_hardware_pwm();
    xSemaphoreGive(s_mutex);

    notify_action_state(false, 0, false);
    notify_enable_now(false);
    ESP_LOGI(TAG, "MG996R detached (duty=0, idle).");
}

void servo_detach_idx(int idx) {
    (void)idx;
    servo_detach();
}

void servo_attach(void) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_servo_enabled = true;
    s_force_pwm_sync = false;
    s_last_activity_tick = xTaskGetTickCount();
    apply_hardware_pwm();
    xSemaphoreGive(s_mutex);

    notify_enable_now(true);
    ESP_LOGI(TAG, "MG996R attached (PWM enabled).");
}

bool servo_is_enabled(void) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool en = s_servo_enabled;
    xSemaphoreGive(s_mutex);
    return en;
}

void servo_set_auto_idle(bool enabled) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_auto_idle = enabled;
    s_last_activity_tick = xTaskGetTickCount();
    xSemaphoreGive(s_mutex);

    nvs_handle_t handle;
    if (nvs_open("arm_cfg", NVS_READWRITE, &handle) == ESP_OK) {
        nvs_set_i32(handle, "auto_idle", enabled ? 1 : 0);
        nvs_commit(handle);
        nvs_close(handle);
    }
    ESP_LOGI(TAG, "Auto-Idle mode set to: %s", enabled ? "ON" : "OFF");
}

bool servo_get_auto_idle(void) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool en = s_auto_idle;
    xSemaphoreGive(s_mutex);
    return en;
}

void servo_set_offset(int offset) {
    offset = clamp_int(offset, SERVO_OFFSET_MIN, SERVO_OFFSET_MAX);
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_offset = offset;
    s_servo_enabled = true;
    s_force_pwm_sync = true;
    s_last_activity_tick = xTaskGetTickCount();
    xSemaphoreGive(s_mutex);
}

int servo_get_offset(void) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int o = s_offset;
    xSemaphoreGive(s_mutex);
    return o;
}

void servo_save_offset(int offset) {
    offset = clamp_int(offset, SERVO_OFFSET_MIN, SERVO_OFFSET_MAX);
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_offset = offset;
    apply_hardware_pwm();
    xSemaphoreGive(s_mutex);

    nvs_handle_t handle;
    if (nvs_open("arm_cfg", NVS_READWRITE, &handle) == ESP_OK) {
        nvs_set_i32(handle, "cal_o1", (int32_t)offset);
        nvs_commit(handle);
        nvs_close(handle);
    }
    ESP_LOGI(TAG, "Saved MG996R trim offset to NVS: %d deg", offset);
}

bool servo_action_set(int slot, const char *def_str) {
    if (slot < 1 || slot > ACTION_NUM_SLOTS || def_str == NULL) return false;
    const char *pipe = strchr(def_str, '|');
    if (pipe == NULL || pipe == def_str) return false;

    seq_step_t tmp[ACTION_MAX_STEPS];
    int count = parse_steps_internal(pipe + 1, tmp, ACTION_MAX_STEPS);
    if (count <= 0) return false;

    int idx = slot - 1;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    strncpy(s_actions[idx], def_str, ACTION_MAX_LEN - 1);
    s_actions[idx][ACTION_MAX_LEN - 1] = '\0';
    xSemaphoreGive(s_mutex);

    nvs_handle_t handle;
    if (nvs_open("arm_acts", NVS_READWRITE, &handle) == ESP_OK) {
        char key[8];
        snprintf(key, sizeof(key), "act%d", slot);
        nvs_set_str(handle, key, s_actions[idx]);
        nvs_commit(handle);
        nvs_close(handle);
    }
    ESP_LOGI(TAG, "Action %d saved (%d steps): %s", slot, count, s_actions[idx]);
    return true;
}

const char* servo_action_get(int slot) {
    if (slot < 1 || slot > ACTION_NUM_SLOTS) return "";
    return s_actions[slot - 1];
}

void servo_action_reset_defaults(void) {
    nvs_handle_t handle;
    bool nvs_ok = (nvs_open("arm_acts", NVS_READWRITE, &handle) == ESP_OK);

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < ACTION_NUM_SLOTS; i++) {
        strncpy(s_actions[i], DEFAULT_ACTIONS[i], ACTION_MAX_LEN - 1);
        s_actions[i][ACTION_MAX_LEN - 1] = '\0';
        if (nvs_ok) {
            char key[8];
            snprintf(key, sizeof(key), "act%d", i + 1);
            nvs_set_str(handle, key, s_actions[i]);
        }
    }
    xSemaphoreGive(s_mutex);

    if (nvs_ok) {
        nvs_commit(handle);
        nvs_close(handle);
    }
    ESP_LOGI(TAG, "Actions reset to MG996R defaults.");
}

bool servo_action_run(int slot, bool loop) {
    if (slot < 1 || slot > ACTION_NUM_SLOTS) return false;
    const char *def = s_actions[slot - 1];
    const char *pipe = strchr(def, '|');
    if (!pipe) return false;

    seq_step_t parsed_steps[ACTION_MAX_STEPS];
    int count = parse_steps_internal(pipe + 1, parsed_steps, ACTION_MAX_STEPS);
    if (count <= 0) return false;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_servo_enabled = true;
    memcpy(s_seq_steps, parsed_steps, sizeof(seq_step_t) * count);
    s_seq_step_count = count;
    s_seq_step_idx = 0;
    s_seq_loop = loop;
    s_seq_stop_req = false;
    s_active_slot = slot;
    s_seq_phase = PHASE_MOVE;
    s_phase_elapsed_ms = 0;
    s_notify_counter = 0;
    s_last_activity_tick = xTaskGetTickCount();
    s_step_start_f = s_current_f;
    s_seq_running = true;
    xSemaphoreGive(s_mutex);

    notify_enable_now(true);
    notify_action_state(true, slot, loop);
    return true;
}

bool servo_run_sequence_str(const char *steps_str, bool loop) {
    seq_step_t parsed_steps[ACTION_MAX_STEPS];
    int count = parse_steps_internal(steps_str, parsed_steps, ACTION_MAX_STEPS);
    if (count <= 0) return false;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_servo_enabled = true;
    memcpy(s_seq_steps, parsed_steps, sizeof(seq_step_t) * count);
    s_seq_step_count = count;
    s_seq_step_idx = 0;
    s_seq_loop = loop;
    s_seq_stop_req = false;
    s_active_slot = 0;
    s_seq_phase = PHASE_MOVE;
    s_phase_elapsed_ms = 0;
    s_notify_counter = 0;
    s_last_activity_tick = xTaskGetTickCount();
    s_step_start_f = s_current_f;
    s_seq_running = true;
    xSemaphoreGive(s_mutex);

    notify_enable_now(true);
    notify_action_state(true, 0, loop);
    return true;
}

void servo_stop_motion(void) {
    int a = 90;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_seq_running = false;
    s_seq_loop = false;
    s_seq_stop_req = false;
    s_active_slot = 0;
    s_target_f = s_current_f;
    s_last_activity_tick = xTaskGetTickCount();
    s_pos_dirty = true;
    a = (int)lroundf(s_current_f);
    xSemaphoreGive(s_mutex);

    notify_angle_now(a);
    notify_action_state(false, 0, false);
}

bool servo_is_sequence_running(void) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool r = s_seq_running;
    xSemaphoreGive(s_mutex);
    return r;
}

int servo_get_active_slot(void) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int s = s_active_slot;
    xSemaphoreGive(s_mutex);
    return s;
}

bool servo_is_sequence_looping(void) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool l = s_seq_loop;
    xSemaphoreGive(s_mutex);
    return l;
}