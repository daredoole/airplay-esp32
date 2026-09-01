#include "dsp_api.h"

#include "audio_output.h"
#include "audio_receiver.h"
#include "api_security.h"
#include "calibration_dsp.h"
#include "cJSON.h"
#include "diagnostics.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "ptp_clock.h"
#include "nvs.h"
#ifdef CONFIG_MQTT_HA_ENABLED
#include "mqtt_ha.h"
#endif
#include "settings.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define TAG              "dsp_api"
#define DSP_API_MAX_BODY 8192U

static esp_err_t send_json(httpd_req_t *req, cJSON *json) {
  char *body = cJSON_PrintUnformatted(json);
  if (!body) {
    cJSON_Delete(json);
    httpd_resp_send_500(req);
    return ESP_ERR_NO_MEM;
  }
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  esp_err_t err = httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
  free(body);
  cJSON_Delete(json);
  return err;
}

static esp_err_t send_error(httpd_req_t *req, const char *status,
                            const char *message) {
  cJSON *json = cJSON_CreateObject();
  cJSON_AddBoolToObject(json, "success", false);
  cJSON_AddStringToObject(json, "error", message);
  httpd_resp_set_status(req, status);
  return send_json(req, json);
}

static char *read_body(httpd_req_t *req) {
  if (req->content_len <= 0 || req->content_len > DSP_API_MAX_BODY) {
    return NULL;
  }
  char *body = malloc((size_t)req->content_len + 1U);
  if (!body) {
    return NULL;
  }
  size_t received = 0;
  while (received < (size_t)req->content_len) {
    int result = httpd_req_recv(req, body + received,
                                (size_t)req->content_len - received);
    if (result == HTTPD_SOCK_ERR_TIMEOUT) {
      continue;
    }
    if (result <= 0) {
      free(body);
      return NULL;
    }
    received += (size_t)result;
  }
  body[received] = '\0';
  return body;
}

static void filter_to_json(cJSON *array, const cal_dsp_filter_t *filter) {
  cJSON *item = cJSON_CreateObject();
  cJSON_AddBoolToObject(item, "enabled", filter->enabled);
  cJSON_AddStringToObject(item, "type",
                          calibration_dsp_filter_type_name(filter->type));
  cJSON_AddNumberToObject(item, "freq", filter->frequency_hz);
  cJSON_AddNumberToObject(item, "gain", filter->gain_db);
  cJSON_AddNumberToObject(item, "q", filter->q);
  cJSON_AddItemToArray(array, item);
}

static void channel_to_json(cJSON *root, const char *name,
                            const cal_dsp_channel_t *channel) {
  cJSON *json = cJSON_AddObjectToObject(root, name);
  cJSON_AddNumberToObject(json, "gain_db", channel->gain_db);
  cJSON_AddNumberToObject(json, "delay_ms", channel->delay_ms);
  cJSON_AddNumberToObject(json, "polarity", channel->polarity);
  cJSON *filters = cJSON_AddArrayToObject(json, "filters");
  for (size_t i = 0; i < channel->filter_count; i++) {
    filter_to_json(filters, &channel->filters[i]);
  }
}

static cJSON *profile_to_json(const cal_dsp_profile_t *profile) {
  cJSON *json = cJSON_CreateObject();
  cJSON_AddBoolToObject(json, "success", true);
  cJSON_AddNumberToObject(json, "version", profile->version);
  cJSON_AddNumberToObject(json, "sample_rate", profile->sample_rate);
  cJSON_AddStringToObject(json, "name", profile->name);
  cJSON_AddBoolToObject(json, "enabled", profile->enabled);
  cJSON_AddNumberToObject(json, "preamp_db", profile->requested_preamp_db);
  cJSON *headroom = cJSON_AddObjectToObject(json, "headroom");
  cJSON_AddBoolToObject(headroom, "auto", profile->auto_headroom);
  cJSON_AddNumberToObject(headroom, "margin_db", profile->headroom_margin_db);
  channel_to_json(json, "left", &profile->channels[0]);
  channel_to_json(json, "right", &profile->channels[1]);
  cJSON *limiter = cJSON_AddObjectToObject(json, "limiter");
  cJSON_AddBoolToObject(limiter, "enabled", profile->limiter.enabled);
  cJSON_AddNumberToObject(limiter, "ceiling_dbfs",
                          profile->limiter.ceiling_dbfs);
  cJSON_AddNumberToObject(limiter, "lookahead_ms",
                          profile->limiter.lookahead_ms);
  cJSON_AddNumberToObject(limiter, "release_ms", profile->limiter.release_ms);
  cJSON *loudness = cJSON_AddObjectToObject(json, "loudness");
  cJSON_AddBoolToObject(loudness, "enabled", profile->loudness.enabled);
  cJSON_AddNumberToObject(loudness, "max_bass_db",
                          profile->loudness.max_bass_db);
  cJSON_AddNumberToObject(loudness, "max_treble_db",
                          profile->loudness.max_treble_db);
  cJSON_AddNumberToObject(loudness, "full_effect_below_db",
                          profile->loudness.full_effect_below_db);

  cal_dsp_metrics_t metrics = {0};
  calibration_dsp_get_metrics(&metrics);
  cJSON_AddNumberToObject(json, "effective_preamp_db",
                          metrics.effective_preamp_db);
  cJSON_AddNumberToObject(json, "cascade_peak_db", metrics.cascade_peak_db);
  cJSON_AddNumberToObject(json, "profile_hash", metrics.profile_hash);
  cJSON_AddBoolToObject(json, "bypassed", metrics.bypassed);
  return json;
}

