#include "api_security.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "settings.h"
#include <stdio.h>
#include <string.h>

#define TAG "api_security"

static char s_token[API_SECURITY_TOKEN_HEX_LEN + 1U];
static bool s_ready;

static bool constant_time_equal(const char *a, const char *b) {
  size_t a_len = a ? strlen(a) : 0;
  size_t b_len = b ? strlen(b) : 0;
  unsigned char difference = (unsigned char)(a_len ^ b_len);
  size_t length = a_len > b_len ? a_len : b_len;
  for (size_t i = 0; i < length; i++) {
    unsigned char ac = i < a_len ? (unsigned char)a[i] : 0;
    unsigned char bc = i < b_len ? (unsigned char)b[i] : 0;
    difference |= ac ^ bc;
  }
  return difference == 0;
}

esp_err_t api_security_init(void) {
  if (s_ready) {
    return ESP_OK;
  }
  gpio_config_t boot_button = {
      .pin_bit_mask = 1ULL << GPIO_NUM_0,
      .mode = GPIO_MODE_INPUT,
      .pull_up_en = GPIO_PULLUP_ENABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  gpio_config(&boot_button);
  size_t length = sizeof(s_token);
  if (settings_get_api_token(s_token, &length) == ESP_OK &&
      strlen(s_token) == API_SECURITY_TOKEN_HEX_LEN) {
    s_ready = true;
    return ESP_OK;
  }

  uint8_t random[API_SECURITY_TOKEN_HEX_LEN / 2U];
  esp_fill_random(random, sizeof(random));
  for (size_t i = 0; i < sizeof(random); i++) {
    snprintf(&s_token[i * 2U], 3U, "%02x", random[i]);
  }
  esp_err_t err = settings_set_api_token(s_token);
  if (err != ESP_OK) {
    memset(s_token, 0, sizeof(s_token));
    return err;
  }
  s_ready = true;
  ESP_LOGW(TAG, "New API token (shown once): %s", s_token);
  ESP_LOGW(TAG, "Save it in the DSP web UI or MCP secret store");
  return ESP_OK;
}

bool api_security_authorized(httpd_req_t *req) {
  if (!req || api_security_init() != ESP_OK) {
    return false;
  }
  char supplied[API_SECURITY_TOKEN_HEX_LEN + 16U] = {0};
  size_t length = httpd_req_get_hdr_value_len(req, "X-AirPlay-Token");
  if (length > 0 && length < sizeof(supplied) &&
      httpd_req_get_hdr_value_str(req, "X-AirPlay-Token", supplied,
                                  sizeof(supplied)) == ESP_OK) {
    return constant_time_equal(supplied, s_token);
  }

  length = httpd_req_get_hdr_value_len(req, "Authorization");
  if (length > 0 && length < sizeof(supplied) &&
      httpd_req_get_hdr_value_str(req, "Authorization", supplied,
                                  sizeof(supplied)) == ESP_OK) {
    const char prefix[] = "Bearer ";
    if (strncmp(supplied, prefix, sizeof(prefix) - 1U) == 0) {
      return constant_time_equal(supplied + sizeof(prefix) - 1U, s_token);
    }
  }
  return false;
}

esp_err_t api_security_require(httpd_req_t *req) {
  if (api_security_authorized(req)) {
    return ESP_OK;
  }
  httpd_resp_set_status(req, "401 Unauthorized");
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_set_hdr(req, "WWW-Authenticate", "Bearer realm=\"airplay-esp32\"");
  httpd_resp_sendstr(req,
                     "{\"success\":false,\"error\":\"API token required\"}");
  return ESP_ERR_INVALID_STATE;
}

void api_security_token_hint(char *out, size_t out_len) {
  if (!out || out_len == 0) {
    return;
  }
  out[0] = '\0';
  if (api_security_init() == ESP_OK) {
    snprintf(out, out_len, "…%s", s_token + API_SECURITY_TOKEN_HEX_LEN - 6U);
  }
}

bool api_security_reveal_if_boot_held(char *out, size_t out_len) {
  if (!out || out_len < sizeof(s_token) || api_security_init() != ESP_OK ||
      gpio_get_level(GPIO_NUM_0) != 0) {
    return false;
  }
  snprintf(out, out_len, "%s", s_token);
  return true;
}
