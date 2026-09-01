#pragma once

#include "esp_err.h"
#include "esp_http_server.h"
#include <stdbool.h>
#include <stddef.h>

#define API_SECURITY_TOKEN_HEX_LEN 64U

/** Load or generate the device-local API token. */
esp_err_t api_security_init(void);

/** Constant-time bearer/X-AirPlay-Token authorization check. */
bool api_security_authorized(httpd_req_t *req);

/** Send a JSON 401 response when the request is not authorized. */
esp_err_t api_security_require(httpd_req_t *req);

/** A non-secret suffix suitable for identifying the installed token. */
void api_security_token_hint(char *out, size_t out_len);

/** Copy the token only while the board's physical BOOT button is held. */
bool api_security_reveal_if_boot_held(char *out, size_t out_len);
