#include "mqtt_ha.h"

#include "audio_output.h"
#include "audio_receiver.h"
#include "calibration_dsp.h"
#include "cJSON.h"
#include "dacp_client.h"
#include "now_playing.h"
#include "playback_control.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include "settings.h"
#include "rtsp_events.h"
#include <stdio.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#define TAG "mqtt_ha"

static mqtt_ha_config_t s_config;
static mqtt_ha_status_t s_status;
static esp_mqtt_client_handle_t s_client;
static TaskHandle_t s_task;
static char s_availability_topic[128];

static void on_rtsp_event(rtsp_event_t event, const rtsp_event_data_t *data,
                          void *user_data) {
  (void)event;
  (void)data;
  (void)user_data;
  if (s_task)
    xTaskNotifyGive(s_task);
}

static void publish(const char *topic, const char *payload, int retain) {
  if (s_client && s_status.connected) {
    esp_mqtt_client_publish(s_client, topic, payload, 0, 1, retain);
  }
}

static void publish_discovery_entity(const char *domain, const char *key,
                                     const char *name, const char *unit,
                                     const char *device_class) {
  char topic[192];
  char state_topic[128];
  snprintf(topic, sizeof(topic), "homeassistant/%s/%s/%s/config", domain,
           s_status.device_id, key);
  snprintf(state_topic, sizeof(state_topic), "airplay_esp32/%s/state",
           s_status.device_id);
  cJSON *json = cJSON_CreateObject();
  char unique_id[64];
  snprintf(unique_id, sizeof(unique_id), "%s_%s", s_status.device_id, key);
  cJSON_AddStringToObject(json, "name", name);
  cJSON_AddStringToObject(json, "unique_id", unique_id);
  cJSON_AddStringToObject(json, "state_topic", state_topic);
  char template[96];
  if (strcmp(domain, "binary_sensor") == 0) {
    snprintf(template, sizeof(template),
             "{{ 'ON' if value_json.%s else 'OFF' }}", key);
  } else {
    snprintf(template, sizeof(template), "{{ value_json.%s }}", key);
  }
  cJSON_AddStringToObject(json, "value_template", template);
  if (unit && unit[0])
    cJSON_AddStringToObject(json, "unit_of_measurement", unit);
  if (device_class && device_class[0])
    cJSON_AddStringToObject(json, "device_class", device_class);
  cJSON *device = cJSON_AddObjectToObject(json, "device");
  cJSON *identifiers = cJSON_AddArrayToObject(device, "identifiers");
  cJSON_AddItemToArray(identifiers, cJSON_CreateString(s_status.device_id));
  cJSON_AddStringToObject(device, "name", "ESP32 AirPlay Calibrated Streamer");
  cJSON_AddStringToObject(device, "manufacturer", "DIY / rbouteiller fork");
  cJSON_AddStringToObject(device, "model", "ESP32-S3 + PCM5102A");
  char *body = cJSON_PrintUnformatted(json);
  if (body) {
    publish(topic, body, 1);
    free(body);
  }
  cJSON_Delete(json);
}

static void add_device(cJSON *json) {
  cJSON *device = cJSON_AddObjectToObject(json, "device");
  cJSON *identifiers = cJSON_AddArrayToObject(device, "identifiers");
  cJSON_AddItemToArray(identifiers, cJSON_CreateString(s_status.device_id));
  cJSON_AddStringToObject(device, "name", "ESP32 AirPlay Calibrated Streamer");
  cJSON_AddStringToObject(device, "manufacturer", "DIY / rbouteiller fork");
  cJSON_AddStringToObject(device, "model", "ESP32-S3 + PCM5102A");
}