static bool json_number(cJSON *object, const char *name, float *value) {
  cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
  if (!item) {
    return true;
  }
  if (!cJSON_IsNumber(item)) {
    return false;
  }
  *value = (float)item->valuedouble;
  return true;
}

static bool parse_channel(cJSON *json, cal_dsp_channel_t *channel) {
  if (!json || !cJSON_IsObject(json)) {
    return false;
  }
  if (!json_number(json, "gain_db", &channel->gain_db) ||
      !json_number(json, "delay_ms", &channel->delay_ms)) {
    return false;
  }
  cJSON *polarity = cJSON_GetObjectItemCaseSensitive(json, "polarity");
  if (polarity) {
    if (!cJSON_IsNumber(polarity)) {
      return false;
    }
    channel->polarity = (int8_t)polarity->valueint;
  }
  cJSON *filters = cJSON_GetObjectItemCaseSensitive(json, "filters");
  if (!filters) {
    return true;
  }
  if (!cJSON_IsArray(filters) ||
      cJSON_GetArraySize(filters) > CAL_DSP_MAX_FILTERS) {
    return false;
  }
  channel->filter_count = (uint8_t)cJSON_GetArraySize(filters);
  for (size_t i = 0; i < channel->filter_count; i++) {
    cJSON *item = cJSON_GetArrayItem(filters, (int)i);
    cJSON *type = cJSON_GetObjectItemCaseSensitive(item, "type");
    cJSON *frequency = cJSON_GetObjectItemCaseSensitive(item, "freq");
    cJSON *gain = cJSON_GetObjectItemCaseSensitive(item, "gain");
    cJSON *q = cJSON_GetObjectItemCaseSensitive(item, "q");
    if (!cJSON_IsObject(item) || !cJSON_IsString(type) ||
        !cJSON_IsNumber(frequency) || !cJSON_IsNumber(q) ||
        !calibration_dsp_filter_type_parse(type->valuestring,
                                           &channel->filters[i].type)) {
      return false;
    }
    channel->filters[i].enabled = true;
    cJSON *enabled = cJSON_GetObjectItemCaseSensitive(item, "enabled");
    if (enabled && cJSON_IsBool(enabled)) {
      channel->filters[i].enabled = cJSON_IsTrue(enabled);
    }
    channel->filters[i].frequency_hz = (float)frequency->valuedouble;
    channel->filters[i].gain_db =
        cJSON_IsNumber(gain) ? (float)gain->valuedouble : 0.0f;
    channel->filters[i].q = (float)q->valuedouble;
  }
  return true;
}

static bool parse_profile(cJSON *json, cal_dsp_profile_t *profile) {
  if (!json || !cJSON_IsObject(json)) {
    return false;
  }
  cal_dsp_profile_t current;
  calibration_dsp_get_profile(&current);
  calibration_dsp_default_profile(profile, current.sample_rate);
  profile->enabled = true;

  cJSON *version = cJSON_GetObjectItemCaseSensitive(json, "version");
  cJSON *sample_rate = cJSON_GetObjectItemCaseSensitive(json, "sample_rate");
  cJSON *name = cJSON_GetObjectItemCaseSensitive(json, "name");
  cJSON *enabled = cJSON_GetObjectItemCaseSensitive(json, "enabled");
  if (version && cJSON_IsNumber(version)) {
    profile->version = (uint32_t)version->valueint;
  }
  if (sample_rate && cJSON_IsNumber(sample_rate)) {
    profile->sample_rate = (uint32_t)sample_rate->valueint;
  }
  if (name && cJSON_IsString(name)) {
    snprintf(profile->name, sizeof(profile->name), "%s", name->valuestring);
  }
  if (enabled && cJSON_IsBool(enabled)) {
    profile->enabled = cJSON_IsTrue(enabled);
  }
  if (!json_number(json, "preamp_db", &profile->requested_preamp_db)) {
    return false;
  }
  cJSON *headroom = cJSON_GetObjectItemCaseSensitive(json, "headroom");
  if (headroom) {
    cJSON *automatic = cJSON_GetObjectItemCaseSensitive(headroom, "auto");
    if (!cJSON_IsObject(headroom) || (automatic && !cJSON_IsBool(automatic)) ||
        !json_number(headroom, "margin_db", &profile->headroom_margin_db)) {
      return false;
    }
    if (automatic) {
      profile->auto_headroom = cJSON_IsTrue(automatic);
    }
  }
  if (!parse_channel(cJSON_GetObjectItemCaseSensitive(json, "left"),
                     &profile->channels[0]) ||
      !parse_channel(cJSON_GetObjectItemCaseSensitive(json, "right"),
                     &profile->channels[1])) {
    return false;
  }
  cJSON *limiter = cJSON_GetObjectItemCaseSensitive(json, "limiter");
  if (limiter) {
    cJSON *limiter_enabled =
        cJSON_GetObjectItemCaseSensitive(limiter, "enabled");
    if (!cJSON_IsObject(limiter) ||
        (limiter_enabled && !cJSON_IsBool(limiter_enabled)) ||
        !json_number(limiter, "ceiling_dbfs", &profile->limiter.ceiling_dbfs) ||
        !json_number(limiter, "lookahead_ms", &profile->limiter.lookahead_ms) ||
        !json_number(limiter, "release_ms", &profile->limiter.release_ms)) {
      return false;
    }
    if (limiter_enabled) {
      profile->limiter.enabled = cJSON_IsTrue(limiter_enabled);
    }
  }
  cJSON *loudness = cJSON_GetObjectItemCaseSensitive(json, "loudness");
  if (loudness) {
    cJSON *loudness_enabled =
        cJSON_GetObjectItemCaseSensitive(loudness, "enabled");
    if (!cJSON_IsObject(loudness) ||
        (loudness_enabled && !cJSON_IsBool(loudness_enabled)) ||
        !json_number(loudness, "max_bass_db", &profile->loudness.max_bass_db) ||
        !json_number(loudness, "max_treble_db",
                     &profile->loudness.max_treble_db) ||
        !json_number(loudness, "full_effect_below_db",
                     &profile->loudness.full_effect_below_db)) {
      return false;
    }
    if (loudness_enabled) {
      profile->loudness.enabled = cJSON_IsTrue(loudness_enabled);
    }
  }
  return true;
}

