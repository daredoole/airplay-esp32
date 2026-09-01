#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CAL_DSP_PROFILE_VERSION  1U
#define CAL_DSP_API_VERSION      1U
#define CAL_DSP_CHANNELS         2U
#define CAL_DSP_MAX_FILTERS      10U
#define CAL_DSP_MAX_DELAY_MS     10.0f
#define CAL_DSP_MAX_LOOKAHEAD_MS 5.0f

typedef enum {
  CAL_DSP_FILTER_PEAK = 0,
  CAL_DSP_FILTER_LOW_SHELF,
  CAL_DSP_FILTER_HIGH_SHELF,
  CAL_DSP_FILTER_HIGH_PASS,
  CAL_DSP_FILTER_LOW_PASS,
} cal_dsp_filter_type_t;

typedef struct {
  bool enabled;
  cal_dsp_filter_type_t type;
  float frequency_hz;
  float gain_db;
  float q;
} cal_dsp_filter_t;

typedef struct {
  float gain_db;
  float delay_ms;
  int8_t polarity;
  uint8_t filter_count;
  cal_dsp_filter_t filters[CAL_DSP_MAX_FILTERS];
} cal_dsp_channel_t;

typedef struct {
  bool enabled;
  float ceiling_dbfs;
  float lookahead_ms;
  float release_ms;
} cal_dsp_limiter_t;

typedef struct {
  uint32_t version;
  uint32_t sample_rate;
  char name[32];
  bool enabled;
  bool auto_headroom;
  float requested_preamp_db;
  float headroom_margin_db;
  cal_dsp_channel_t channels[CAL_DSP_CHANNELS];
  cal_dsp_limiter_t limiter;
} cal_dsp_profile_t;

typedef struct {
  uint64_t frames_processed;
  uint64_t limited_frames;
  uint64_t clipped_samples;
  uint32_t profile_hash;
  uint32_t sample_rate;
  float effective_preamp_db;
  float cascade_peak_db;
  float limiter_gain_db;
  float dsp_load_percent;
  float max_dsp_load_percent;
  bool bypassed;
} cal_dsp_metrics_t;

/** Initialize the DSP and load a saved profile when one is available. */
esp_err_t calibration_dsp_init(uint32_t sample_rate);

/** Fill a safe, bypassed default profile. */
void calibration_dsp_default_profile(cal_dsp_profile_t *profile,
                                     uint32_t sample_rate);

/** Validate and atomically activate a profile. */
esp_err_t calibration_dsp_set_profile(const cal_dsp_profile_t *profile);

/** Copy the active profile. */
void calibration_dsp_get_profile(cal_dsp_profile_t *profile);

/** Runtime bypass; the saved profile remains intact. */
void calibration_dsp_set_bypass(bool bypass);
bool calibration_dsp_get_bypass(void);

/**
 * Process interleaved stereo PCM entirely in float before final quantization.
 * software_volume_q15 is unity (32768) when volume is handled by a DAC.
 */
void calibration_dsp_process(int16_t *pcm, size_t frames,
                             int32_t software_volume_q15);

/** Clear filter, delay, and limiter history after flush/seek. */
void calibration_dsp_reset(void);

/** Fixed look-ahead delay introduced by the active limiter. */
uint32_t calibration_dsp_get_latency_us(void);

/** Runtime telemetry for MCP apply/measure/verify workflows. */
void calibration_dsp_get_metrics(cal_dsp_metrics_t *metrics);

/** Stable hash of the exact profile bytes accepted by the firmware. */
uint32_t calibration_dsp_profile_hash(const cal_dsp_profile_t *profile);

const char *calibration_dsp_filter_type_name(cal_dsp_filter_type_t type);
bool calibration_dsp_filter_type_parse(const char *name,
                                       cal_dsp_filter_type_t *type);
