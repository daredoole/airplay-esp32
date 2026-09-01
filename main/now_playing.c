#include "now_playing.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG               "now_playing"
#define ARTWORK_MAX_BYTES (192U * 1024U)

static SemaphoreHandle_t s_lock;
static now_playing_snapshot_t s_snapshot;
static uint8_t *s_artwork;

static void clear_artwork_locked(void) {
  free(s_artwork);
  s_artwork = NULL;
  s_snapshot.artwork_size = 0;
  s_snapshot.artwork_mime[0] = '\0';
  s_snapshot.metadata.has_artwork = false;
  s_snapshot.artwork_revision++;
}

static void cache_artwork_locked(const rtsp_metadata_t *metadata) {
  if (!metadata->artwork_data || metadata->artwork_size == 0 ||
      metadata->artwork_size > ARTWORK_MAX_BYTES) {
    ESP_LOGW(TAG, "Artwork rejected: %u bytes (limit %u)",
             (unsigned)metadata->artwork_size, (unsigned)ARTWORK_MAX_BYTES);
    return;
  }
  uint8_t *copy = heap_caps_malloc(metadata->artwork_size,
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!copy) {
    ESP_LOGW(TAG, "Artwork dropped: PSRAM allocation failed");
    return;
  }
  memcpy(copy, metadata->artwork_data, metadata->artwork_size);
  free(s_artwork);
  s_artwork = copy;
  s_snapshot.artwork_size = metadata->artwork_size;
  strlcpy(s_snapshot.artwork_mime,
          metadata->artwork_mime ? metadata->artwork_mime
                                 : "application/octet-stream",
          sizeof(s_snapshot.artwork_mime));
  s_snapshot.metadata.has_artwork = true;
  s_snapshot.artwork_revision++;
}

static void on_rtsp_event(rtsp_event_t event, const rtsp_event_data_t *data,
                          void *user_data) {
  (void)user_data;
  if (!s_lock)
    return;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  if (event == RTSP_EVENT_CLIENT_CONNECTED) {
    s_snapshot.connected = true;
  } else if (event == RTSP_EVENT_PLAYING) {
    s_snapshot.connected = true;
    s_snapshot.playing = true;
  } else if (event == RTSP_EVENT_PAUSED) {
    s_snapshot.playing = false;
  } else if (event == RTSP_EVENT_DISCONNECTED) {
    bool had_artwork = s_artwork != NULL;
    memset(&s_snapshot, 0, sizeof(s_snapshot));
    if (had_artwork)
      clear_artwork_locked();
  } else if (event == RTSP_EVENT_METADATA && data) {
    const rtsp_metadata_t *metadata = &data->metadata;
    bool new_track = metadata->title[0] &&
                     strncmp(metadata->title, s_snapshot.metadata.title,
                             METADATA_STRING_MAX) != 0;
    if (new_track) {
      memset(&s_snapshot.metadata, 0, sizeof(s_snapshot.metadata));
      clear_artwork_locked();
    }
    if (metadata->title[0])
      snprintf(s_snapshot.metadata.title, sizeof(s_snapshot.metadata.title),
               "%s", metadata->title);
    if (metadata->artist[0])
      snprintf(s_snapshot.metadata.artist, sizeof(s_snapshot.metadata.artist),
               "%s", metadata->artist);
    if (metadata->album[0])
      snprintf(s_snapshot.metadata.album, sizeof(s_snapshot.metadata.album),
               "%s", metadata->album);
    if (metadata->genre[0])
      snprintf(s_snapshot.metadata.genre, sizeof(s_snapshot.metadata.genre),
               "%s", metadata->genre);
    if (metadata->duration_secs)
      s_snapshot.metadata.duration_secs = metadata->duration_secs;
    if (metadata->position_secs || metadata->duration_secs)
      s_snapshot.metadata.position_secs = metadata->position_secs;
    if (metadata->artwork_data)
      cache_artwork_locked(metadata);
  }
  s_snapshot.updated_at_us = esp_timer_get_time();
  xSemaphoreGive(s_lock);
}

esp_err_t now_playing_init(void) {
  if (s_lock)
    return ESP_OK;
  s_lock = xSemaphoreCreateMutex();
  if (!s_lock)
    return ESP_ERR_NO_MEM;
  memset(&s_snapshot, 0, sizeof(s_snapshot));
  if (rtsp_events_register(on_rtsp_event, NULL) != 0) {
    vSemaphoreDelete(s_lock);
    s_lock = NULL;
    return ESP_ERR_NO_MEM;
  }
  return ESP_OK;
}

void now_playing_set_sender(const char *sender, uint8_t protocol_version) {
  if (!s_lock)
    return;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  if (sender && sender[0])
    snprintf(s_snapshot.sender, sizeof(s_snapshot.sender), "%s", sender);
  s_snapshot.protocol_version = protocol_version;
  xSemaphoreGive(s_lock);
}

void now_playing_get_snapshot(now_playing_snapshot_t *snapshot) {
  if (!snapshot)
    return;
  memset(snapshot, 0, sizeof(*snapshot));
  if (!s_lock)
    return;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  *snapshot = s_snapshot;
  xSemaphoreGive(s_lock);
}

esp_err_t now_playing_copy_artwork(uint8_t **data, size_t *size, char *mime,
                                   size_t mime_size, uint32_t *revision) {
  if (!data || !size || !s_lock)
    return ESP_ERR_INVALID_ARG;
  *data = NULL;
  *size = 0;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  if (!s_artwork || s_snapshot.artwork_size == 0) {
    xSemaphoreGive(s_lock);
    return ESP_ERR_NOT_FOUND;
  }
  uint8_t *copy = heap_caps_malloc(s_snapshot.artwork_size,
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!copy) {
    xSemaphoreGive(s_lock);
    return ESP_ERR_NO_MEM;
  }
  memcpy(copy, s_artwork, s_snapshot.artwork_size);
  *data = copy;
  *size = s_snapshot.artwork_size;
  if (mime && mime_size)
    snprintf(mime, mime_size, "%s", s_snapshot.artwork_mime);
  if (revision)
    *revision = s_snapshot.artwork_revision;
  xSemaphoreGive(s_lock);
  return ESP_OK;
}