static esp_err_t apply_and_persist(const cal_dsp_profile_t *profile) {
  cal_dsp_profile_t previous;
  calibration_dsp_get_profile(&previous);
  esp_err_t err = settings_set_dsp_backup(&previous, sizeof(previous));
  if (err != ESP_OK) {
    return err;
  }
  err = calibration_dsp_set_profile(profile);
  if (err != ESP_OK) {
    return err;
  }
  err = settings_set_dsp_profile(profile, sizeof(*profile));
  if (err != ESP_OK) {
    calibration_dsp_set_profile(&previous);
  }
  return err;
}

static esp_err_t capabilities_handler(httpd_req_t *req) {
  cJSON *json = cJSON_CreateObject();
  cJSON_AddBoolToObject(json, "success", true);
  cJSON_AddStringToObject(json, "target", "esp32_airplay");
  cJSON_AddNumberToObject(json, "api_version", CAL_DSP_API_VERSION);
  cJSON_AddNumberToObject(json, "profile_version", CAL_DSP_PROFILE_VERSION);
  cJSON_AddNumberToObject(json, "sample_rate", CONFIG_OUTPUT_SAMPLE_RATE_HZ);
  cJSON_AddNumberToObject(json, "channels", CAL_DSP_CHANNELS);
  cJSON_AddNumberToObject(json, "filters_per_channel", CAL_DSP_MAX_FILTERS);
  cJSON *types = cJSON_AddArrayToObject(json, "filter_types");
  for (int type = CAL_DSP_FILTER_PEAK; type <= CAL_DSP_FILTER_LOW_PASS;
       type++) {
    cJSON_AddItemToArray(
        types, cJSON_CreateString(calibration_dsp_filter_type_name(type)));
  }
  cJSON_AddNumberToObject(json, "max_delay_ms", CAL_DSP_MAX_DELAY_MS);
  cJSON_AddBoolToObject(json, "per_channel", true);
  cJSON_AddBoolToObject(json, "automatic_headroom", true);
  cJSON_AddBoolToObject(json, "stereo_linked_lookahead_limiter", true);
  cJSON_AddNumberToObject(json, "max_lookahead_ms", CAL_DSP_MAX_LOOKAHEAD_MS);
  cJSON_AddBoolToObject(json, "rew_text_import", true);
  cJSON_AddBoolToObject(json, "rollback", true);
  cJSON_AddNumberToObject(json, "profile_slots", SETTINGS_DSP_PROFILE_SLOTS);
  cJSON_AddBoolToObject(json, "click_free_switching", true);
  cJSON_AddBoolToObject(json, "measurement_mode", true);
  cJSON_AddBoolToObject(json, "test_tone", true);
  cJSON_AddBoolToObject(json, "i2s_32bit_slots", true);
  cJSON_AddNumberToObject(json, "i2s_effective_bits", 24);
  cJSON_AddBoolToObject(json, "tpdf_dither", true);
  cJSON_AddBoolToObject(json, "volume_dependent_loudness", true);
  cJSON_AddBoolToObject(json, "mutations_require_token", true);
  return send_json(req, json);
}

static esp_err_t profile_get_handler(httpd_req_t *req) {
  cal_dsp_profile_t profile;
  calibration_dsp_get_profile(&profile);
  return send_json(req, profile_to_json(&profile));
}

