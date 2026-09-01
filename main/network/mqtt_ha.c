#include "mqtt_ha.h"

#include "audio_output.h"
#include "audio_receiver.h"
#include "calibration_dsp.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include "settings.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG "mqtt_ha"

static mqtt_ha_config_t s_config;
static mqtt_ha_status_t s_status;
static esp_mqtt_client_handle_t s_client;
static TaskHandle_t s_task;

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

static void publish_discovery(void) {
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
  cJSON *json = cJSON_CreateObject();
  cJSON_AddStringToObject(json, "profile", profile.name);
  cJSON_AddNumberToObject(json, "profile_hash", metrics.profile_hash);
  cJSON_AddNumberToObject(json, "dsp_load", metrics.dsp_load_percent);
  cJSON_AddNumberToObject(json, "limiter_gain", metrics.limiter_gain_db);
  cJSON_AddNumberToObject(json, "underruns", audio_output_get_underruns());
  cJSON_AddNumberToObject(json, "clipped_samples",
                          (double)metrics.clipped_samples);
  cJSON_AddBoolToObject(json, "playing", audio_receiver_is_playing());
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
    cal_dsp_profile_t previous;
    size_t size = sizeof(profile);
    if (settings_get_dsp_slot((uint8_t)slot_item->valueint, &profile, &size) ==
            ESP_OK &&
        size == sizeof(profile)) {
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
    publish_discovery();
    mqtt_ha_publish_now();
  } else if (event_id == MQTT_EVENT_DISCONNECTED) {
    s_status.connected = false;
  } else if (event_id == MQTT_EVENT_DATA && event && event->data &&
             event->data_len > 0) {
    apply_command(event->data, (size_t)event->data_len);
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
    vTaskDelay(pdMS_TO_TICKS(10000));
  }
}

esp_err_t mqtt_ha_init(void) {
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  snprintf(s_status.device_id, sizeof(s_status.device_id),
           "airplay_%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3],
           mac[4], mac[5]);
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
  if (!s_task)
    xTaskCreate(telemetry_task, "mqtt_telemetry", 4096, NULL, 4, &s_task);
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
