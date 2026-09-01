#include "calibration_dsp.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "settings.h"
#include <float.h>
#include <inttypes.h>
#include <math.h>
#include <string.h>

#define TAG                       "cal_dsp"
#define CAL_DSP_MAX_RATE          48000U
#define CAL_DSP_DELAY_SAMPLES     ((CAL_DSP_MAX_RATE * 10U / 1000U) + 1U)
#define CAL_DSP_LOOKAHEAD_SAMPLES ((CAL_DSP_MAX_RATE * 5U / 1000U) + 2U)
#define RESPONSE_POINTS           512U
#define PI_F                      3.14159265358979323846f

/* Version 2 ended immediately before output_latency_trim_us. Keeping the new
 * field appended makes old NVS blobs safely prefix-compatible. */
#define CAL_DSP_PROFILE_V2_SIZE \
  offsetof(cal_dsp_profile_t, output_latency_trim_us)

typedef struct {
  float b0;
  float b1;
  float b2;
  float a1;
  float a2;
  float z1;
  float z2;
} biquad_t;

typedef struct {
  cal_dsp_profile_t profile;
  biquad_t biquads[CAL_DSP_CHANNELS][CAL_DSP_MAX_FILTERS];
  biquad_t loudness_biquads[CAL_DSP_CHANNELS][2];
  float delay[CAL_DSP_CHANNELS][CAL_DSP_DELAY_SAMPLES];
  size_t delay_length[CAL_DSP_CHANNELS];
  size_t delay_pos[CAL_DSP_CHANNELS];
  float limiter_audio[CAL_DSP_LOOKAHEAD_SAMPLES][CAL_DSP_CHANNELS];
  float limiter_peak[CAL_DSP_LOOKAHEAD_SAMPLES];
  uint64_t limiter_deque[CAL_DSP_LOOKAHEAD_SAMPLES];
  size_t limiter_head;
  size_t limiter_tail;
  uint64_t limiter_sequence;
  size_t limiter_lookahead;
  float limiter_gain;
  float limiter_release;
  float limiter_ceiling;
  int32_t volume_q15_current;
  float effective_preamp_db;
  float cascade_peak_db;
  float loudness_bass_db;
  float loudness_treble_db;
  float last_output[CAL_DSP_CHANNELS];
  float transition_from[CAL_DSP_CHANNELS];
  uint32_t transition_total;
  uint32_t transition_remaining;
  bool measurement_mode;
  int32_t measurement_volume_q15;
  uint32_t measurement_session_id;
  bool test_tone;
  float test_tone_frequency_hz;
  float test_tone_gain;
  float test_tone_phase;
  uint8_t test_tone_channel_mask;
  int64_t test_tone_expires_us;
  bool sync_test;
  float sync_test_gain;
  uint8_t sync_test_channel_mask;
  uint32_t sync_test_interval_samples;
  uint32_t sync_test_pulse_samples;
  uint32_t sync_test_position;
  int64_t sync_test_expires_us;
  uint32_t dither_state;
  cal_dsp_metrics_t metrics;
  bool bypass;
  bool ready;
} dsp_state_t;

static dsp_state_t s_dsp;
static SemaphoreHandle_t s_lock;

static float db_to_gain(float db) {
  return powf(10.0f, db / 20.0f);
}

static float gain_to_db(float gain) {
  return gain > 0.0000001f ? 20.0f * log10f(gain) : -140.0f;
}

static float clampf(float value, float low, float high) {
  return value < low ? low : value > high ? high : value;
}

const char *calibration_dsp_filter_type_name(cal_dsp_filter_type_t type) {
  switch (type) {
  case CAL_DSP_FILTER_PEAK:
    return "PK";
  case CAL_DSP_FILTER_LOW_SHELF:
    return "LS";
  case CAL_DSP_FILTER_HIGH_SHELF:
    return "HS";
  case CAL_DSP_FILTER_HIGH_PASS:
    return "HP";
  case CAL_DSP_FILTER_LOW_PASS:
    return "LP";
  default:
    return "PK";
  }
}

bool calibration_dsp_filter_type_parse(const char *name,
                                       cal_dsp_filter_type_t *type) {
  if (!name || !type) {
    return false;
  }
  if (strcmp(name, "PK") == 0) {
    *type = CAL_DSP_FILTER_PEAK;
  } else if (strcmp(name, "LS") == 0) {
    *type = CAL_DSP_FILTER_LOW_SHELF;
  } else if (strcmp(name, "HS") == 0) {
    *type = CAL_DSP_FILTER_HIGH_SHELF;
  } else if (strcmp(name, "HP") == 0 || strcmp(name, "HPF") == 0) {
    *type = CAL_DSP_FILTER_HIGH_PASS;
  } else if (strcmp(name, "LP") == 0 || strcmp(name, "LPF") == 0) {
    *type = CAL_DSP_FILTER_LOW_PASS;
  } else {
    return false;
  }
  return true;
}

