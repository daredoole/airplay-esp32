#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MQTT_HA_CONFIG_VERSION 1U

typedef struct {
  uint32_t version;
  bool enabled;
  char broker_uri[128];
  char username[64];
  char password[64];
} mqtt_ha_config_t;

typedef struct {
  bool configured;
  bool enabled;
  bool connected;
  bool password_set;
  char broker_uri[128];
  char username[64];
  char device_id[32];
  int last_error;
} mqtt_ha_status_t;

esp_err_t mqtt_ha_init(void);
esp_err_t mqtt_ha_set_config(const mqtt_ha_config_t *config);
void mqtt_ha_get_config(mqtt_ha_config_t *config);
void mqtt_ha_get_status(mqtt_ha_status_t *status);
void mqtt_ha_publish_now(void);