static void publish_media_player_discovery(void) {
  char topic[192], state_topic[128], command_topic[128], volume_topic[128];
  char mute_topic[128], unique_id[64];
  snprintf(topic, sizeof(topic), "homeassistant/media_player/%s/airplay/config",
           s_status.device_id);
  snprintf(state_topic, sizeof(state_topic), "airplay_esp32/%s/state",
           s_status.device_id);
  snprintf(command_topic, sizeof(command_topic), "airplay_esp32/%s/media/set",
           s_status.device_id);
  snprintf(volume_topic, sizeof(volume_topic), "airplay_esp32/%s/volume/set",
           s_status.device_id);
  snprintf(mute_topic, sizeof(mute_topic), "airplay_esp32/%s/mute/set",
           s_status.device_id);
  snprintf(unique_id, sizeof(unique_id), "%s_media_player", s_status.device_id);

  cJSON *json = cJSON_CreateObject();
  cJSON_AddStringToObject(json, "name", "AirPlay Streamer");
  cJSON_AddStringToObject(json, "unique_id", unique_id);
  cJSON_AddStringToObject(json, "icon", "mdi:cast-audio");
  cJSON_AddStringToObject(json, "state_topic", state_topic);
  cJSON_AddStringToObject(json, "value_template",
                          "{{ value_json.media_state }}");
  cJSON_AddStringToObject(json, "command_topic", command_topic);
  cJSON_AddStringToObject(json, "volume_state_topic", state_topic);
  cJSON_AddStringToObject(json, "volume_value_template",
                          "{{ value_json.volume_level }}");
  cJSON_AddStringToObject(json, "volume_command_topic", volume_topic);
  cJSON_AddStringToObject(json, "mute_state_topic", state_topic);
  cJSON_AddStringToObject(json, "mute_value_template",
                          "{{ value_json.is_volume_muted }}");
  cJSON_AddStringToObject(json, "mute_command_topic", mute_topic);
  cJSON_AddStringToObject(json, "payload_mute_on", "ON");
  cJSON_AddStringToObject(json, "payload_mute_off", "OFF");
  cJSON_AddStringToObject(json, "json_attributes_topic", state_topic);
  cJSON_AddStringToObject(json, "availability_topic", s_availability_topic);
  add_device(json);
  char *body = cJSON_PrintUnformatted(json);
  if (body) {
    publish(topic, body, 1);
    free(body);
  }
  cJSON_Delete(json);
}

static void publish_discovery(void) {
  publish_media_player_discovery();
  publish_discovery_entity("sensor", "profile", "DSP profile", "", "");
  publish_discovery_entity("sensor", "dsp_load", "DSP load", "%", "");
  publish_discovery_entity("sensor", "limiter_gain", "Limiter reduction", "dB",
                           "");
  publish_discovery_entity("sensor", "underruns", "Output underruns", "", "");
  publish_discovery_entity("sensor", "clipped_samples", "Clipped samples", "",
                           "");
  publish_discovery_entity("binary_sensor", "playing", "AirPlay playing", "",
                           "");
  publish_discovery_entity("binary_sensor", "dsp_bypassed", "DSP bypassed", "",
                           "");
  publish_discovery_entity("binary_sensor", "measurement_mode",
                           "Measurement mode", "", "");
}