void calibration_dsp_default_profile(cal_dsp_profile_t *profile,
                                     uint32_t sample_rate) {
  if (!profile) {
    return;
  }
  memset(profile, 0, sizeof(*profile));
  profile->version = CAL_DSP_PROFILE_VERSION;
  profile->sample_rate = sample_rate;
  memcpy(profile->name, "Flat", 5);
  profile->enabled = false;
  profile->auto_headroom = true;
  profile->headroom_margin_db = 1.0f;
  for (size_t channel = 0; channel < CAL_DSP_CHANNELS; channel++) {
    profile->channels[channel].polarity = 1;
  }
  profile->limiter.enabled = true;
  profile->limiter.ceiling_dbfs = -1.0f;
  profile->limiter.lookahead_ms = 3.0f;
  profile->limiter.release_ms = 80.0f;
  profile->loudness.enabled = false;
  profile->loudness.max_bass_db = 6.0f;
  profile->loudness.max_treble_db = 2.5f;
  profile->loudness.full_effect_below_db = -40.0f;
}

bool calibration_dsp_upgrade_profile(cal_dsp_profile_t *profile,
                                     size_t stored_size, uint32_t sample_rate) {
  if (!profile || profile->sample_rate != sample_rate) {
    return false;
  }
  if (stored_size == sizeof(*profile) &&
      profile->version == CAL_DSP_PROFILE_VERSION) {
    return true;
  }
  if (stored_size == CAL_DSP_PROFILE_V2_SIZE && profile->version == 2U) {
    profile->output_latency_trim_us = 0;
    profile->version = CAL_DSP_PROFILE_VERSION;
    return true;
  }
  return false;
}

uint32_t calibration_dsp_profile_hash(const cal_dsp_profile_t *profile) {
  if (!profile) {
    return 0;
  }
  const uint8_t *bytes = (const uint8_t *)profile;
  uint32_t hash = 2166136261U;
  for (size_t i = 0; i < sizeof(*profile); i++) {
    hash ^= bytes[i];
    hash *= 16777619U;
  }
  return hash;
}

static esp_err_t validate_profile(const cal_dsp_profile_t *profile) {
  if (!profile || profile->version != CAL_DSP_PROFILE_VERSION ||
      profile->sample_rate < 8000U || profile->sample_rate > CAL_DSP_MAX_RATE ||
      profile->requested_preamp_db < -30.0f ||
      profile->requested_preamp_db > 6.0f ||
      profile->headroom_margin_db < 0.0f ||
      profile->headroom_margin_db > 6.0f ||
      profile->output_latency_trim_us < -CAL_DSP_MAX_LATENCY_TRIM_US ||
      profile->output_latency_trim_us > CAL_DSP_MAX_LATENCY_TRIM_US) {
    return ESP_ERR_INVALID_ARG;
  }
  for (size_t channel = 0; channel < CAL_DSP_CHANNELS; channel++) {
    const cal_dsp_channel_t *ch = &profile->channels[channel];
    if (ch->filter_count > CAL_DSP_MAX_FILTERS || ch->gain_db < -18.0f ||
        ch->gain_db > 6.0f || ch->delay_ms < 0.0f ||
        ch->delay_ms > CAL_DSP_MAX_DELAY_MS ||
        (ch->polarity != 1 && ch->polarity != -1)) {
      return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < ch->filter_count; i++) {
      const cal_dsp_filter_t *filter = &ch->filters[i];
      if (filter->type > CAL_DSP_FILTER_LOW_PASS ||
          filter->frequency_hz < 10.0f ||
          filter->frequency_hz > (float)profile->sample_rate * 0.45f ||
          filter->gain_db < -18.0f || filter->gain_db > 12.0f ||
          filter->q < 0.1f || filter->q > 20.0f) {
        return ESP_ERR_INVALID_ARG;
      }
    }
  }
  if (profile->limiter.enabled &&
      (profile->limiter.ceiling_dbfs > 0.0f ||
       profile->limiter.ceiling_dbfs < -12.0f ||
       profile->limiter.lookahead_ms < 0.5f ||
       profile->limiter.lookahead_ms > CAL_DSP_MAX_LOOKAHEAD_MS ||
       profile->limiter.release_ms < 10.0f ||
       profile->limiter.release_ms > 1000.0f)) {
    return ESP_ERR_INVALID_ARG;
  }
  if (profile->loudness.max_bass_db < 0.0f ||
      profile->loudness.max_bass_db > 12.0f ||
      profile->loudness.max_treble_db < 0.0f ||
      profile->loudness.max_treble_db > 6.0f ||
      profile->loudness.full_effect_below_db > -10.0f ||
      profile->loudness.full_effect_below_db < -80.0f) {
    return ESP_ERR_INVALID_ARG;
  }
  return ESP_OK;
}

static void normalize_biquad(biquad_t *bq, float b0, float b1, float b2,
                             float a0, float a1, float a2) {
  float inv = 1.0f / a0;
  bq->b0 = b0 * inv;
  bq->b1 = b1 * inv;
  bq->b2 = b2 * inv;
  bq->a1 = a1 * inv;
  bq->a2 = a2 * inv;
  bq->z1 = 0.0f;
  bq->z2 = 0.0f;
}

