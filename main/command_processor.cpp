#include "command_processor.h"
#include "servo_controller.h"
#include "joystick.h"
#include "ble_server.h"
#include "esp_system.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

static const char *TAG = "CMD_PROC";

static bool parse_bool_arg(const char *s) {
    if (!s) return false;
    return (strcasecmp(s, "on") == 0 || strcmp(s, "1") == 0 || strcasecmp(s, "true") == 0);
}

void command_processor_init(void) {
    ESP_LOGI(TAG, "Command Processor Initialized for MG996R on GPIO 1");
}

void command_notify_status(void) {
    if (!ble_server_is_connected()) return;

    int s1 = servo_get_angle();
    int o1 = servo_get_offset();
    bool en = servo_is_enabled();
    bool inv = joystick_get_invert();
    int brownout = (esp_reset_reason() == ESP_RST_BROWNOUT) ? 1 : 0;

    char status_str[160];
    snprintf(status_str, sizeof(status_str),
             "STATUS:s1=%d,o1=%d,en1=%d,idle=%d,run=%d,act=%d,loop=%d,bo=%d,inv=%d\n",
             s1, o1, en ? 1 : 0,
             servo_get_auto_idle() ? 1 : 0,
             servo_is_sequence_running() ? 1 : 0,
             servo_get_active_slot(),
             servo_is_sequence_looping() ? 1 : 0,
             brownout,
             inv ? 1 : 0);
    ble_server_send_notify(status_str);
}

