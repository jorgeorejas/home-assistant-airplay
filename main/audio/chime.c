#include "chime.h"

#include "esp_log.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <string.h>

static const char *TAG = "chime";

/* The PCM blob (raw S16LE 44.1 kHz stereo) is linked in via EMBED_FILES
   in main/CMakeLists.txt. Currently the macOS MagSafe connect chime —
   ascending bell arpeggio that fits the "connection just happened"
   semantic AirPlay needs. Swap by overwriting main/audio/chime.pcm. */
extern const uint8_t chime_pcm_start[] asm("_binary_chime_pcm_start");
extern const uint8_t chime_pcm_end[] asm("_binary_chime_pcm_end");

#define BYTES_PER_FRAME 4 /* 2 ch × 16 bit */

static const int16_t *s_samples = NULL;
static size_t s_total_frames = 0;

/* `s_pos` is a frame-aligned cursor. The audio task is the sole writer
   when active; the control plane resets it to 0 in `chime_play()`. The
   `s_active` atomic publishes the (re)start. */
static size_t s_pos = 0;
static atomic_bool s_active = false;

void chime_init(void) {
  if (s_samples) {
    return;
  }
  s_samples = (const int16_t *)chime_pcm_start;
  size_t bytes = (size_t)(chime_pcm_end - chime_pcm_start);
  s_total_frames = bytes / BYTES_PER_FRAME;
  ESP_LOGI(TAG, "chime ready: %u frames (%.2fs @ 44.1kHz stereo)",
           (unsigned)s_total_frames, (double)s_total_frames / 44100.0);
}

void chime_play(void) {
  if (!s_samples) {
    return;
  }
  /* Disarm first so a torn read on the audio side picks up either the
     old end-state or the new start, never a stale midpoint. */
  atomic_store_explicit(&s_active, false, memory_order_release);
  s_pos = 0;
  atomic_store_explicit(&s_active, true, memory_order_release);
}

void chime_stop(void) {
  atomic_store_explicit(&s_active, false, memory_order_release);
  s_pos = 0;
}

size_t chime_consume(int16_t *out, size_t stereo_frames) {
  if (!atomic_load_explicit(&s_active, memory_order_acquire)) {
    return 0;
  }
  if (s_pos >= s_total_frames) {
    atomic_store_explicit(&s_active, false, memory_order_release);
    return 0;
  }

  size_t available = s_total_frames - s_pos;
  size_t to_copy = available < stereo_frames ? available : stereo_frames;

  /* Halve every sample (-6 dB) so the chime doesn't sit at full digital
     scale relative to typical music programme material. The codec's
     hardware volume register applies on top. Right shift by 1 with
     rounding-to-zero is fine for chime quality. */
  const int16_t *src = s_samples + s_pos * 2;
  for (size_t i = 0; i < to_copy * 2; i++) {
    out[i] = (int16_t)(src[i] >> 1);
  }
  s_pos += to_copy;

  if (s_pos >= s_total_frames) {
    atomic_store_explicit(&s_active, false, memory_order_release);
  }
  return to_copy;
}