static void design_biquad(const cal_dsp_filter_t *filter, uint32_t rate,
                          biquad_t *bq) {
  float w0 = 2.0f * PI_F * filter->frequency_hz / (float)rate;
  float cosine = cosf(w0);
  float sine = sinf(w0);
  float alpha = sine / (2.0f * filter->q);
  float a = powf(10.0f, filter->gain_db / 40.0f);
  float sqrt_a = sqrtf(a);

  switch (filter->type) {
  case CAL_DSP_FILTER_LOW_SHELF:
    normalize_biquad(
        bq, a * ((a + 1.0f) - (a - 1.0f) * cosine + 2.0f * sqrt_a * alpha),
        2.0f * a * ((a - 1.0f) - (a + 1.0f) * cosine),
        a * ((a + 1.0f) - (a - 1.0f) * cosine - 2.0f * sqrt_a * alpha),
        (a + 1.0f) + (a - 1.0f) * cosine + 2.0f * sqrt_a * alpha,
        -2.0f * ((a - 1.0f) + (a + 1.0f) * cosine),
        (a + 1.0f) + (a - 1.0f) * cosine - 2.0f * sqrt_a * alpha);
    break;
  case CAL_DSP_FILTER_HIGH_SHELF:
    normalize_biquad(
        bq, a * ((a + 1.0f) + (a - 1.0f) * cosine + 2.0f * sqrt_a * alpha),
        -2.0f * a * ((a - 1.0f) + (a + 1.0f) * cosine),
        a * ((a + 1.0f) + (a - 1.0f) * cosine - 2.0f * sqrt_a * alpha),
        (a + 1.0f) - (a - 1.0f) * cosine + 2.0f * sqrt_a * alpha,
        2.0f * ((a - 1.0f) - (a + 1.0f) * cosine),
        (a + 1.0f) - (a - 1.0f) * cosine - 2.0f * sqrt_a * alpha);
    break;
  case CAL_DSP_FILTER_HIGH_PASS:
    normalize_biquad(bq, (1.0f + cosine) * 0.5f, -(1.0f + cosine),
                     (1.0f + cosine) * 0.5f, 1.0f + alpha, -2.0f * cosine,
                     1.0f - alpha);
    break;
  case CAL_DSP_FILTER_LOW_PASS:
    normalize_biquad(bq, (1.0f - cosine) * 0.5f, 1.0f - cosine,
                     (1.0f - cosine) * 0.5f, 1.0f + alpha, -2.0f * cosine,
                     1.0f - alpha);
    break;
  case CAL_DSP_FILTER_PEAK:
  default:
    normalize_biquad(bq, 1.0f + alpha * a, -2.0f * cosine, 1.0f - alpha * a,
                     1.0f + alpha / a, -2.0f * cosine, 1.0f - alpha / a);
    break;
  }
}

static float biquad_magnitude(const biquad_t *bq, float radians) {
  float c1 = cosf(radians);
  float s1 = sinf(radians);
  float c2 = cosf(2.0f * radians);
  float s2 = sinf(2.0f * radians);
  float nr = bq->b0 + bq->b1 * c1 + bq->b2 * c2;
  float ni = -bq->b1 * s1 - bq->b2 * s2;
  float dr = 1.0f + bq->a1 * c1 + bq->a2 * c2;
  float di = -bq->a1 * s1 - bq->a2 * s2;
  float denominator = dr * dr + di * di;
  return denominator > FLT_MIN ? sqrtf((nr * nr + ni * ni) / denominator)
                               : 1.0f;
}

static float compute_cascade_peak_db(void) {
  float peak_db = 0.0f;
  for (size_t channel = 0; channel < CAL_DSP_CHANNELS; channel++) {
    for (size_t point = 0; point < RESPONSE_POINTS; point++) {
      float t = (float)point / (float)(RESPONSE_POINTS - 1U);
      float frequency = 20.0f * powf(1000.0f, t);
      if (frequency > (float)s_dsp.profile.sample_rate * 0.45f) {
        frequency = (float)s_dsp.profile.sample_rate * 0.45f;
      }
      float radians =
          2.0f * PI_F * frequency / (float)s_dsp.profile.sample_rate;
      float total_db = s_dsp.profile.channels[channel].gain_db;
      for (size_t i = 0; i < s_dsp.profile.channels[channel].filter_count;
           i++) {
        if (s_dsp.profile.channels[channel].filters[i].enabled) {
          total_db +=
              gain_to_db(biquad_magnitude(&s_dsp.biquads[channel][i], radians));
        }
      }
      if (total_db > peak_db) {
        peak_db = total_db;
      }
    }
  }
  return peak_db;
}