static esp_err_t profile_put_handler(httpd_req_t *req) {
  if (api_security_require(req) != ESP_OK)
    return ESP_OK;
  char *body = read_body(req);
  if (!body) {
    return send_error(req, "400 Bad Request", "Missing or oversized body");
  }
  cJSON *json = cJSON_Parse(body);
  free(body);
  cal_dsp_profile_t profile;
  if (!json || !parse_profile(json, &profile)) {
    cJSON_Delete(json);
    return send_error(req, "400 Bad Request", "Invalid DSP profile");
  }
  cJSON_Delete(json);
  esp_err_t err = apply_and_persist(&profile);
  if (err != ESP_OK) {
    return send_error(req, "422 Unprocessable Entity",
                      "Profile validation or persistence failed");
  }
  return send_json(req, profile_to_json(&profile));
}

static esp_err_t bypass_handler(httpd_req_t *req) {
  if (api_security_require(req) != ESP_OK)
    return ESP_OK;
  char *body = read_body(req);
  if (!body) {
    return send_error(req, "400 Bad Request", "Expected bypass JSON");
  }
  cJSON *json = cJSON_Parse(body);
  free(body);
  cJSON *bypass =
      json ? cJSON_GetObjectItemCaseSensitive(json, "bypass") : NULL;
  if (!cJSON_IsBool(bypass)) {
    cJSON_Delete(json);
    return send_error(req, "400 Bad Request", "Expected boolean 'bypass'");
  }
  calibration_dsp_set_bypass(cJSON_IsTrue(bypass));
  cJSON *response = cJSON_CreateObject();
  cJSON_AddBoolToObject(response, "success", true);
  cJSON_AddBoolToObject(response, "bypassed", cJSON_IsTrue(bypass));
  cJSON_Delete(json);
  return send_json(req, response);
}

static esp_err_t rollback_handler(httpd_req_t *req) {
  if (api_security_require(req) != ESP_OK)
    return ESP_OK;
  (void)req;
  cal_dsp_profile_t backup;
  size_t size = sizeof(backup);
  if (settings_get_dsp_backup(&backup, &size) != ESP_OK ||
      size != sizeof(backup)) {
    return send_error(req, "404 Not Found", "No rollback profile saved");
  }
  esp_err_t err = apply_and_persist(&backup);
  if (err != ESP_OK) {
    return send_error(req, "422 Unprocessable Entity", "Rollback failed");
  }
  return send_json(req, profile_to_json(&backup));
}

static esp_err_t metrics_handler(httpd_req_t *req) {
  cal_dsp_metrics_t metrics = {0};
  calibration_dsp_get_metrics(&metrics);
  cJSON *json = cJSON_CreateObject();
  cJSON_AddBoolToObject(json, "success", true);
  cJSON_AddNumberToObject(json, "profile_hash", metrics.profile_hash);
  cJSON_AddNumberToObject(json, "sample_rate", metrics.sample_rate);
  cJSON_AddNumberToObject(json, "frames_processed",
                          (double)metrics.frames_processed);
  cJSON_AddNumberToObject(json, "limited_frames",
                          (double)metrics.limited_frames);
  cJSON_AddNumberToObject(json, "clipped_samples",
                          (double)metrics.clipped_samples);
  cJSON_AddNumberToObject(json, "limiter_gain_db", metrics.limiter_gain_db);
  cJSON_AddNumberToObject(json, "effective_preamp_db",
                          metrics.effective_preamp_db);
  cJSON_AddNumberToObject(json, "cascade_peak_db", metrics.cascade_peak_db);
  cJSON_AddNumberToObject(json, "dsp_load_percent", metrics.dsp_load_percent);
  cJSON_AddNumberToObject(json, "max_dsp_load_percent",
                          metrics.max_dsp_load_percent);
  cJSON_AddNumberToObject(json, "output_underruns",
                          audio_output_get_underruns());
  cJSON_AddNumberToObject(json, "fixed_latency_us",
                          calibration_dsp_get_latency_us());
  cJSON_AddBoolToObject(json, "bypassed", metrics.bypassed);
  cJSON_AddBoolToObject(json, "transition_active", metrics.transition_active);
  cJSON_AddNumberToObject(json, "transition_remaining_ms",
                          metrics.transition_remaining_ms);
  cJSON_AddBoolToObject(json, "measurement_mode", metrics.measurement_mode);
  cJSON_AddNumberToObject(json, "measurement_session_id",
                          metrics.measurement_session_id);
  cJSON_AddBoolToObject(json, "test_tone_active", metrics.test_tone_active);
  return send_json(req, json);
}

