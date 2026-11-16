#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*command_print_fn_t)(const char *str);

void command_processor_init(void);
void command_process(const char *cmd_line, command_print_fn_t print_fn);
void command_notify_status(void);
void command_on_ble_received(const char *cmd, void (*reply_fn)(const char *reply));

#ifdef __cplusplus
}
#endif