static void rebuild_state(void) {
  memset(s_dsp.biquads, 0, sizeof(s_dsp.biquads));
  memset(s_dsp.delay, 0, sizeof(s_dsp.delay));
  memset(s_dsp.delay_pos, 0, sizeof(s_dsp.delay_pos));
  memset(s_dsp.limiter_audio, 0, sizeof(s_dsp.limiter_audio));
  memset(s_dsp.limiter_peak, 0, sizeof(s_dsp.limiter_peak));
  memset(s_dsp.limiter_deque, 0, sizeof(s_dsp.limiter_deque));
  memset(s_dsp.loudness_biquads, 0, sizeof(s_dsp.loudness_biquads));
  s_dsp.limiter_head = 0;
  s_dsp.limiter_tail = 0;
  s_dsp.limiter_sequence = 0;
  s_dsp.limiter_gain = 1.0f;
  s_dsp.loudness_bass_db = -1000.0f;
  s_dsp.loudness_treble_db = -1000.0f;

  for (size_t channel = 0; channel < CAL_DSP_CHANNELS; channel++) {
    const cal_dsp_channel_t *ch = &s_dsp.profile.channels[channel];
    s_dsp.delay_length[channel] =
        (size_t)lroundf(ch->delay_ms * s_dsp.profile.sample_rate / 1000.0f);
    for (size_t i = 0; i < ch->filter_count; i++) {
      design_biquad(&ch->filters[i], s_dsp.profile.sample_rate,
                    &s_dsp.biquads[channel][i]);
    }
  }

  s_dsp.cascade_peak_db = compute_cascade_peak_db();
  s_dsp.effective_preamp_db = s_dsp.profile.requested_preamp_db;
  if (s_dsp.profile.auto_headroom && s_dsp.cascade_peak_db > 0.0f) {
    float safe = -(s_dsp.cascade_peak_db + s_dsp.profile.headroom_margin_db);
    if (safe < s_dsp.effective_preamp_db) {
      s_dsp.effective_preamp_db = safe;
    }
  }

  if (s_dsp.profile.limiter.enabled) {
    s_dsp.limiter_lookahead =
        (size_t)lroundf(s_dsp.profile.limiter.lookahead_ms *
                        s_dsp.profile.sample_rate / 1000.0f);
    if (s_dsp.limiter_lookahead >= CAL_DSP_LOOKAHEAD_SAMPLES) {
      s_dsp.limiter_lookahead = CAL_DSP_LOOKAHEAD_SAMPLES - 1U;
    }
    s_dsp.limiter_ceiling = db_to_gain(s_dsp.profile.limiter.ceiling_dbfs);
    float release_samples =
        s_dsp.profile.limiter.release_ms * s_dsp.profile.sample_rate / 1000.0f;
    s_dsp.limiter_release = expf(-1.0f / release_samples);
  } else {
    s_dsp.limiter_lookahead = 0;
    s_dsp.limiter_ceiling = 1.0f;
    s_dsp.limiter_release = 0.0f;
  }

  s_dsp.metrics.profile_hash = calibration_dsp_profile_hash(&s_dsp.profile);
  s_dsp.metrics.sample_rate = s_dsp.profile.sample_rate;
  s_dsp.metrics.effective_preamp_db = s_dsp.effective_preamp_db;
  s_dsp.metrics.cascade_peak_db = s_dsp.cascade_peak_db;
}

esp_err_t calibration_dsp_init(uint32_t sample_rate) {
  if (!s_lock) {
    s_lock = xSemaphoreCreateMutex();
  }
  if (!s_lock) {
    return ESP_ERR_NO_MEM;
  }

  cal_dsp_profile_t profile;
  memset(&profile, 0, sizeof(profile));
  size_t size = sizeof(profile);
  if (settings_get_dsp_profile(&profile, &size) != ESP_OK ||
      !calibration_dsp_upgrade_profile(&profile, size, sample_rate) ||
      validate_profile(&profile) != ESP_OK) {
    calibration_dsp_default_profile(&profile, sample_rate);
  }
  memset(&s_dsp, 0, sizeof(s_dsp));
  s_dsp.volume_q15_current = -1;
  s_dsp.measurement_volume_q15 = 32768;
  s_dsp.dither_state = 0x9e3779b9U;
  s_dsp.profile = profile;
  s_dsp.bypass = !profile.enabled;
  rebuild_state();
  s_dsp.ready = true;
  ESP_LOGI(TAG, "DSP ready: %s, hash=%08" PRIx32 ", preamp=%.2f dB",
           s_dsp.bypass ? "bypassed" : "active", s_dsp.metrics.profile_hash,
           s_dsp.effective_preamp_db);
  return ESP_OK;
}

esp_err_t calibration_dsp_set_profile(const cal_dsp_profile_t *profile) {
  esp_err_t err = validate_profile(profile);
  if (err != ESP_OK || !s_lock) {
    return err != ESP_OK ? err : ESP_ERR_INVALID_STATE;
  }
  xSemaphoreTake(s_lock, portMAX_DELAY);
  float transition_from[CAL_DSP_CHANNELS] = {s_dsp.last_output[0],
                                             s_dsp.last_output[1]};
  s_dsp.profile = *profile;
  s_dsp.bypass = !profile->enabled;
  rebuild_state();
  memcpy(s_dsp.transition_from, transition_from, sizeof(transition_from));
  s_dsp.transition_total = profile->sample_rate / 20U;
  s_dsp.transition_remaining = s_dsp.transition_total;
  xSemaphoreGive(s_lock);
  return ESP_OK;
}

void calibration_dsp_get_profile(cal_dsp_profile_t *profile) {
  if (!profile || !s_lock) {
    return;
  }
  xSemaphoreTake(s_lock, portMAX_DELAY);
  *profile = s_dsp.profile;
  xSemaphoreGive(s_lock);
}

void calibration_dsp_set_bypass(bool bypass) {
  if (!s_lock) {
    return;
  }
  xSemaphoreTake(s_lock, portMAX_DELAY);
  if (s_dsp.bypass != bypass) {
    float transition_from[CAL_DSP_CHANNELS] = {s_dsp.last_output[0],
                                               s_dsp.last_output[1]};
    s_dsp.bypass = bypass;
    rebuild_state();
    memcpy(s_dsp.transition_from, transition_from, sizeof(transition_from));
    s_dsp.transition_total = s_dsp.profile.sample_rate / 20U;
    s_dsp.transition_remaining = s_dsp.transition_total;
  }
  s_dsp.metrics.bypassed = bypass;
  xSemaphoreGive(s_lock);
}

