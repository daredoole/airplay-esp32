#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CAL_DSP_PROFILE_VERSION     3U
#define CAL_DSP_API_VERSION         3U
#define CAL_DSP_CHANNELS            2U
#define CAL_DSP_MAX_FILTERS         10U
#define CAL_DSP_MAX_DELAY_MS        10.0f
#define CAL_DSP_MAX_LOOKAHEAD_MS    5.0f
#define CAL_DSP_MAX_LATENCY_TRIM_US 250000

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
  bool enabled;
  float max_bass_db;
  float max_treble_db;
  float full_effect_below_db;
} cal_dsp_loudness_t;

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
  cal_dsp_loudness_t loudness;
  /** Acoustic/output timing correction, positive when this output is late. */
  int32_t output_latency_trim_us;
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
  bool transition_active;
  bool measurement_mode;
  bool test_tone_active;
  bool sync_test_active;
  uint32_t transition_remaining_ms;
  uint32_t measurement_session_id;
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

/** Process to signed 24-bit PCM left-aligned in 32-bit I2S slots. */
void calibration_dsp_process_i32(int16_t *pcm, int32_t *pcm_i32, size_t frames,
                                 int32_t software_volume_q15);

/** Lock gain and disable loudness for repeatable measurements. */
esp_err_t calibration_dsp_set_measurement_mode(bool enabled,
                                               int32_t fixed_volume_q15,
                                               uint32_t expected_profile_hash);
bool calibration_dsp_get_measurement_mode(uint32_t *session_id,
                                          int32_t *fixed_volume_q15);

/** Safe, time-limited I2S path test signal. channel_mask: 1=L, 2=R, 3=both. */
esp_err_t calibration_dsp_set_test_tone(bool enabled, float frequency_hz,
                                        float level_dbfs, uint8_t channel_mask,
                                        uint32_t duration_ms);
bool calibration_dsp_test_tone_active(void);

/** Repeating, time-bounded 2 kHz burst for acoustic multi-room alignment. */
esp_err_t calibration_dsp_set_sync_test(bool enabled, uint32_t interval_ms,
                                        uint32_t pulse_ms, float level_dbfs,
                                        uint8_t channel_mask,
                                        uint32_t duration_ms);

/** True while either protected local signal generator is active. */
bool calibration_dsp_signal_generator_active(void);

/** Clear filter, delay, and limiter history after flush/seek. */
void calibration_dsp_reset(void);

/** Fixed look-ahead delay introduced by the active limiter. */
uint32_t calibration_dsp_get_latency_us(void);

/** Signed profile-bound correction used by AirPlay playout timing. */
int32_t calibration_dsp_get_output_latency_trim_us(void);

/** Upgrade an older, prefix-compatible profile blob in place. */
bool calibration_dsp_upgrade_profile(cal_dsp_profile_t *profile,
                                     size_t stored_size, uint32_t sample_rate);

/** Runtime telemetry for MCP apply/measure/verify workflows. */
void calibration_dsp_get_metrics(cal_dsp_metrics_t *metrics);

/** Stable hash of the exact profile bytes accepted by the firmware. */
uint32_t calibration_dsp_profile_hash(const cal_dsp_profile_t *profile);

const char *calibration_dsp_filter_type_name(cal_dsp_filter_type_t type);
bool calibration_dsp_filter_type_parse(const char *name,
                                       cal_dsp_filter_type_t *type);
