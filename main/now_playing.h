#pragma once

#include "esp_err.h"
#include "rtsp_events.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NOW_PLAYING_SENDER_MAX 64
#define NOW_PLAYING_MIME_MAX   32

typedef struct {
  rtsp_metadata_t metadata;
  char sender[NOW_PLAYING_SENDER_MAX];
  char artwork_mime[NOW_PLAYING_MIME_MAX];
  uint32_t artwork_revision;
  size_t artwork_size;
  int64_t updated_at_us;
  uint8_t protocol_version;
  bool connected;
  bool playing;
} now_playing_snapshot_t;

esp_err_t now_playing_init(void);
void now_playing_set_sender(const char *sender, uint8_t protocol_version);
void now_playing_get_snapshot(now_playing_snapshot_t *snapshot);

/** Returns a PSRAM copy owned by the caller, or ESP_ERR_NOT_FOUND. */
esp_err_t now_playing_copy_artwork(uint8_t **data, size_t *size, char *mime,
                                   size_t mime_size, uint32_t *revision);
