#include "diagnostics.h"

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "nvs.h"
#include <stdio.h>
#include <string.h>

#define TAG            "diagnostics"
#define DIAG_NAMESPACE "airplay_diag"
#define DIAG_RTC_MAGIC 0x41554449U

typedef struct {
  uint32_t magic;
  uint32_t write_index;
  uint32_t count;
  char events[DIAGNOSTICS_EVENT_COUNT][DIAGNOSTICS_EVENT_LEN];
} diagnostics_rtc_t;

RTC_NOINIT_ATTR static diagnostics_rtc_t s_rtc;
static diagnostics_snapshot_t s_snapshot;

static bool is_crash_reset(esp_reset_reason_t reason) {
  return reason == ESP_RST_PANIC || reason == ESP_RST_INT_WDT ||
         reason == ESP_RST_TASK_WDT || reason == ESP_RST_WDT ||
         reason == ESP_RST_BROWNOUT;
}

void diagnostics_record_event(const char *event) {
  if (!event) {
    return;
  }
  if (s_rtc.magic != DIAG_RTC_MAGIC) {
    memset(&s_rtc, 0, sizeof(s_rtc));
    s_rtc.magic = DIAG_RTC_MAGIC;
  }
  uint32_t slot = s_rtc.write_index++ % DIAGNOSTICS_EVENT_COUNT;
  snprintf(s_rtc.events[slot], DIAGNOSTICS_EVENT_LEN, "%lldms %s",
           (long long)(esp_timer_get_time() / 1000LL), event);
  if (s_rtc.count < DIAGNOSTICS_EVENT_COUNT) {
    s_rtc.count++;
  }
}

esp_err_t diagnostics_init(void) {
  memset(&s_snapshot, 0, sizeof(s_snapshot));
  s_snapshot.reset_reason = esp_reset_reason();
  if (s_rtc.magic != DIAG_RTC_MAGIC) {
    memset(&s_rtc, 0, sizeof(s_rtc));
    s_rtc.magic = DIAG_RTC_MAGIC;
  }

  nvs_handle_t nvs;
  esp_err_t err = nvs_open(DIAG_NAMESPACE, NVS_READWRITE, &nvs);
  if (err == ESP_OK) {
    nvs_get_u32(nvs, "boots", &s_snapshot.boot_count);
    nvs_get_u32(nvs, "crashes", &s_snapshot.crash_count);
    s_snapshot.boot_count++;
    if (is_crash_reset(s_snapshot.reset_reason)) {
      s_snapshot.crash_count++;
    }
    nvs_set_u32(nvs, "boots", s_snapshot.boot_count);
    nvs_set_u32(nvs, "crashes", s_snapshot.crash_count);
    nvs_commit(nvs);
    nvs_close(nvs);
  }

  const esp_partition_t *running = esp_ota_get_running_partition();
  esp_ota_img_states_t state;
  if (running && esp_ota_get_state_partition(running, &state) == ESP_OK) {
    s_snapshot.ota_pending_verify = state == ESP_OTA_IMG_PENDING_VERIFY;
  }
  char event[DIAGNOSTICS_EVENT_LEN];
  snprintf(event, sizeof(event), "boot reset=%d", s_snapshot.reset_reason);
  diagnostics_record_event(event);
  ESP_LOGI(TAG, "boot=%lu crashes=%lu reset=%d ota_pending=%d",
           (unsigned long)s_snapshot.boot_count,
           (unsigned long)s_snapshot.crash_count, s_snapshot.reset_reason,
           s_snapshot.ota_pending_verify);
  return err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err;
}

esp_err_t diagnostics_mark_boot_healthy(void) {
  if (!s_snapshot.ota_pending_verify) {
    s_snapshot.ota_marked_valid = true;
    return ESP_OK;
  }
  esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
  if (err == ESP_OK) {
    s_snapshot.ota_pending_verify = false;
    s_snapshot.ota_marked_valid = true;
    diagnostics_record_event("OTA image marked valid");
  }
  return err;
}

void diagnostics_get_snapshot(diagnostics_snapshot_t *snapshot) {
  if (!snapshot) {
    return;
  }
  *snapshot = s_snapshot;
  snapshot->rtc_event_count = s_rtc.count;
  uint32_t first = s_rtc.count == DIAGNOSTICS_EVENT_COUNT
                       ? s_rtc.write_index % DIAGNOSTICS_EVENT_COUNT
                       : 0;
  for (uint32_t i = 0; i < s_rtc.count; i++) {
    uint32_t source = (first + i) % DIAGNOSTICS_EVENT_COUNT;
    memcpy(snapshot->events[i], s_rtc.events[source], DIAGNOSTICS_EVENT_LEN);
  }
}