void mqtt_ha_publish_now(void) {
  if (!s_status.connected)
    return;
  cal_dsp_profile_t profile;
  cal_dsp_metrics_t metrics = {0};
  calibration_dsp_get_profile(&profile);
  calibration_dsp_get_metrics(&metrics);
  now_playing_snapshot_t now;
  now_playing_get_snapshot(&now);
  uint32_t media_position = now.metadata.position_secs;
  if (now.playing && now.updated_at_us > 0) {
    int64_t elapsed = (esp_timer_get_time() - now.updated_at_us) / 1000000;
    if (elapsed > 0 && elapsed < 86400)
      media_position += (uint32_t)elapsed;
  }
  if (now.metadata.duration_secs && media_position > now.metadata.duration_secs)
    media_position = now.metadata.duration_secs;
  int volume_percent = playback_control_get_volume_percent();
  bool muted = playback_control_is_muted();
  cJSON *json = cJSON_CreateObject();
  cJSON_AddStringToObject(json, "profile", profile.name);
  cJSON_AddNumberToObject(json, "profile_hash", metrics.profile_hash);
  cJSON_AddNumberToObject(json, "dsp_load", metrics.dsp_load_percent);
  cJSON_AddNumberToObject(json, "limiter_gain", metrics.limiter_gain_db);
  cJSON_AddNumberToObject(json, "underruns", audio_output_get_underruns());
  cJSON_AddNumberToObject(json, "clipped_samples",
                          (double)metrics.clipped_samples);
  cJSON_AddBoolToObject(json, "playing", now.playing);
  cJSON_AddStringToObject(json, "media_state",
                          !now.connected            ? "idle"
                          : (muted || !now.playing) ? "paused"
                                                    : "playing");
  cJSON_AddNumberToObject(json, "volume_level", (double)volume_percent / 100.0);
  cJSON_AddBoolToObject(json, "is_volume_muted", muted);
  cJSON_AddStringToObject(json, "media_title", now.metadata.title);
  cJSON_AddStringToObject(json, "media_artist", now.metadata.artist);
  cJSON_AddStringToObject(json, "media_album_name", now.metadata.album);
  cJSON_AddNumberToObject(json, "media_duration", now.metadata.duration_secs);
  cJSON_AddNumberToObject(json, "media_position", media_position);
  cJSON_AddStringToObject(json, "source", "AirPlay");
  cJSON_AddStringToObject(json, "sender", now.sender);
  if (now.artwork_size > 0) {
    char device_name[65], hostname[33], artwork_url[160];
    settings_get_device_name(device_name, sizeof(device_name));
    settings_device_name_to_hostname(device_name, hostname, sizeof(hostname));
    snprintf(artwork_url, sizeof(artwork_url),
             "http://%s.local/api/now-playing/artwork?v=%" PRIu32, hostname,
             now.artwork_revision);
    cJSON_AddStringToObject(json, "entity_picture", artwork_url);
  }
  cJSON_AddBoolToObject(json, "dsp_bypassed", metrics.bypassed);
  cJSON_AddBoolToObject(json, "measurement_mode", metrics.measurement_mode);
  cJSON_AddNumberToObject(json, "measurement_session_id",
                          metrics.measurement_session_id);
  char *body = cJSON_PrintUnformatted(json);
  if (body) {
    char topic[128];
    snprintf(topic, sizeof(topic), "airplay_esp32/%s/state",
             s_status.device_id);
    publish(topic, body, 1);
    free(body);
  }
  cJSON_Delete(json);
}

static bool topic_matches(const esp_mqtt_event_handle_t event,
                          const char *expected) {
  size_t length = strlen(expected);
  return event && event->topic && event->topic_len == (int)length &&
         memcmp(event->topic, expected, length) == 0;
}

static bool payload_equals(const char *payload, size_t length,
                           const char *expected) {
  size_t expected_length = strlen(expected);
  return length == expected_length &&
         memcmp(payload, expected, expected_length) == 0;
}

static void apply_media_command(const char *payload, size_t length) {
  if (payload_equals(payload, length, "PLAY") ||
      payload_equals(payload, length, "ON")) {
    if (playback_control_is_muted())
      playback_control_set_muted(false);
    else if (!audio_receiver_is_playing() && dacp_is_active())
      playback_control_play_pause();
  } else if (payload_equals(payload, length, "PAUSE") ||
             payload_equals(payload, length, "STOP") ||
             payload_equals(payload, length, "OFF")) {
    if (!playback_control_is_muted() && audio_receiver_is_playing()) {
      if (dacp_is_active())
        playback_control_play_pause();
      else
        playback_control_set_muted(true);
    }
  } else if (payload_equals(payload, length, "NEXT")) {
    playback_control_next();
  } else if (payload_equals(payload, length, "PREVIOUS")) {
    playback_control_prev();
  }
}

static void apply_volume_command(const char *payload, size_t length) {
  char value[24];
  if (length == 0 || length >= sizeof(value))
    return;
  memcpy(value, payload, length);
  value[length] = '\0';
  char *end = NULL;
  float volume = strtof(value, &end);
  if (end == value || volume < 0.0f || volume > 1.0f)
    return;
  playback_control_set_volume_percent((int)(volume * 100.0f + 0.5f));
}