bool calibration_dsp_get_bypass(void) {
  if (!s_lock) {
    return true;
  }
  xSemaphoreTake(s_lock, portMAX_DELAY);
  bool bypass = s_dsp.bypass;
  xSemaphoreGive(s_lock);
  return bypass;
}

static float process_biquad(biquad_t *bq, float sample) {
  float output = bq->b0 * sample + bq->z1;
  bq->z1 = bq->b1 * sample - bq->a1 * output + bq->z2;
  bq->z2 = bq->b2 * sample - bq->a2 * output;
  return output;
}

static float process_delay(size_t channel, float sample) {
  size_t length = s_dsp.delay_length[channel];
  if (length == 0) {
    return sample;
  }
  size_t pos = s_dsp.delay_pos[channel];
  float output = s_dsp.delay[channel][pos];
  s_dsp.delay[channel][pos] = sample;
  s_dsp.delay_pos[channel] = (pos + 1U) % length;
  return output;
}

static float ramp_volume(int32_t target_q15) {
  target_q15 = target_q15 < 0 ? 0 : target_q15 > 32768 ? 32768 : target_q15;
  if (s_dsp.volume_q15_current < 0) {
    s_dsp.volume_q15_current = target_q15;
  } else if (s_dsp.volume_q15_current != target_q15) {
    int32_t difference = target_q15 - s_dsp.volume_q15_current;
    int32_t step = difference / 256;
    if (step == 0) {
      step = difference > 0 ? 1 : -1;
    }
    s_dsp.volume_q15_current += step;
  }
  return (float)s_dsp.volume_q15_current / 32768.0f;
}

static void update_loudness(float volume_gain) {
  if (!s_dsp.profile.loudness.enabled || s_dsp.measurement_mode) {
    return;
  }
  float volume_db = gain_to_db(volume_gain);
  float denominator = -s_dsp.profile.loudness.full_effect_below_db;
  float amount =
      denominator > 1.0f ? clampf(-volume_db / denominator, 0.0f, 1.0f) : 0.0f;
  float bass_db = amount * s_dsp.profile.loudness.max_bass_db;
  float treble_db = amount * s_dsp.profile.loudness.max_treble_db;
  if (fabsf(bass_db - s_dsp.loudness_bass_db) < 0.25f &&
      fabsf(treble_db - s_dsp.loudness_treble_db) < 0.25f) {
    return;
  }
  s_dsp.loudness_bass_db = bass_db;
  s_dsp.loudness_treble_db = treble_db;
  for (size_t channel = 0; channel < CAL_DSP_CHANNELS; channel++) {
    const float gains[2] = {bass_db, treble_db};
    const float frequencies[2] = {120.0f, 8000.0f};
    const cal_dsp_filter_type_t types[2] = {CAL_DSP_FILTER_LOW_SHELF,
                                            CAL_DSP_FILTER_HIGH_SHELF};
    for (size_t band = 0; band < 2; band++) {
      cal_dsp_filter_t filter = {.enabled = true,
                                 .type = types[band],
                                 .frequency_hz = frequencies[band],
                                 .gain_db = gains[band],
                                 .q = 0.707f};
      biquad_t designed;
      design_biquad(&filter, s_dsp.profile.sample_rate, &designed);
      designed.z1 = s_dsp.loudness_biquads[channel][band].z1;
      designed.z2 = s_dsp.loudness_biquads[channel][band].z2;
      s_dsp.loudness_biquads[channel][band] = designed;
    }
  }
}

static uint32_t dither_random(void) {
  uint32_t x = s_dsp.dither_state;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  s_dsp.dither_state = x;
  return x;
}

static int32_t quantize_24(float sample) {
  float u1 = (float)(dither_random() & 0xffffU) / 65535.0f;
  float u2 = (float)(dither_random() & 0xffffU) / 65535.0f;
  float dither = (u1 - u2) / 8388608.0f;
  float scaled = clampf(sample + dither, -1.0f, 0.99999988f) * 8388608.0f;
  int32_t q24 = (int32_t)lroundf(scaled);
  if (q24 < -8388608)
    q24 = -8388608;
  if (q24 > 8388607)
    q24 = 8388607;
  return q24 * 256;
}

