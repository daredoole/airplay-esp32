#include "dsp_api.h"

#include "audio_output.h"
#include "calibration_dsp.h"
#include "cJSON.h"
#include "esp_log.h"
#include "settings.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
  return send_json(req, json);
}

static esp_err_t profile_get_handler(httpd_req_t *req) {
  cal_dsp_profile_t profile;
  calibration_dsp_get_profile(&profile);
  return send_json(req, profile_to_json(&profile));
}

static esp_err_t profile_put_handler(httpd_req_t *req) {
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
  return send_json(req, json);
}

static esp_err_t rew_import_handler(httpd_req_t *req) {
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