static void apply_mute_command(const char *payload, size_t length) {
  if (payload_equals(payload, length, "ON"))
    playback_control_set_muted(true);
  else if (payload_equals(payload, length, "OFF"))
    playback_control_set_muted(false);
}

static void apply_command(const char *payload, size_t length) {
  char *copy = malloc(length + 1U);
  if (!copy)
    return;
  memcpy(copy, payload, length);
  copy[length] = '\0';
  cJSON *json = cJSON_Parse(copy);
  free(copy);
  if (!json)
    return;
  cJSON *bypass = cJSON_GetObjectItemCaseSensitive(json, "bypass");
  if (cJSON_IsBool(bypass))
    calibration_dsp_set_bypass(cJSON_IsTrue(bypass));
  cJSON *measurement =
      cJSON_GetObjectItemCaseSensitive(json, "measurement_mode");
  if (cJSON_IsBool(measurement)) {
    cal_dsp_metrics_t metrics = {0};
    calibration_dsp_get_metrics(&metrics);
    calibration_dsp_set_measurement_mode(cJSON_IsTrue(measurement), 3277,
                                         metrics.profile_hash);
  }
  cJSON *slot_item = cJSON_GetObjectItemCaseSensitive(json, "profile_slot");
  if (cJSON_IsNumber(slot_item) && slot_item->valueint >= 0 &&
      slot_item->valueint < SETTINGS_DSP_PROFILE_SLOTS) {
    cal_dsp_profile_t profile;
    memset(&profile, 0, sizeof(profile));
    cal_dsp_profile_t previous;
    size_t size = sizeof(profile);
    if (settings_get_dsp_slot((uint8_t)slot_item->valueint, &profile, &size) ==
            ESP_OK &&
        calibration_dsp_upgrade_profile(&profile, size,
                                        CONFIG_OUTPUT_SAMPLE_RATE_HZ)) {
      calibration_dsp_get_profile(&previous);
      if (settings_set_dsp_backup(&previous, sizeof(previous)) == ESP_OK &&
          calibration_dsp_set_profile(&profile) == ESP_OK) {
        settings_set_dsp_profile(&profile, sizeof(profile));
      }
    }
  }
  cJSON_Delete(json);
  mqtt_ha_publish_now();
}

static void mqtt_event(void *args, esp_event_base_t base, int32_t event_id,
                       void *event_data) {
  (void)args;
  (void)base;
  esp_mqtt_event_handle_t event = event_data;
  if (event_id == MQTT_EVENT_CONNECTED) {
    s_status.connected = true;
    s_status.last_error = 0;
    char command_topic[128];
    snprintf(command_topic, sizeof(command_topic), "airplay_esp32/%s/set",
             s_status.device_id);
    esp_mqtt_client_subscribe(s_client, command_topic, 1);
    char media_topic[128], volume_topic[128], mute_topic[128];
    snprintf(media_topic, sizeof(media_topic), "airplay_esp32/%s/media/set",
             s_status.device_id);
    snprintf(volume_topic, sizeof(volume_topic), "airplay_esp32/%s/volume/set",
             s_status.device_id);
    snprintf(mute_topic, sizeof(mute_topic), "airplay_esp32/%s/mute/set",
             s_status.device_id);
    esp_mqtt_client_subscribe(s_client, media_topic, 1);
    esp_mqtt_client_subscribe(s_client, volume_topic, 1);
    esp_mqtt_client_subscribe(s_client, mute_topic, 1);
    publish(s_availability_topic, "online", 1);
    publish_discovery();
    mqtt_ha_publish_now();
  } else if (event_id == MQTT_EVENT_DISCONNECTED) {
    s_status.connected = false;
  } else if (event_id == MQTT_EVENT_DATA && event && event->data &&
             event->data_len > 0) {
    char topic[128];
    snprintf(topic, sizeof(topic), "airplay_esp32/%s/media/set",
             s_status.device_id);
    if (topic_matches(event, topic)) {
      apply_media_command(event->data, (size_t)event->data_len);
    } else {
      snprintf(topic, sizeof(topic), "airplay_esp32/%s/volume/set",
               s_status.device_id);
      if (topic_matches(event, topic))
        apply_volume_command(event->data, (size_t)event->data_len);
      else {
        snprintf(topic, sizeof(topic), "airplay_esp32/%s/mute/set",
                 s_status.device_id);
        if (topic_matches(event, topic))
          apply_mute_command(event->data, (size_t)event->data_len);
        else
          apply_command(event->data, (size_t)event->data_len);
      }
    }
    mqtt_ha_publish_now();
  } else if (event_id == MQTT_EVENT_ERROR) {
    s_status.last_error =
        event && event->error_handle ? event->error_handle->error_type : -1;
  }
}