static void limiter_push(float input[CAL_DSP_CHANNELS],
                         float output[CAL_DSP_CHANNELS]) {
  const size_t capacity = CAL_DSP_LOOKAHEAD_SAMPLES;
  uint64_t sequence = s_dsp.limiter_sequence++;
  size_t position = (size_t)(sequence % capacity);
  float peak = fmaxf(fabsf(input[0]), fabsf(input[1]));
  s_dsp.limiter_audio[position][0] = input[0];
  s_dsp.limiter_audio[position][1] = input[1];
  s_dsp.limiter_peak[position] = peak;

  uint64_t oldest = sequence > s_dsp.limiter_lookahead
                        ? sequence - s_dsp.limiter_lookahead
                        : 0;
  while (s_dsp.limiter_head < s_dsp.limiter_tail &&
         s_dsp.limiter_deque[s_dsp.limiter_head % capacity] < oldest) {
    s_dsp.limiter_head++;
  }
  while (s_dsp.limiter_head < s_dsp.limiter_tail) {
    uint64_t tail_sequence =
        s_dsp.limiter_deque[(s_dsp.limiter_tail - 1U) % capacity];
    if (s_dsp.limiter_peak[tail_sequence % capacity] > peak) {
      break;
    }
    s_dsp.limiter_tail--;
  }
  s_dsp.limiter_deque[s_dsp.limiter_tail % capacity] = sequence;
  s_dsp.limiter_tail++;

  uint64_t peak_sequence = s_dsp.limiter_deque[s_dsp.limiter_head % capacity];
  float window_peak = s_dsp.limiter_peak[peak_sequence % capacity];
  float target = window_peak > s_dsp.limiter_ceiling
                     ? s_dsp.limiter_ceiling / window_peak
                     : 1.0f;
  if (target < s_dsp.limiter_gain) {
    s_dsp.limiter_gain = target;
  } else {
    s_dsp.limiter_gain =
        target + s_dsp.limiter_release * (s_dsp.limiter_gain - target);
  }

  if (sequence < s_dsp.limiter_lookahead) {
    output[0] = 0.0f;
    output[1] = 0.0f;
  } else {
    size_t output_pos =
        (size_t)((sequence - s_dsp.limiter_lookahead) % capacity);
    output[0] = s_dsp.limiter_audio[output_pos][0] * s_dsp.limiter_gain;
    output[1] = s_dsp.limiter_audio[output_pos][1] * s_dsp.limiter_gain;
  }
  if (s_dsp.limiter_gain < 0.9999f) {
    s_dsp.metrics.limited_frames++;
  }
}

static void calibration_dsp_process_common(int16_t *pcm, int32_t *pcm_i32,
                                           size_t frames,
                                           int32_t software_volume_q15) {
  if (!pcm || frames == 0 || !s_lock || !s_dsp.ready) {
    return;
  }
  int64_t started = esp_timer_get_time();
  xSemaphoreTake(s_lock, portMAX_DELAY);
  if (s_dsp.test_tone && s_dsp.test_tone_expires_us > 0 &&
      esp_timer_get_time() >= s_dsp.test_tone_expires_us) {
    s_dsp.test_tone = false;
  }
  if (s_dsp.sync_test && s_dsp.sync_test_expires_us > 0 &&
      esp_timer_get_time() >= s_dsp.sync_test_expires_us) {
    s_dsp.sync_test = false;
  }

  float preamp = db_to_gain(s_dsp.effective_preamp_db);
  for (size_t frame = 0; frame < frames; frame++) {
    int32_t target_volume = s_dsp.measurement_mode
                                ? s_dsp.measurement_volume_q15
                                : software_volume_q15;
    float volume = ramp_volume(target_volume);
    update_loudness(volume);
    float samples[CAL_DSP_CHANNELS];
    for (size_t channel = 0; channel < CAL_DSP_CHANNELS; channel++) {
      const cal_dsp_channel_t *ch = &s_dsp.profile.channels[channel];
      float sample;
      if (s_dsp.sync_test) {
        bool channel_enabled =
            (s_dsp.sync_test_channel_mask & (1U << channel)) != 0;
        if (channel_enabled &&
            s_dsp.sync_test_position < s_dsp.sync_test_pulse_samples) {
          float phase = 2.0f * PI_F * 2000.0f *
                        (float)s_dsp.sync_test_position /
                        (float)s_dsp.profile.sample_rate;
          /* Hann envelope keeps the calibration marker pop-free while its
           * leading edge remains easy for correlation to detect. */
          float envelope =
              0.5f - 0.5f * cosf(2.0f * PI_F * (float)s_dsp.sync_test_position /
                                 (float)s_dsp.sync_test_pulse_samples);
          sample = sinf(phase) * envelope * s_dsp.sync_test_gain;
        } else {
          sample = 0.0f;
        }
      } else if (s_dsp.test_tone) {
        bool channel_enabled =
            (s_dsp.test_tone_channel_mask & (1U << channel)) != 0;
        sample = channel_enabled
                     ? sinf(s_dsp.test_tone_phase) * s_dsp.test_tone_gain
                     : 0.0f;
      } else {
        sample = (float)pcm[frame * 2U + channel] / 32768.0f;
      }

      if (s_dsp.bypass) {
        samples[channel] = sample * volume;
      } else {
        sample *= preamp * db_to_gain(ch->gain_db) *
                  (ch->polarity < 0 ? -1.0f : 1.0f);
        if (s_dsp.profile.loudness.enabled && !s_dsp.measurement_mode) {
          sample = process_biquad(&s_dsp.loudness_biquads[channel][0], sample);
          sample = process_biquad(&s_dsp.loudness_biquads[channel][1], sample);
        }
        for (size_t i = 0; i < ch->filter_count; i++) {
          if (ch->filters[i].enabled) {
            sample = process_biquad(&s_dsp.biquads[channel][i], sample);
          }
        }
        samples[channel] = process_delay(channel, sample) * volume;
      }
    }
    if (s_dsp.test_tone) {
      s_dsp.test_tone_phase += 2.0f * PI_F * s_dsp.test_tone_frequency_hz /
                               (float)s_dsp.profile.sample_rate;
      if (s_dsp.test_tone_phase >= 2.0f * PI_F) {
        s_dsp.test_tone_phase -= 2.0f * PI_F;
      }
    }
    if (s_dsp.sync_test && s_dsp.sync_test_interval_samples > 0) {
      s_dsp.sync_test_position =
          (s_dsp.sync_test_position + 1U) % s_dsp.sync_test_interval_samples;
    }

    float limited[CAL_DSP_CHANNELS];
    if (!s_dsp.bypass && s_dsp.profile.limiter.enabled) {
      limiter_push(samples, limited);
    } else {
      limited[0] = samples[0];
      limited[1] = samples[1];
    }

    if (s_dsp.transition_remaining > 0 && s_dsp.transition_total > 0) {
      float progress = 1.0f - (float)s_dsp.transition_remaining /
                                  (float)s_dsp.transition_total;
      for (size_t channel = 0; channel < CAL_DSP_CHANNELS; channel++) {
        limited[channel] = s_dsp.transition_from[channel] * (1.0f - progress) +
                           limited[channel] * progress;
      }
      s_dsp.transition_remaining--;
    }

    for (size_t channel = 0; channel < CAL_DSP_CHANNELS; channel++) {
      if (fabsf(limited[channel]) > 1.0f) {
        s_dsp.metrics.clipped_samples++;
      }
      s_dsp.last_output[channel] = limited[channel];
      if (pcm_i32) {
        int32_t output = quantize_24(limited[channel]);
        pcm_i32[frame * 2U + channel] = output;
        pcm[frame * 2U + channel] = (int16_t)(output >> 16);
      } else {
        float quantized =
            clampf(limited[channel], -1.0f, 0.9999695f) * 32768.0f;
        pcm[frame * 2U + channel] = (int16_t)lroundf(quantized);
      }
    }
  }

  int64_t elapsed = esp_timer_get_time() - started;
  float audio_us =
      (float)frames * 1000000.0f / (float)s_dsp.profile.sample_rate;
  float load = audio_us > 0.0f ? 100.0f * (float)elapsed / audio_us : 0.0f;
  if (!s_dsp.bypass || s_dsp.test_tone) {
    s_dsp.metrics.frames_processed += frames;
  }
  s_dsp.metrics.limiter_gain_db = gain_to_db(s_dsp.limiter_gain);
  s_dsp.metrics.dsp_load_percent =
      s_dsp.metrics.dsp_load_percent * 0.95f + load * 0.05f;
  if (load > s_dsp.metrics.max_dsp_load_percent) {
    s_dsp.metrics.max_dsp_load_percent = load;
  }
  s_dsp.metrics.bypassed = s_dsp.bypass;
  s_dsp.metrics.transition_active = s_dsp.transition_remaining > 0;
  s_dsp.metrics.transition_remaining_ms =
      (uint32_t)((uint64_t)s_dsp.transition_remaining * 1000ULL /
                 s_dsp.profile.sample_rate);
  s_dsp.metrics.measurement_mode = s_dsp.measurement_mode;
  s_dsp.metrics.test_tone_active = s_dsp.test_tone;
  s_dsp.metrics.sync_test_active = s_dsp.sync_test;
  s_dsp.metrics.measurement_session_id = s_dsp.measurement_session_id;
  xSemaphoreGive(s_lock);
}