static esp_err_t rew_import_handler(httpd_req_t *req) {
  if (api_security_require(req) != ESP_OK)
    return ESP_OK;
  char *body = read_body(req);
  if (!body) {
    return send_error(req, "400 Bad Request", "Expected REW filter text");
  }

  cal_dsp_profile_t profile;
  calibration_dsp_get_profile(&profile);
  snprintf(profile.name, sizeof(profile.name), "REW Import");
  profile.enabled = true;
  profile.auto_headroom = true;
  for (size_t channel = 0; channel < CAL_DSP_CHANNELS; channel++) {
    profile.channels[channel].filter_count = 0;
    memset(profile.channels[channel].filters, 0,
           sizeof(profile.channels[channel].filters));
  }

  size_t imported = 0;
  char *save = NULL;
  for (char *line = strtok_r(body, "\r\n", &save); line;
       line = strtok_r(NULL, "\r\n", &save)) {
    float preamp;
    if (sscanf(line, "Preamp: %f dB", &preamp) == 1) {
      profile.requested_preamp_db = preamp;
      continue;
    }
    char type_name[4] = {0};
    float frequency;
    float gain = 0.0f;
    float q;
    int parsed = sscanf(line, "Filter %*d: ON %3s Fc %f Hz Gain %f dB Q %f",
                        type_name, &frequency, &gain, &q);
    if (parsed != 4) {
      parsed = sscanf(line, "Filter %*d: ON %3s Fc %f Hz Q %f", type_name,
                      &frequency, &q);
      if (parsed != 3) {
        continue;
      }
    }
    cal_dsp_filter_type_t type;
    if (!calibration_dsp_filter_type_parse(type_name, &type) ||
        imported >= CAL_DSP_MAX_FILTERS) {
      continue;
    }
    for (size_t channel = 0; channel < CAL_DSP_CHANNELS; channel++) {
      cal_dsp_filter_t *filter = &profile.channels[channel].filters[imported];
      filter->enabled = true;
      filter->type = type;
      filter->frequency_hz = frequency;
      filter->gain_db = gain;
      filter->q = q;
      profile.channels[channel].filter_count = (uint8_t)(imported + 1U);
    }
    imported++;
  }
  free(body);

  if (imported == 0) {
    return send_error(req, "400 Bad Request", "No supported REW filters found");
  }
  esp_err_t err = apply_and_persist(&profile);
  if (err != ESP_OK) {
    return send_error(req, "422 Unprocessable Entity", "REW profile rejected");
  }
  cJSON *response = profile_to_json(&profile);
  cJSON_AddNumberToObject(response, "imported_filters", imported);
  return send_json(req, response);
}

static bool parse_slot_body(httpd_req_t *req, uint8_t *slot) {
  char *body = read_body(req);
  if (!body)
    return false;
  cJSON *json = cJSON_Parse(body);
  free(body);
  cJSON *item = json ? cJSON_GetObjectItemCaseSensitive(json, "slot") : NULL;
  bool valid = cJSON_IsNumber(item) && item->valueint >= 0 &&
               item->valueint < SETTINGS_DSP_PROFILE_SLOTS;
  if (valid)
    *slot = (uint8_t)item->valueint;
  cJSON_Delete(json);
  return valid;
}

static esp_err_t profiles_get_handler(httpd_req_t *req) {
  cJSON *json = cJSON_CreateObject();
  cJSON_AddBoolToObject(json, "success", true);
  cJSON *slots = cJSON_AddArrayToObject(json, "slots");
  for (uint8_t slot = 0; slot < SETTINGS_DSP_PROFILE_SLOTS; slot++) {
    cal_dsp_profile_t profile;
    size_t size = sizeof(profile);
    bool exists = settings_get_dsp_slot(slot, &profile, &size) == ESP_OK &&
                  size == sizeof(profile) &&
                  profile.version == CAL_DSP_PROFILE_VERSION;
    cJSON *item = cJSON_CreateObject();
    cJSON_AddNumberToObject(item, "slot", slot);
    cJSON_AddBoolToObject(item, "exists", exists);
    if (exists) {
      cJSON_AddStringToObject(item, "name", profile.name);
      cJSON_AddNumberToObject(item, "profile_hash",
                              calibration_dsp_profile_hash(&profile));
    }
    cJSON_AddItemToArray(slots, item);
  }
  return send_json(req, json);
}

static esp_err_t profile_slot_save_handler(httpd_req_t *req) {
  if (api_security_require(req) != ESP_OK)
    return ESP_OK;
  uint8_t slot;
  if (!parse_slot_body(req, &slot)) {
    return send_error(req, "400 Bad Request", "Expected profile slot 0-7");
  }
  cal_dsp_profile_t profile;
  calibration_dsp_get_profile(&profile);
  if (settings_set_dsp_slot(slot, &profile, sizeof(profile)) != ESP_OK) {
    return send_error(req, "500 Internal Server Error", "Profile save failed");
  }
  cJSON *json = profile_to_json(&profile);
  cJSON_AddNumberToObject(json, "slot", slot);
  return send_json(req, json);
}

static esp_err_t profile_slot_load_handler(httpd_req_t *req) {
  if (api_security_require(req) != ESP_OK)
    return ESP_OK;
  uint8_t slot;
  if (!parse_slot_body(req, &slot)) {
    return send_error(req, "400 Bad Request", "Expected profile slot 0-7");
  }
  cal_dsp_profile_t profile;
  size_t size = sizeof(profile);
  if (settings_get_dsp_slot(slot, &profile, &size) != ESP_OK ||
      size != sizeof(profile)) {
    return send_error(req, "404 Not Found", "Profile slot is empty");
  }
  if (apply_and_persist(&profile) != ESP_OK) {
    return send_error(req, "422 Unprocessable Entity",
                      "Stored profile rejected");
  }
  cJSON *json = profile_to_json(&profile);
  cJSON_AddNumberToObject(json, "slot", slot);
  return send_json(req, json);
}

