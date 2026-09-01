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
      profile->headroom_margin_db > 6.0f) {
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
  s_dsp.limiter_head = 0;
  s_dsp.limiter_tail = 0;
  s_dsp.limiter_sequence = 0;
  s_dsp.limiter_gain = 1.0f;

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
  size_t size = sizeof(profile);
  if (settings_get_dsp_profile(&profile, &size) != ESP_OK ||
      size != sizeof(profile) || profile.sample_rate != sample_rate ||
      validate_profile(&profile) != ESP_OK) {
    calibration_dsp_default_profile(&profile, sample_rate);
  }
  memset(&s_dsp, 0, sizeof(s_dsp));
  s_dsp.volume_q15_current = -1;
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
  s_dsp.profile = *profile;
  s_dsp.bypass = !profile->enabled;
  rebuild_state();
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
    s_dsp.bypass = bypass;
    rebuild_state();
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

void calibration_dsp_process(int16_t *pcm, size_t frames,
                             int32_t software_volume_q15) {
  if (!pcm || frames == 0 || !s_lock || !s_dsp.ready) {
    return;
  }
  int64_t started = esp_timer_get_time();
  xSemaphoreTake(s_lock, portMAX_DELAY);
  if (s_dsp.bypass) {
    for (size_t frame = 0; frame < frames; frame++) {
      float volume = ramp_volume(software_volume_q15);
      for (size_t channel = 0; channel < CAL_DSP_CHANNELS; channel++) {
        float sample = (float)pcm[frame * 2U + channel] * volume;
        pcm[frame * 2U + channel] = (int16_t)lroundf(sample);
      }
    }
    s_dsp.metrics.bypassed = true;
    xSemaphoreGive(s_lock);
    return;
  }

  float preamp = db_to_gain(s_dsp.effective_preamp_db);
  for (size_t frame = 0; frame < frames; frame++) {
    float volume = ramp_volume(software_volume_q15);
    float samples[CAL_DSP_CHANNELS];
    for (size_t channel = 0; channel < CAL_DSP_CHANNELS; channel++) {
      const cal_dsp_channel_t *ch = &s_dsp.profile.channels[channel];
      float sample = (float)pcm[frame * 2U + channel] / 32768.0f;
      sample *=
          preamp * db_to_gain(ch->gain_db) * (ch->polarity < 0 ? -1.0f : 1.0f);
      for (size_t i = 0; i < ch->filter_count; i++) {
        if (ch->filters[i].enabled) {
          sample = process_biquad(&s_dsp.biquads[channel][i], sample);
        }
      }
      samples[channel] = process_delay(channel, sample) * volume;
    }

    float limited[CAL_DSP_CHANNELS];
    if (s_dsp.profile.limiter.enabled) {
      limiter_push(samples, limited);
    } else {
      limited[0] = samples[0];
      limited[1] = samples[1];
    }
    for (size_t channel = 0; channel < CAL_DSP_CHANNELS; channel++) {
      if (fabsf(limited[channel]) > 1.0f) {
        s_dsp.metrics.clipped_samples++;
      }
      float quantized = clampf(limited[channel], -1.0f, 0.9999695f) * 32768.0f;
      pcm[frame * 2U + channel] = (int16_t)lroundf(quantized);
    }
  }

  int64_t elapsed = esp_timer_get_time() - started;
  float audio_us =
      (float)frames * 1000000.0f / (float)s_dsp.profile.sample_rate;
  float load = audio_us > 0.0f ? 100.0f * (float)elapsed / audio_us : 0.0f;
  s_dsp.metrics.frames_processed += frames;
  s_dsp.metrics.limiter_gain_db = gain_to_db(s_dsp.limiter_gain);
  s_dsp.metrics.dsp_load_percent =
      s_dsp.metrics.dsp_load_percent * 0.95f + load * 0.05f;
  if (load > s_dsp.metrics.max_dsp_load_percent) {
    s_dsp.metrics.max_dsp_load_percent = load;
  }
  s_dsp.metrics.bypassed = false;
  xSemaphoreGive(s_lock);
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

void calibration_dsp_get_metrics(cal_dsp_metrics_t *metrics) {
  if (!metrics || !s_lock) {
    return;
  }
  xSemaphoreTake(s_lock, portMAX_DELAY);
  *metrics = s_dsp.metrics;
  metrics->bypassed = s_dsp.bypass;
  xSemaphoreGive(s_lock);
}
