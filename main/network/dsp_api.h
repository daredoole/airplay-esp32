#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

/** Register the calibration DSP, REW import, and MCP-facing endpoints. */
esp_err_t dsp_api_register(httpd_handle_t server);
