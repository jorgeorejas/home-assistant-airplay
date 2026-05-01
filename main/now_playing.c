#include "now_playing.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "rtsp_events.h"

#include <string.h>

static now_playing_t s_state = {0};
static SemaphoreHandle_t s_mutex = NULL;

static void on_event(rtsp_event_t event, const rtsp_event_data_t *data,
                     void *user_data) {
  (void)user_data;

  if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
    return;
  }

  switch (event) {
  case RTSP_EVENT_CLIENT_CONNECTED:
    s_state.state = NOW_PLAYING_CONNECTED;
    break;
  case RTSP_EVENT_PLAYING:
    s_state.state = NOW_PLAYING_PLAYING;
    break;
  case RTSP_EVENT_PAUSED:
    s_state.state = NOW_PLAYING_PAUSED;
    break;
  case RTSP_EVENT_DISCONNECTED:
    /* Wipe metadata on disconnect — leaving stale title around is worse
       than showing nothing. */
    memset(&s_state, 0, sizeof(s_state));
    s_state.state = NOW_PLAYING_IDLE;
    break;
  case RTSP_EVENT_METADATA:
    if (data) {
      const rtsp_metadata_t *m = &data->metadata;
      /* Copy only non-empty fields so progress-only updates don't blank
         the title/artist that came in earlier. */
      if (m->title[0]) {
        strncpy(s_state.title, m->title, sizeof(s_state.title) - 1);
        s_state.title[sizeof(s_state.title) - 1] = '\0';
      }
      if (m->artist[0]) {
        strncpy(s_state.artist, m->artist, sizeof(s_state.artist) - 1);
        s_state.artist[sizeof(s_state.artist) - 1] = '\0';
      }
      if (m->album[0]) {
        strncpy(s_state.album, m->album, sizeof(s_state.album) - 1);
        s_state.album[sizeof(s_state.album) - 1] = '\0';
      }
      if (m->genre[0]) {
        strncpy(s_state.genre, m->genre, sizeof(s_state.genre) - 1);
        s_state.genre[sizeof(s_state.genre) - 1] = '\0';
      }
      if (m->duration_secs) {
        s_state.duration_secs = m->duration_secs;
      }
      s_state.position_secs = m->position_secs;
      s_state.has_artwork = m->has_artwork || s_state.has_artwork;
    }
    break;
  }

  xSemaphoreGive(s_mutex);
}

esp_err_t now_playing_init(void) {
  if (s_mutex) {
    return ESP_OK;
  }
  s_mutex = xSemaphoreCreateMutex();
  if (!s_mutex) {
    return ESP_ERR_NO_MEM;
  }
  rtsp_events_register(on_event, NULL);
  return ESP_OK;
}

void now_playing_get(now_playing_t *out) {
  if (!out) {
    return;
  }
  if (!s_mutex) {
    memset(out, 0, sizeof(*out));
    return;
  }
  if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
    memcpy(out, &s_state, sizeof(*out));
    xSemaphoreGive(s_mutex);
  } else {
    memset(out, 0, sizeof(*out));
  }
}