void calibration_dsp_process(int16_t *pcm, size_t frames,
                             int32_t software_volume_q15) {
  calibration_dsp_process_common(pcm, NULL, frames, software_volume_q15);
}

void calibration_dsp_process_i32(int16_t *pcm, int32_t *pcm_i32, size_t frames,
                                 int32_t software_volume_q15) {
  if (!pcm_i32) {
    return;
  }
  calibration_dsp_process_common(pcm, pcm_i32, frames, software_volume_q15);
}

esp_err_t calibration_dsp_set_measurement_mode(bool enabled,
                                               int32_t fixed_volume_q15,
                                               uint32_t expected_profile_hash) {
  if (!s_lock || fixed_volume_q15 < 0 || fixed_volume_q15 > 32768) {
    return ESP_ERR_INVALID_ARG;
  }
  xSemaphoreTake(s_lock, portMAX_DELAY);
  if (enabled && expected_profile_hash != 0 &&
      expected_profile_hash != s_dsp.metrics.profile_hash) {
    xSemaphoreGive(s_lock);
    return ESP_ERR_INVALID_CRC;
  }
  s_dsp.measurement_mode = enabled;
  s_dsp.measurement_volume_q15 = fixed_volume_q15;
  if (enabled) {
    s_dsp.measurement_session_id++;
  }
  s_dsp.metrics.measurement_mode = enabled;
  s_dsp.metrics.measurement_session_id = s_dsp.measurement_session_id;
  xSemaphoreGive(s_lock);
  return ESP_OK;
}

bool calibration_dsp_get_measurement_mode(uint32_t *session_id,
                                          int32_t *fixed_volume_q15) {
  if (!s_lock) {
    return false;
  }
  xSemaphoreTake(s_lock, portMAX_DELAY);
  bool enabled = s_dsp.measurement_mode;
  if (session_id)
    *session_id = s_dsp.measurement_session_id;
  if (fixed_volume_q15)
    *fixed_volume_q15 = s_dsp.measurement_volume_q15;
  xSemaphoreGive(s_lock);
  return enabled;
}

