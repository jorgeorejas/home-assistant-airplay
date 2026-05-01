/**
 * Source-agnostic playback controller.
 *
 * For AirPlay:
 *   - Play/Pause: mute/unmute the DAC locally.  If the source sent
 *     DACP headers (AirPlay 1), we also forward the command via DACP
 *     so the source UI updates.  Modern iOS AirPlay 2 does not send
 *     DACP headers, so local mute is the only option.
 *   - Next/Prev: forwarded via DACP when available (AirPlay 1 only).
 *   - Volume: adjusted locally (DAC + NVS persistence) and mirrored
 *     to the source via DACP when available.
 *
 * For Bluetooth:
 *   - All commands sent as AVRCP passthrough to source device
 *   - Source device controls playback and sends volume back
 */

#include "playback_control.h"

#include "dac.h"
#include "dacp_client.h"
#include "rtsp_events.h"
#include "rtsp_server.h"
#include "settings.h"

#include "esp_log.h"

#ifdef CONFIG_BT_A2DP_ENABLE
#include "a2dp_sink.h"
#endif

static const char *TAG = "playback_ctrl";

#define VOLUME_MIN_DB  (-30.0f)
#define VOLUME_MAX_DB  0.0f
/* Non-linear curve: a single 3 dB step at low volume is comfortable, but
   the same step from -12 dB → -9 dB is a noticeable ~40 % SPL jump. Use
   a coarser step below this knee and a finer step above it. UX review L8. */
#define VOLUME_KNEE_DB         (-18.0f)
#define VOLUME_STEP_COARSE_DB    3.0f
#define VOLUME_STEP_FINE_DB      1.5f

static float volume_step_for(float current_db, bool going_up) {
  /* The step used to *exit* the coarse band should be coarse, and the
     step used to *enter* the fine band should be fine — pick based on
     the side of the knee we'll be on after the step. */
  float pivot = going_up ? current_db : current_db - VOLUME_STEP_FINE_DB;
  return pivot < VOLUME_KNEE_DB ? VOLUME_STEP_COARSE_DB
                                : VOLUME_STEP_FINE_DB;
}

static playback_source_t s_source = PLAYBACK_SOURCE_NONE;
static bool s_muted = false;
static float s_pre_mute_db = -15.0f;

esp_err_t playback_control_init(void) {
  dacp_init();
  ESP_LOGI(TAG, "Playback control initialized");
  return ESP_OK;
}

void playback_control_set_source(playback_source_t source) {
  s_muted = false;
  s_source = source;
  ESP_LOGI(TAG, "Source set to %d", source);
}

playback_source_t playback_control_get_source(void) {
  return s_source;
}

// ============================================================================
// AirPlay local volume helpers
// ============================================================================

static float clamp_volume(float db) {
  if (db < VOLUME_MIN_DB) {
    return VOLUME_MIN_DB;
  }
  if (db > VOLUME_MAX_DB) {
    return VOLUME_MAX_DB;
  }
  return db;
}

// Convert AirPlay dB (-30..0) to DACP percent (0..100)
static float db_to_dacp_percent(float db) {
  if (db <= VOLUME_MIN_DB) {
    return 0.0f;
  }
  if (db >= VOLUME_MAX_DB) {
    return 100.0f;
  }
  return ((db - VOLUME_MIN_DB) / (VOLUME_MAX_DB - VOLUME_MIN_DB)) * 100.0f;
}

static void airplay_adjust_volume(float step_db) {
  float current_db;
  if (settings_get_volume(&current_db) != ESP_OK) {
    current_db = -15.0f; // default 50 %
  }

  float new_db = clamp_volume(current_db + step_db);
  airplay_set_volume(new_db);

  if (s_muted) {
    // Update saved level so unmute restores the new volume
    s_pre_mute_db = new_db;
  } else {
    dac_set_volume(new_db);
  }

  ESP_LOGI(TAG, "AirPlay volume: %.1f -> %.1f dB%s", current_db, new_db,
           s_muted ? " (muted)" : "");

  // Notify AirPlay client via DACP (best-effort, don't block local action)
  dacp_send_volume(db_to_dacp_percent(new_db));
}