void command_process(const char *cmd_line, command_print_fn_t print_fn) {
    if (!cmd_line || !print_fn) return;

    const char *p = cmd_line;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '\0') return;

    // Direct angle command: "s1 <0-180>", "servo <0-180>", "set <0-180>"
    if (strncmp(p, "s1 ", 3) == 0 || strncmp(p, "servo ", 6) == 0 || strncmp(p, "set ", 4) == 0) {
        const char *val_str = (strncmp(p, "s1 ", 3) == 0) ? (p + 3) : ((strncmp(p, "set ", 4) == 0) ? (p + 4) : (p + 6));
        int angle = atoi(val_str);
        if (angle >= SERVO_MIN_ANGLE && angle <= SERVO_MAX_ANGLE) {
            servo_set_angle(angle);
            char msg[32];
            snprintf(msg, sizeof(msg), "ANG:%d\n", angle);
            print_fn(msg);
        } else {
            print_fn(">> Error: Angle must be 0-180 deg.\n");
        }
        return;
    }

    // Pure number command: e.g. "90", "180"
    if (isdigit((unsigned char)p[0])) {
        int angle = atoi(p);
        if (angle >= SERVO_MIN_ANGLE && angle <= SERVO_MAX_ANGLE) {
            servo_set_angle(angle);
            char msg[32];
            snprintf(msg, sizeof(msg), "ANG:%d\n", angle);
            print_fn(msg);
            return;
        }
    }

    // Smooth movement command: "move <0-180> [duration_ms]"
    if (strncmp(p, "move ", 5) == 0) {
        int target = 90, dur = 400;
        int n = sscanf(p + 5, "%d %d", &target, &dur);
        if (n >= 1) {
            servo_move_smooth(target, dur);
            char msg[64];
            snprintf(msg, sizeof(msg), ">> Moving to %d deg over %dms\n", target, dur);
            print_fn(msg);
        } else {
            print_fn(">> Usage: move <0-180> [duration_ms]\n");
        }
        return;
    }

    // Nudge command: "nudge <+/-deg>"
    if (strncmp(p, "nudge ", 6) == 0) {
        int delta = atoi(p + 6);
        int target = servo_get_angle() + delta;
        if (target < SERVO_MIN_ANGLE) target = SERVO_MIN_ANGLE;
        if (target > SERVO_MAX_ANGLE) target = SERVO_MAX_ANGLE;
        servo_set_angle(target);
        char msg[32];
        snprintf(msg, sizeof(msg), "ANG:%d\n", target);
        print_fn(msg);
        return;
    }

    // Presets
    if (strcmp(p, "home") == 0 || strcmp(p, "center") == 0) {
        servo_move_smooth(90, 450);
        print_fn(">> Centering to 90 deg...\n");
        return;
    }
    if (strcmp(p, "min") == 0) {
        servo_move_smooth(0, 450);
        print_fn(">> Moving to 0 deg...\n");
        return;
    }
    if (strcmp(p, "max") == 0) {
        servo_move_smooth(180, 450);
        print_fn(">> Moving to 180 deg...\n");
        return;
    }

    // Stop motion
    if (strcmp(p, "stop") == 0) {
        servo_stop_motion();
        print_fn(">> Motion stopped.\n");
        return;
    }

    // Detach / Idle (cuts PWM to 0%)
    if (strcmp(p, "detach") == 0 || strcmp(p, "idle") == 0 || strcmp(p, "disable") == 0) {
        servo_detach();
        print_fn(">> Servo detached (idle, PWM=0).\n");
        return;
    }

    // Attach (re-enables PWM)
    if (strcmp(p, "attach") == 0 || strcmp(p, "enable") == 0) {
        servo_attach();
        print_fn(">> Servo attached (PWM enabled).\n");
        return;
    }

    // Auto-Idle toggle: "autoidle <on|off>"
    if (strncmp(p, "autoidle ", 9) == 0) {
        bool en = parse_bool_arg(p + 9);
        servo_set_auto_idle(en);
        char msg[32];
        snprintf(msg, sizeof(msg), "AUTO_IDLE:%d\n", en ? 1 : 0);
        print_fn(msg);
        return;
    }

    // Joystick Speed: "jspeed [30-500]"
    if (strcmp(p, "jspeed") == 0) {
        char msg[32];
        snprintf(msg, sizeof(msg), "JSPEED:%d\n", joystick_get_speed());
        print_fn(msg);
        return;
    }
    if (strncmp(p, "jspeed ", 7) == 0) {
        int spd = atoi(p + 7);
        joystick_set_speed(spd);
        char msg[48];
        snprintf(msg, sizeof(msg), ">> Joystick speed set to %d deg/s\n", joystick_get_speed());
        print_fn(msg);
        return;
    }

    // Joystick Invert: "jinvert [on|off]"
    if (strcmp(p, "jinvert") == 0) {
        char msg[32];
        snprintf(msg, sizeof(msg), "JINVERT:%d\n", joystick_get_invert() ? 1 : 0);
        print_fn(msg);
        return;
    }
    if (strncmp(p, "jinvert ", 8) == 0) {
        bool inv = parse_bool_arg(p + 8);
        joystick_set_invert(inv);
        char msg[32];
        snprintf(msg, sizeof(msg), "JINVERT:%d\n", inv ? 1 : 0);
        print_fn(msg);
        command_notify_status();
        return;
    }

    // Live Trim Offset: "offset <-45..45>" or "offset s1 <-45..45>"
    if (strncmp(p, "offset ", 7) == 0) {
        const char *arg = p + 7;
        if (strncmp(arg, "s1 ", 3) == 0) arg += 3;
        int off = atoi(arg);
        servo_set_offset(off);
        char msg[32];
        snprintf(msg, sizeof(msg), "CAL:%d\n", servo_get_offset());
        print_fn(msg);
        return;
    }

    // Save Trim Offset: "cal_set <offset>"
    if (strncmp(p, "cal_set ", 8) == 0) {
        int off = atoi(p + 8);
        servo_save_offset(off);
        char msg[32];
        snprintf(msg, sizeof(msg), "CAL:%d\n", servo_get_offset());
        print_fn(msg);
        command_notify_status();
        return;
    }

    // Get Trim Offset: "cal_get"
    if (strcmp(p, "cal_get") == 0) {
        char msg[32];
        snprintf(msg, sizeof(msg), "CAL:%d\n", servo_get_offset());
        print_fn(msg);
        return;
    }

    // Run programmable action slot: "action_run <1-8>" or "run <1-8>"
    if (strncmp(p, "action_run ", 11) == 0 || strncmp(p, "run ", 4) == 0) {
        int slot = atoi(strncmp(p, "run ", 4) == 0 ? (p + 4) : (p + 11));
        if (servo_action_run(slot, false)) {
            char msg[48];
            snprintf(msg, sizeof(msg), ">> Running action %d\n", slot);
            print_fn(msg);
        } else {
            print_fn(">> Error: Invalid action slot (1-8).\n");
        }
        return;
    }

    // Loop programmable action slot: "action_loop <1-8>" or "loop <1-8>"
    if (strncmp(p, "action_loop ", 12) == 0 || strncmp(p, "loop ", 5) == 0) {
        int slot = atoi(strncmp(p, "loop ", 5) == 0 ? (p + 5) : (p + 12));
        if (servo_action_run(slot, true)) {
            char msg[48];
            snprintf(msg, sizeof(msg), ">> Looping action %d\n", slot);
            print_fn(msg);
        } else {
            print_fn(">> Error: Invalid action slot (1-8).\n");
        }
        return;
    }

    // Save programmable action slot: "action_set <1-8> <name>|<angle,move,wait;...>"
    if (strncmp(p, "action_set ", 11) == 0) {
        const char *arg = p + 11;
        while (*arg == ' ') arg++;
        char *endptr = NULL;
        long slot = strtol(arg, &endptr, 10);
        if (slot >= 1 && slot <= ACTION_NUM_SLOTS && endptr != NULL) {
            while (*endptr == ' ' || *endptr == '\t') endptr++;
            if (servo_action_set((int)slot, endptr)) {
                char resp[ACTION_MAX_LEN + 16];
                snprintf(resp, sizeof(resp), "ACT:%ld=%s\n", slot, servo_action_get((int)slot));
                print_fn(resp);
                return;
            }
        }
        print_fn(">> Usage: action_set <1-8> <name>|<angle,move_ms,wait_ms;...>\n");
        return;
    }

    // Query all action slots: "action_get"
    if (strcmp(p, "action_get") == 0) {
        for (int i = 1; i <= ACTION_NUM_SLOTS; i++) {
            char line[ACTION_MAX_LEN + 16];
            snprintf(line, sizeof(line), "ACT:%d=%s\n", i, servo_action_get(i));
            print_fn(line);
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        return;
    }

    // Reset actions to factory defaults: "action_reset"
    if (strcmp(p, "action_reset") == 0) {
        servo_action_reset_defaults();
        for (int i = 1; i <= ACTION_NUM_SLOTS; i++) {
            char line[ACTION_MAX_LEN + 16];
            snprintf(line, sizeof(line), "ACT:%d=%s\n", i, servo_action_get(i));
            print_fn(line);
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        print_fn(">> Actions reset to defaults.\n");
        return;
    }

    // Run custom sequence string: "seq <angle,move,wait;...>" or "seq_loop <...>"
    if (strncmp(p, "seq_loop ", 9) == 0) {
        if (servo_run_sequence_str(p + 9, true)) {
            print_fn(">> Looping custom sequence...\n");
        } else {
            print_fn(">> Error: Invalid sequence format.\n");
        }
        return;
    }
    if (strncmp(p, "seq ", 4) == 0) {
        if (servo_run_sequence_str(p + 4, false)) {
            print_fn(">> Running custom sequence...\n");
        } else {
            print_fn(">> Error: Invalid sequence format.\n");
        }
        return;
    }

    // Status query
    if (strcmp(p, "status") == 0) {
        int s1 = servo_get_angle();
        int o1 = servo_get_offset();
        bool en = servo_is_enabled();
        bool inv = joystick_get_invert();
        int brownout = (esp_reset_reason() == ESP_RST_BROWNOUT) ? 1 : 0;
        char msg[160];
        snprintf(msg, sizeof(msg),
                 "STATUS:s1=%d,o1=%d,en1=%d,idle=%d,run=%d,act=%d,loop=%d,bo=%d,inv=%d\n",
                 s1, o1, en ? 1 : 0,
                 servo_get_auto_idle() ? 1 : 0,
                 servo_is_sequence_running() ? 1 : 0,
                 servo_get_active_slot(),
                 servo_is_sequence_looping() ? 1 : 0,
                 brownout,
                 inv ? 1 : 0);
        print_fn(msg);
        return;
    }

    if (strcmp(p, "help") == 0 || strcmp(p, "?") == 0) {
        print_fn("Single MG996R Servo on GPIO 1 Commands:\n"
                 "  s1 <0-180> / servo <0-180>           : Move servo to angle immediately\n"
                 "  move <0-180> [duration_ms]           : Smooth ease over duration\n"
                 "  nudge <+/-deg>                       : Adjust angle by delta\n"
                 "  home / min / max                     : Preset angles (90, 0, 180)\n"
                 "  detach / attach                      : Disable / Enable PWM\n"
                 "  autoidle <on|off>                    : Relax PWM 3s after motion\n"
                 "  offset <-45..45> / cal_set <offset>  : Adjust and save trim offset\n"
                 "  jspeed [30-500]                      : Get/Set joystick speed (deg/s)\n"
                 "  jinvert [on|off]                     : Toggle joystick direction\n"
                 "  action_run <1-8> / action_loop <1-8> : Run or loop saved sequence\n"
                 "  status                               : Print full servo telemetry\n");
        return;
    }

    if (strcmp(p, "reboot") == 0 || strcmp(p, "restart") == 0) {
        print_fn("Rebooting ESP32-S3...\n");
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_restart();
        return;
    }

    char err_msg[64];
    snprintf(err_msg, sizeof(err_msg), ">> Unknown command: '%s'. Type 'help'.\n", p);
    print_fn(err_msg);
}

void command_on_ble_received(const char *cmd, void (*reply_fn)(const char *reply)) {
    ESP_LOGI(TAG, "BLE Command: '%s'", cmd);
    command_process(cmd, reply_fn);
}