static esp_err_t start_client(void) {
  if (s_client) {
    esp_mqtt_client_stop(s_client);
    esp_mqtt_client_destroy(s_client);
    s_client = NULL;
  }
  s_status.connected = false;
  if (!s_config.enabled || s_config.broker_uri[0] == '\0')
    return ESP_OK;
  esp_mqtt_client_config_t config = {
      .broker.address.uri = s_config.broker_uri,
      .credentials.username = s_config.username[0] ? s_config.username : NULL,
      .credentials.authentication.password =
          s_config.password[0] ? s_config.password : NULL,
      .session.keepalive = 30,
      .session.last_will.topic = s_availability_topic,
      .session.last_will.msg = "offline",
      .session.last_will.qos = 1,
      .session.last_will.retain = 1,
  };
  s_client = esp_mqtt_client_init(&config);
  if (!s_client)
    return ESP_ERR_NO_MEM;
  esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event, NULL);
  return esp_mqtt_client_start(s_client);
}

static void telemetry_task(void *arg) {
  (void)arg;
  while (true) {
    mqtt_ha_publish_now();
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10000));
  }
}

esp_err_t mqtt_ha_init(void) {
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  snprintf(s_status.device_id, sizeof(s_status.device_id),
           "airplay_%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3],
           mac[4], mac[5]);
  snprintf(s_availability_topic, sizeof(s_availability_topic),
           "airplay_esp32/%s/availability", s_status.device_id);
  size_t size = sizeof(s_config);
  if (settings_get_mqtt_config(&s_config, &size) == ESP_OK &&
      size == sizeof(s_config) && s_config.version == MQTT_HA_CONFIG_VERSION) {
    s_status.configured = s_config.broker_uri[0] != '\0';
    s_status.enabled = s_config.enabled;
    s_status.password_set = s_config.password[0] != '\0';
    snprintf(s_status.broker_uri, sizeof(s_status.broker_uri), "%s",
             s_config.broker_uri);
    snprintf(s_status.username, sizeof(s_status.username), "%s",
             s_config.username);
  } else {
    memset(&s_config, 0, sizeof(s_config));
    s_config.version = MQTT_HA_CONFIG_VERSION;
  }
  esp_err_t err = start_client();
  if (!s_task) {
    xTaskCreate(telemetry_task, "mqtt_telemetry", 4096, NULL, 4, &s_task);
    rtsp_events_register(on_rtsp_event, NULL);
  }
  return err;
}

esp_err_t mqtt_ha_set_config(const mqtt_ha_config_t *config) {
  if (!config || config->version != MQTT_HA_CONFIG_VERSION ||
      (config->enabled && strncmp(config->broker_uri, "mqtt://", 7) != 0)) {
    return ESP_ERR_INVALID_ARG;
  }
  s_config = *config;
  esp_err_t err = settings_set_mqtt_config(&s_config, sizeof(s_config));
  if (err != ESP_OK)
    return err;
  s_status.configured = s_config.broker_uri[0] != '\0';
  s_status.enabled = s_config.enabled;
  s_status.password_set = s_config.password[0] != '\0';
  snprintf(s_status.broker_uri, sizeof(s_status.broker_uri), "%s",
           s_config.broker_uri);
  snprintf(s_status.username, sizeof(s_status.username), "%s",
           s_config.username);
  return start_client();
}

void mqtt_ha_get_status(mqtt_ha_status_t *status) {
  if (status)
    *status = s_status;
}

void mqtt_ha_get_config(mqtt_ha_config_t *config) {
  if (config)
    *config = s_config;
}