// ============================================================================
// Public API
// ============================================================================

void playback_control_play_pause(void) {
  switch (s_source) {
  case PLAYBACK_SOURCE_AIRPLAY: {
    if (dacp_is_active()) {
      // Tell the source to toggle playback — it will FLUSH the stream
      // on pause and RECORD on resume, so we don't need local muting.
      // Signal the v1 grace period loop (if active) so it sends the
      // DACP command at the right time and keeps waiting for reconnect.
      // If not in a grace period, the flag is harmless.
      rtsp_server_request_resume();
      dacp_send_playpause();
      ESP_LOGI(TAG, "AirPlay play/pause sent via DACP");
    } else {
      // Fallback: mute/unmute the DAC locally when no DACP session
      if (!s_muted) {
        if (settings_get_volume(&s_pre_mute_db) != ESP_OK) {
          s_pre_mute_db = -15.0f; // default 50 %
        }
        dac_set_volume(VOLUME_MIN_DB);
        s_muted = true;
        rtsp_events_emit(RTSP_EVENT_PAUSED, NULL);
        ESP_LOGI(TAG, "AirPlay muted locally (was %.1f dB)", s_pre_mute_db);
      } else {
        dac_set_volume(s_pre_mute_db);
        s_muted = false;
        rtsp_events_emit(RTSP_EVENT_PLAYING, NULL);
        ESP_LOGI(TAG, "AirPlay unmuted locally (%.1f dB)", s_pre_mute_db);
      }
    }
    break;
  }
#ifdef CONFIG_BT_A2DP_ENABLE
  case PLAYBACK_SOURCE_BLUETOOTH:
    bt_a2dp_send_playpause();
    break;
#endif
  default:
    ESP_LOGI(TAG, "Play/pause: no active source (source=%d)", s_source);
    break;
  }
}

void playback_control_volume_up(void) {
  switch (s_source) {
  case PLAYBACK_SOURCE_AIRPLAY: {
    float current_db;
    if (settings_get_volume(&current_db) != ESP_OK) {
      current_db = -15.0f;
    }
    airplay_adjust_volume(volume_step_for(current_db, true));
    break;
  }
#ifdef CONFIG_BT_A2DP_ENABLE
  case PLAYBACK_SOURCE_BLUETOOTH:
    bt_a2dp_send_volume_up();
    break;
#endif
  default:
    break;
  }
}

void playback_control_volume_down(void) {
  switch (s_source) {
  case PLAYBACK_SOURCE_AIRPLAY: {
    float current_db;
    if (settings_get_volume(&current_db) != ESP_OK) {
      current_db = -15.0f;
    }
    airplay_adjust_volume(-volume_step_for(current_db, false));
    break;
  }
#ifdef CONFIG_BT_A2DP_ENABLE
  case PLAYBACK_SOURCE_BLUETOOTH:
    bt_a2dp_send_volume_down();
    break;
#endif
  default:
    break;
  }
}

void playback_control_next(void) {
  switch (s_source) {
  case PLAYBACK_SOURCE_AIRPLAY:
    dacp_send_next();
    ESP_LOGI(TAG, "AirPlay next track via DACP");
    break;
#ifdef CONFIG_BT_A2DP_ENABLE
  case PLAYBACK_SOURCE_BLUETOOTH:
    bt_a2dp_send_next();
    break;
#endif
  default:
    break;
  }
}

void playback_control_prev(void) {
  switch (s_source) {
  case PLAYBACK_SOURCE_AIRPLAY:
    dacp_send_prev();
    ESP_LOGI(TAG, "AirPlay prev track via DACP");
    break;
#ifdef CONFIG_BT_A2DP_ENABLE
  case PLAYBACK_SOURCE_BLUETOOTH:
    bt_a2dp_send_prev();
    break;
#endif
  default:
    break;
  }
}
