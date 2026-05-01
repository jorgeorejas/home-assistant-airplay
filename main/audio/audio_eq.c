#include "audio_eq.h"

#include "eq_events.h"
#include "esp_log.h"
#include "settings.h"

#include <math.h>
#include <stdatomic.h>
#include <string.h>

static const char *TAG = "audio_eq";

/* Center frequencies match the TAS58XX EQ layout so the same web UI and
   NVS blob work unchanged. */
static const float c_band_freq[AUDIO_EQ_BANDS] = {
    20.0f,  31.5f,   50.0f,   80.0f,   125.0f,  200.0f,  315.0f,   500.0f,
    800.0f, 1250.0f, 2000.0f, 3150.0f, 5000.0f, 8000.0f, 16000.0f,
};

#define EQ_Q          1.41f /* matches the TAS58XX UI's perceived bandwidth */
#define EPSILON_DB    0.05f /* below this, the band is considered flat */

typedef struct {
  float b0, b1, b2;
  float a1, a2; /* a0 normalized */
} biquad_coeff_t;

typedef struct {
  float z1, z2;
} biquad_state_t;

/* Double-buffered coefficient table. The audio task reads from
   s_coeffs[s_active]; the listener writes the inactive slot then publishes
   it via an atomic store of s_active. */
static biquad_coeff_t s_coeffs[2][AUDIO_EQ_BANDS];
static atomic_int s_active = 0;
static atomic_bool s_any_nonflat = false;

/* Filter state belongs to the audio task — never touched by the control
   plane. [channel][band] */
static biquad_state_t s_state[2][AUDIO_EQ_BANDS];

static atomic_bool s_enabled = false;
static uint32_t s_sample_rate = 48000;
static float s_gains_db[AUDIO_EQ_BANDS];
static bool s_initialized = false;

/* RBJ peaking-EQ cookbook formula. */
static void compute_peaking(biquad_coeff_t *c, float fs, float f0, float Q,
                            float gain_db) {
  if (fabsf(gain_db) < EPSILON_DB) {
    /* Pass-through biquad: y = x. */
    c->b0 = 1.0f;
    c->b1 = 0.0f;
    c->b2 = 0.0f;
    c->a1 = 0.0f;
    c->a2 = 0.0f;
    return;
  }

  float A = powf(10.0f, gain_db / 40.0f);
  float w0 = 2.0f * (float)M_PI * f0 / fs;
  float cos_w0 = cosf(w0);
  float sin_w0 = sinf(w0);
  float alpha = sin_w0 / (2.0f * Q);

  float a0 = 1.0f + alpha / A;
  c->b0 = (1.0f + alpha * A) / a0;
  c->b1 = (-2.0f * cos_w0) / a0;
  c->b2 = (1.0f - alpha * A) / a0;
  c->a1 = (-2.0f * cos_w0) / a0;
  c->a2 = (1.0f - alpha / A) / a0;
}

static void recompute_all(int slot, const float gains_db[AUDIO_EQ_BANDS]) {
  bool any = false;
  for (int b = 0; b < AUDIO_EQ_BANDS; b++) {
    compute_peaking(&s_coeffs[slot][b], (float)s_sample_rate, c_band_freq[b],
                    EQ_Q, gains_db[b]);
    if (fabsf(gains_db[b]) >= EPSILON_DB) {
      any = true;
    }
  }
  atomic_store_explicit(&s_any_nonflat, any, memory_order_release);
}

static void publish(int next_slot) {
  atomic_store_explicit(&s_active, next_slot, memory_order_release);
}

static void on_eq_event(eq_event_t event, const eq_event_data_t *data,
                        void *user_data) {
  (void)user_data;

  switch (event) {
  case EQ_EVENT_ALL_BANDS_SET:
    if (data) {
      audio_eq_set_gains(data->all_bands.gains_db);
      settings_set_eq_gains(data->all_bands.gains_db);
      ESP_LOGI(TAG, "EQ: all bands updated and saved");
    }
    break;

  case EQ_EVENT_BAND_CHANGED:
    if (data) {
      float gains[AUDIO_EQ_BANDS];
      memcpy(gains, s_gains_db, sizeof(gains));
      gains[data->band_changed.band] = data->band_changed.gain_db;
      audio_eq_set_gains(gains);
      settings_set_eq_gains(gains);
    }
    break;

  case EQ_EVENT_FLAT:
    audio_eq_flat();
    settings_clear_eq();
    ESP_LOGI(TAG, "EQ: reset to flat");
    break;
  }
}