esp_err_t calibration_dsp_set_test_tone(bool enabled, float frequency_hz,
                                        float level_dbfs, uint8_t channel_mask,
                                        uint32_t duration_ms) {
  if (!s_lock || (enabled && (frequency_hz < 20.0f || frequency_hz > 20000.0f ||
                              level_dbfs > -12.0f || level_dbfs < -60.0f ||
                              channel_mask == 0 || channel_mask > 3 ||
                              duration_ms == 0 || duration_ms > 30000U))) {
    return ESP_ERR_INVALID_ARG;
  }
  xSemaphoreTake(s_lock, portMAX_DELAY);
  s_dsp.test_tone = enabled;
  if (enabled) {
    s_dsp.sync_test = false;
    s_dsp.test_tone_frequency_hz = frequency_hz;
    s_dsp.test_tone_gain = db_to_gain(level_dbfs);
    s_dsp.test_tone_channel_mask = channel_mask;
    s_dsp.test_tone_phase = 0.0f;
    s_dsp.test_tone_expires_us =
        esp_timer_get_time() + (int64_t)duration_ms * 1000LL;
  } else {
    s_dsp.test_tone_expires_us = 0;
  }
  s_dsp.metrics.test_tone_active = enabled;
  xSemaphoreGive(s_lock);
  return ESP_OK;
}

bool calibration_dsp_test_tone_active(void) {
  if (!s_lock)
    return false;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  bool active =
      s_dsp.test_tone && (s_dsp.test_tone_expires_us == 0 ||
                          esp_timer_get_time() < s_dsp.test_tone_expires_us);
  if (!active)
    s_dsp.test_tone = false;
  xSemaphoreGive(s_lock);
  return active;
}

esp_err_t calibration_dsp_set_sync_test(bool enabled, uint32_t interval_ms,
                                        uint32_t pulse_ms, float level_dbfs,
                                        uint8_t channel_mask,
                                        uint32_t duration_ms) {
  if (!s_lock ||
      (enabled &&
       (interval_ms < 250U || interval_ms > 5000U || pulse_ms < 5U ||
        pulse_ms > 50U || pulse_ms >= interval_ms || level_dbfs > -18.0f ||
        level_dbfs < -60.0f || channel_mask == 0 || channel_mask > 3 ||
        duration_ms == 0 || duration_ms > 60000U))) {
    return ESP_ERR_INVALID_ARG;
  }
  xSemaphoreTake(s_lock, portMAX_DELAY);
  s_dsp.sync_test = enabled;
  if (enabled) {
    s_dsp.test_tone = false;
    s_dsp.sync_test_gain = db_to_gain(level_dbfs);
    s_dsp.sync_test_channel_mask = channel_mask;
    s_dsp.sync_test_interval_samples =
        (uint32_t)((uint64_t)s_dsp.profile.sample_rate * interval_ms / 1000U);
    s_dsp.sync_test_pulse_samples =
        (uint32_t)((uint64_t)s_dsp.profile.sample_rate * pulse_ms / 1000U);
    s_dsp.sync_test_position = 0;
    s_dsp.sync_test_expires_us =
        esp_timer_get_time() + (int64_t)duration_ms * 1000LL;
  } else {
    s_dsp.sync_test_expires_us = 0;
  }
  s_dsp.metrics.sync_test_active = enabled;
  xSemaphoreGive(s_lock);
  return ESP_OK;
}

bool calibration_dsp_signal_generator_active(void) {
  if (!s_lock)
    return false;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  int64_t now = esp_timer_get_time();
  bool tone = s_dsp.test_tone && (s_dsp.test_tone_expires_us == 0 ||
                                  now < s_dsp.test_tone_expires_us);
  bool sync = s_dsp.sync_test && (s_dsp.sync_test_expires_us == 0 ||
                                  now < s_dsp.sync_test_expires_us);
  if (!tone)
    s_dsp.test_tone = false;
  if (!sync)
    s_dsp.sync_test = false;
  xSemaphoreGive(s_lock);
  return tone || sync;
}

void calibration_dsp_reset(void) {
  if (!s_lock) {
    return;
  }
  xSemaphoreTake(s_lock, portMAX_DELAY);
  rebuild_state();
  xSemaphoreGive(s_lock);
}

uint32_t calibration_dsp_get_latency_us(void) {
  if (!s_lock) {
    return 0;
  }
  xSemaphoreTake(s_lock, portMAX_DELAY);
  uint32_t latency = (!s_dsp.bypass && s_dsp.profile.limiter.enabled)
                         ? (uint32_t)((uint64_t)s_dsp.limiter_lookahead *
                                      1000000ULL / s_dsp.profile.sample_rate)
                         : 0;
  xSemaphoreGive(s_lock);
  return latency;
}

int32_t calibration_dsp_get_output_latency_trim_us(void) {
  if (!s_lock)
    return 0;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  int32_t trim = s_dsp.ready ? s_dsp.profile.output_latency_trim_us : 0;
  xSemaphoreGive(s_lock);
  return trim;
}

void calibration_dsp_get_metrics(cal_dsp_metrics_t *metrics) {
  if (!metrics || !s_lock) {
    return;
  }
  xSemaphoreTake(s_lock, portMAX_DELAY);
  *metrics = s_dsp.metrics;
  metrics->bypassed = s_dsp.bypass;
  xSemaphoreGive(s_lock);
}