static esp_err_t profile_slot_delete_handler(httpd_req_t *req) {
  if (api_security_require(req) != ESP_OK)
    return ESP_OK;
  uint8_t slot;
  if (!parse_slot_body(req, &slot)) {
    return send_error(req, "400 Bad Request", "Expected profile slot 0-7");
  }
  esp_err_t err = settings_delete_dsp_slot(slot);
  if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
    return send_error(req, "500 Internal Server Error",
                      "Profile delete failed");
  }
  cJSON *json = cJSON_CreateObject();
  cJSON_AddBoolToObject(json, "success", true);
  cJSON_AddNumberToObject(json, "slot", slot);
  return send_json(req, json);
}

static esp_err_t measurement_handler(httpd_req_t *req) {
  if (api_security_require(req) != ESP_OK)
    return ESP_OK;
  char *body = read_body(req);
  cJSON *json = body ? cJSON_Parse(body) : NULL;
  free(body);
  cJSON *enabled =
      json ? cJSON_GetObjectItemCaseSensitive(json, "enabled") : NULL;
  cJSON *volume =
      json ? cJSON_GetObjectItemCaseSensitive(json, "fixed_volume_db") : NULL;
  cJSON *hash =
      json ? cJSON_GetObjectItemCaseSensitive(json, "expected_profile_hash")
           : NULL;
  if (!cJSON_IsBool(enabled) || (volume && !cJSON_IsNumber(volume)) ||
      (hash && !cJSON_IsNumber(hash))) {
    cJSON_Delete(json);
    return send_error(req, "400 Bad Request", "Invalid measurement settings");
  }
  float volume_db = volume ? (float)volume->valuedouble : -20.0f;
  if (volume_db < -60.0f || volume_db > 0.0f) {
    cJSON_Delete(json);
    return send_error(req, "400 Bad Request", "fixed_volume_db must be -60..0");
  }
  int32_t q15 = (int32_t)lroundf(powf(10.0f, volume_db / 20.0f) * 32768.0f);
  uint32_t expected = hash ? (uint32_t)hash->valuedouble : 0;
  esp_err_t err = calibration_dsp_set_measurement_mode(cJSON_IsTrue(enabled),
                                                       q15, expected);
  cJSON_Delete(json);
  if (err == ESP_ERR_INVALID_CRC) {
    return send_error(req, "409 Conflict",
                      "Active profile hash does not match");
  }
  if (err != ESP_OK) {
    return send_error(req, "422 Unprocessable Entity",
                      "Measurement mode rejected");
  }
  uint32_t session_id;
  int32_t fixed_q15;
  bool active = calibration_dsp_get_measurement_mode(&session_id, &fixed_q15);
  cJSON *response = cJSON_CreateObject();
  cJSON_AddBoolToObject(response, "success", true);
  cJSON_AddBoolToObject(response, "enabled", active);
  cJSON_AddNumberToObject(response, "session_id", session_id);
  cJSON_AddNumberToObject(response, "fixed_volume_q15", fixed_q15);
  cJSON_AddBoolToObject(response, "loudness_forced_off", active);
  return send_json(req, response);
}

static esp_err_t test_tone_handler(httpd_req_t *req) {
  if (api_security_require(req) != ESP_OK)
    return ESP_OK;
  char *body = read_body(req);
  cJSON *json = body ? cJSON_Parse(body) : NULL;
  free(body);
  cJSON *enabled =
      json ? cJSON_GetObjectItemCaseSensitive(json, "enabled") : NULL;
  if (!cJSON_IsBool(enabled)) {
    cJSON_Delete(json);
    return send_error(req, "400 Bad Request", "Expected boolean enabled");
  }
  bool turn_on = cJSON_IsTrue(enabled);
  if (turn_on && audio_receiver_is_playing()) {
    cJSON_Delete(json);
    return send_error(req, "409 Conflict",
                      "Stop AirPlay before starting a test tone");
  }
  cJSON *frequency = cJSON_GetObjectItemCaseSensitive(json, "frequency_hz");
  cJSON *level = cJSON_GetObjectItemCaseSensitive(json, "level_dbfs");
  cJSON *channels = cJSON_GetObjectItemCaseSensitive(json, "channel_mask");
  cJSON *duration = cJSON_GetObjectItemCaseSensitive(json, "duration_ms");
  esp_err_t err = calibration_dsp_set_test_tone(
      turn_on,
      cJSON_IsNumber(frequency) ? (float)frequency->valuedouble : 1000.0f,
      cJSON_IsNumber(level) ? (float)level->valuedouble : -30.0f,
      cJSON_IsNumber(channels) ? (uint8_t)channels->valueint : 3,
      cJSON_IsNumber(duration) ? (uint32_t)duration->valuedouble : 5000U);
  cJSON_Delete(json);
  if (err != ESP_OK) {
    return send_error(
        req, "422 Unprocessable Entity",
        "Tone must be 20-20000 Hz, -60..-12 dBFS, max 30 seconds");
  }
  cJSON *response = cJSON_CreateObject();
  cJSON_AddBoolToObject(response, "success", true);
  cJSON_AddBoolToObject(response, "active", turn_on);
  return send_json(req, response);
}