esp_err_t audio_eq_init(uint32_t sample_rate) {
  if (s_initialized) {
    return ESP_OK;
  }

  s_sample_rate = sample_rate ? sample_rate : 48000;
  memset(s_state, 0, sizeof(s_state));
  memset(s_gains_db, 0, sizeof(s_gains_db));

  /* Both slots flat at boot. */
  recompute_all(0, s_gains_db);
  recompute_all(1, s_gains_db);

  /* Restore saved gains. */
  float gains[AUDIO_EQ_BANDS];
  if (settings_get_eq_gains(gains) == ESP_OK) {
    memcpy(s_gains_db, gains, sizeof(gains));
    int next = atomic_load(&s_active) ^ 1;
    recompute_all(next, gains);
    publish(next);
    ESP_LOGI(TAG, "EQ: restored %d bands from NVS", AUDIO_EQ_BANDS);
  }

  eq_events_register(on_eq_event, NULL);
  atomic_store(&s_enabled, true);
  s_initialized = true;
  ESP_LOGI(TAG, "EQ ready: %d bands @ %lu Hz, Q=%.2f", AUDIO_EQ_BANDS,
           (unsigned long)s_sample_rate, (double)EQ_Q);
  return ESP_OK;
}

esp_err_t audio_eq_set_sample_rate(uint32_t sample_rate) {
  if (!sample_rate) {
    return ESP_ERR_INVALID_ARG;
  }
  if (sample_rate == s_sample_rate) {
    return ESP_OK;
  }
  s_sample_rate = sample_rate;
  int next = atomic_load(&s_active) ^ 1;
  recompute_all(next, s_gains_db);
  publish(next);
  return ESP_OK;
}

esp_err_t audio_eq_set_gains(const float gains_db[AUDIO_EQ_BANDS]) {
  if (!gains_db) {
    return ESP_ERR_INVALID_ARG;
  }

  memcpy(s_gains_db, gains_db, sizeof(s_gains_db));
  int next = atomic_load(&s_active) ^ 1;
  recompute_all(next, gains_db);
  publish(next);

  /* Reset filter state so a big gain swing doesn't ring forever. */
  memset(s_state, 0, sizeof(s_state));
  return ESP_OK;
}

esp_err_t audio_eq_flat(void) {
  float zero[AUDIO_EQ_BANDS] = {0};
  return audio_eq_set_gains(zero);
}

void audio_eq_set_enabled(bool enabled) {
  atomic_store(&s_enabled, enabled);
}

bool audio_eq_is_enabled(void) { return atomic_load(&s_enabled); }

float audio_eq_get_center_freq(int band) {
  if (band < 0 || band >= AUDIO_EQ_BANDS) {
    return 0.0f;
  }
  return c_band_freq[band];
}

/* ---------- hot path ---------- */

static inline float biquad_df2t(const biquad_coeff_t *c, biquad_state_t *s,
                                float x) {
  float y = c->b0 * x + s->z1;
  s->z1 = c->b1 * x - c->a1 * y + s->z2;
  s->z2 = c->b2 * x - c->a2 * y;
  return y;
}

void audio_eq_process(int16_t *pcm, size_t stereo_frames) {
  if (!atomic_load_explicit(&s_enabled, memory_order_acquire)) {
    return;
  }
  if (!atomic_load_explicit(&s_any_nonflat, memory_order_acquire)) {
    return;
  }

  int idx = atomic_load_explicit(&s_active, memory_order_acquire);
  const biquad_coeff_t *c = s_coeffs[idx];
  biquad_state_t *sL = s_state[0];
  biquad_state_t *sR = s_state[1];

  const float k_in = 1.0f / 32768.0f;
  const float k_out = 32768.0f;

  for (size_t i = 0; i < stereo_frames; i++) {
    float xL = (float)pcm[i * 2 + 0] * k_in;
    float xR = (float)pcm[i * 2 + 1] * k_in;

    for (int b = 0; b < AUDIO_EQ_BANDS; b++) {
      xL = biquad_df2t(&c[b], &sL[b], xL);
      xR = biquad_df2t(&c[b], &sR[b], xR);
    }

    xL *= k_out;
    xR *= k_out;
    if (xL > 32767.0f) {
      xL = 32767.0f;
    } else if (xL < -32768.0f) {
      xL = -32768.0f;
    }
    if (xR > 32767.0f) {
      xR = 32767.0f;
    } else if (xR < -32768.0f) {
      xR = -32768.0f;
    }

    pcm[i * 2 + 0] = (int16_t)xL;
    pcm[i * 2 + 1] = (int16_t)xR;
  }
}
