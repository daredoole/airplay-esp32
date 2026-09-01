#pragma once

#include "esp_err.h"
#include "esp_system.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DIAGNOSTICS_EVENT_COUNT 16U
#define DIAGNOSTICS_EVENT_LEN   48U

typedef struct {
  uint32_t boot_count;
  uint32_t crash_count;
  esp_reset_reason_t reset_reason;
  bool ota_pending_verify;
  bool ota_marked_valid;
  uint32_t rtc_event_count;
  char events[DIAGNOSTICS_EVENT_COUNT][DIAGNOSTICS_EVENT_LEN];
} diagnostics_snapshot_t;

esp_err_t diagnostics_init(void);
void diagnostics_record_event(const char *event);
void diagnostics_get_snapshot(diagnostics_snapshot_t *snapshot);

/** Mark a pending OTA image valid after the application health gate passes. */
esp_err_t diagnostics_mark_boot_healthy(void);