static esp_err_t security_status_handler(httpd_req_t *req) {
  char hint[16];
  api_security_token_hint(hint, sizeof(hint));
  cJSON *json = cJSON_CreateObject();
  cJSON_AddBoolToObject(json, "success", true);
  cJSON_AddBoolToObject(json, "token_required", true);
  cJSON_AddStringToObject(json, "token_hint", hint);
  cJSON_AddStringToObject(json, "header", "X-AirPlay-Token");
  return send_json(req, json);
}

static esp_err_t security_reveal_handler(httpd_req_t *req) {
  char token[API_SECURITY_TOKEN_HEX_LEN + 1U];
  if (!api_security_reveal_if_boot_held(token, sizeof(token))) {
    return send_error(
        req, "403 Forbidden",
        "Hold the physical BOOT button while requesting the token");
  }
  cJSON *json = cJSON_CreateObject();
  cJSON_AddBoolToObject(json, "success", true);
  cJSON_AddStringToObject(json, "token", token);
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return send_json(req, json);
}

static esp_err_t audio_health_handler(httpd_req_t *req) {
  audio_stats_t stats = {0};
  audio_health_snapshot_t health = {0};
  ptp_stats_t ptp = {0};
  diagnostics_snapshot_t diagnostics = {0};
  audio_receiver_get_stats(&stats);
  audio_receiver_get_health(&health);
  ptp_clock_get_stats(&ptp);
  diagnostics_get_snapshot(&diagnostics);

  cJSON *json = cJSON_CreateObject();
  cJSON_AddBoolToObject(json, "success", true);
  cJSON_AddBoolToObject(json, "playing", health.playing);
  cJSON_AddBoolToObject(json, "playout_started", health.playout_started);
  cJSON_AddNumberToObject(json, "stream_type", health.stream_type);
  cJSON_AddStringToObject(json, "codec", health.format.codec);
  cJSON_AddNumberToObject(json, "sample_rate", health.format.sample_rate);
  cJSON_AddNumberToObject(json, "bits_per_sample",
                          health.format.bits_per_sample);
  cJSON_AddNumberToObject(json, "buffered_frames", health.buffered_frames);
  cJSON_AddNumberToObject(json, "packets_received", stats.packets_received);
  cJSON_AddNumberToObject(json, "packets_decoded", stats.packets_decoded);
  cJSON_AddNumberToObject(json, "packets_dropped", stats.packets_dropped);
  cJSON_AddNumberToObject(json, "decrypt_errors", stats.decrypt_errors);
  cJSON_AddNumberToObject(json, "buffer_underruns", stats.buffer_underruns);
  cJSON_AddNumberToObject(json, "buffer_overruns", stats.buffer_overruns);
  cJSON_AddNumberToObject(json, "late_frames", stats.late_frames);
  cJSON_AddNumberToObject(json, "rtp_gaps", health.rtp_gaps);
  cJSON_AddNumberToObject(json, "servo_trims", health.servo_trims);
  cJSON_AddNumberToObject(json, "position_error_us",
                          (double)health.position_error_us);
  cJSON_AddBoolToObject(json, "ptp_locked", ptp_clock_is_locked());
  cJSON_AddNumberToObject(json, "ptp_offset_ns",
                          (double)ptp.filtered_offset_ns);
  cJSON_AddNumberToObject(
      json, "ptp_gap_us",
      (double)((ptp.last_offset_ns - ptp.filtered_offset_ns) / 1000LL));
  cJSON_AddNumberToObject(json, "ptp_outliers", ptp.outlier_count);
  cJSON_AddNumberToObject(json, "output_underruns",
                          audio_output_get_underruns());
  cJSON_AddNumberToObject(json, "free_heap", esp_get_free_heap_size());
  cJSON_AddNumberToObject(json, "free_psram",
                          heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  cJSON_AddNumberToObject(json, "boot_count", diagnostics.boot_count);
  cJSON_AddNumberToObject(json, "crash_count", diagnostics.crash_count);
  cJSON_AddNumberToObject(json, "last_reset_reason", diagnostics.reset_reason);
  cJSON_AddBoolToObject(json, "ota_pending_verify",
                        diagnostics.ota_pending_verify);
  cJSON *events = cJSON_AddArrayToObject(json, "recent_events");
  for (uint32_t i = 0; i < diagnostics.rtc_event_count; i++) {
    cJSON_AddItemToArray(events, cJSON_CreateString(diagnostics.events[i]));
  }
  return send_json(req, json);
}

#ifdef CONFIG_MQTT_HA_ENABLED
static esp_err_t mqtt_status_handler(httpd_req_t *req) {
  mqtt_ha_status_t status = {0};
  mqtt_ha_get_status(&status);
  cJSON *json = cJSON_CreateObject();
  cJSON_AddBoolToObject(json, "success", true);
  cJSON_AddBoolToObject(json, "configured", status.configured);
  cJSON_AddBoolToObject(json, "enabled", status.enabled);
  cJSON_AddBoolToObject(json, "connected", status.connected);
  cJSON_AddStringToObject(json, "broker_uri", status.broker_uri);
  cJSON_AddStringToObject(json, "username", status.username);
  cJSON_AddStringToObject(json, "device_id", status.device_id);
  cJSON_AddNumberToObject(json, "last_error", status.last_error);
  cJSON_AddBoolToObject(json, "password_set", status.password_set);
  return send_json(req, json);
}

static esp_err_t mqtt_config_handler(httpd_req_t *req) {
  if (api_security_require(req) != ESP_OK)
    return ESP_OK;
  char *body = read_body(req);
  cJSON *json = body ? cJSON_Parse(body) : NULL;
  free(body);
  cJSON *enabled =
      json ? cJSON_GetObjectItemCaseSensitive(json, "enabled") : NULL;
  cJSON *broker =
      json ? cJSON_GetObjectItemCaseSensitive(json, "broker_uri") : NULL;
  cJSON *username =
      json ? cJSON_GetObjectItemCaseSensitive(json, "username") : NULL;
  cJSON *password =
      json ? cJSON_GetObjectItemCaseSensitive(json, "password") : NULL;
  if (!cJSON_IsBool(enabled) || !cJSON_IsString(broker) ||
      (username && !cJSON_IsString(username)) ||
      (password && !cJSON_IsString(password))) {
    cJSON_Delete(json);
    return send_error(req, "400 Bad Request", "Invalid MQTT configuration");
  }
  mqtt_ha_config_t config;
  mqtt_ha_get_config(&config);
  config.version = MQTT_HA_CONFIG_VERSION;
  config.enabled = cJSON_IsTrue(enabled);
  snprintf(config.broker_uri, sizeof(config.broker_uri), "%s",
           broker->valuestring);
  if (username)
    snprintf(config.username, sizeof(config.username), "%s",
             username->valuestring);
  if (password && password->valuestring[0]) {
    snprintf(config.password, sizeof(config.password), "%s",
             password->valuestring);
  }
  cJSON_Delete(json);
  if (mqtt_ha_set_config(&config) != ESP_OK) {
    return send_error(req, "422 Unprocessable Entity",
                      "Use a trusted-LAN mqtt:// broker and valid credentials");
  }
  mqtt_ha_publish_now();
  return mqtt_status_handler(req);
}
#endif

esp_err_t dsp_api_register(httpd_handle_t server) {
  if (!server) {
    return ESP_ERR_INVALID_ARG;
  }
  const httpd_uri_t endpoints[] = {
      {.uri = "/api/dsp/capabilities",
       .method = HTTP_GET,
       .handler = capabilities_handler},
      {.uri = "/api/dsp/profile",
       .method = HTTP_GET,
       .handler = profile_get_handler},
      {.uri = "/api/dsp/profile",
       .method = HTTP_PUT,
       .handler = profile_put_handler},
      {.uri = "/api/dsp/bypass",
       .method = HTTP_POST,
       .handler = bypass_handler},
      {.uri = "/api/dsp/rollback",
       .method = HTTP_POST,
       .handler = rollback_handler},
      {.uri = "/api/dsp/metrics",
       .method = HTTP_GET,
       .handler = metrics_handler},
      {.uri = "/api/dsp/rew",
       .method = HTTP_POST,
       .handler = rew_import_handler},
      {.uri = "/api/dsp/profiles",
       .method = HTTP_GET,
       .handler = profiles_get_handler},
      {.uri = "/api/dsp/profile/save",
       .method = HTTP_POST,
       .handler = profile_slot_save_handler},
      {.uri = "/api/dsp/profile/load",
       .method = HTTP_POST,
       .handler = profile_slot_load_handler},
      {.uri = "/api/dsp/profile/delete",
       .method = HTTP_POST,
       .handler = profile_slot_delete_handler},
      {.uri = "/api/dsp/measurement",
       .method = HTTP_POST,
       .handler = measurement_handler},
      {.uri = "/api/audio/test-tone",
       .method = HTTP_POST,
       .handler = test_tone_handler},
      {.uri = "/api/audio/health",
       .method = HTTP_GET,
       .handler = audio_health_handler},
      {.uri = "/api/security/status",
       .method = HTTP_GET,
       .handler = security_status_handler},
      {.uri = "/api/security/reveal",
       .method = HTTP_POST,
       .handler = security_reveal_handler},
#ifdef CONFIG_MQTT_HA_ENABLED
      {.uri = "/api/mqtt/status",
       .method = HTTP_GET,
       .handler = mqtt_status_handler},
      {.uri = "/api/mqtt/config",
       .method = HTTP_PUT,
       .handler = mqtt_config_handler},
#endif
  };
  for (size_t i = 0; i < sizeof(endpoints) / sizeof(endpoints[0]); i++) {
    esp_err_t err = httpd_register_uri_handler(server, &endpoints[i]);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "Failed to register %s: %s", endpoints[i].uri,
               esp_err_to_name(err));
      return err;
    }
  }
  return ESP_OK;
}
