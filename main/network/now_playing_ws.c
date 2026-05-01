/**
 * @file now_playing_ws.c
 * @brief Push-driven now-playing over /ws/now_playing.
 *
 * Subscribes to the RTSP event bus once at init. On each event,
 * snapshots the now_playing state into JSON and broadcasts to every
 * connected WebSocket client. Connect-time send is the latest snapshot
 * so the dashboard renders correctly without an extra HTTP fetch.
 */

#include "now_playing_ws.h"

#include "cJSON.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "ha_airplay_artwork.h"
#include "now_playing.h"
#include "rtsp_events.h"

#include <stdio.h>
#include <string.h>

#define TAG "np_ws"
#define MAX_NP_WS_CLIENTS 3

static httpd_handle_t s_server = NULL;
static int s_clients[MAX_NP_WS_CLIENTS];
static int s_client_count = 0;
static SemaphoreHandle_t s_mutex = NULL;

static const char *state_str_for(now_playing_state_t s) {
  switch (s) {
  case NOW_PLAYING_CONNECTED: return "connected";
  case NOW_PLAYING_PLAYING:   return "playing";
  case NOW_PLAYING_PAUSED:    return "paused";
  default:                    return "idle";
  }
}

static char *snapshot_json(void) {
  now_playing_t np;
  memset(&np, 0, sizeof(np));
  now_playing_get(&np);

  cJSON *json = cJSON_CreateObject();
  cJSON_AddStringToObject(json, "state", state_str_for(np.state));
  cJSON_AddStringToObject(json, "title", np.title);
  cJSON_AddStringToObject(json, "artist", np.artist);
  cJSON_AddStringToObject(json, "album", np.album);
  cJSON_AddStringToObject(json, "genre", np.genre);
  cJSON_AddNumberToObject(json, "duration_secs", np.duration_secs);
  cJSON_AddNumberToObject(json, "position_secs", np.position_secs);
  cJSON_AddBoolToObject(json, "has_artwork", np.has_artwork);
  cJSON_AddNumberToObject(json, "artwork_etag",
                          (double)ha_airplay_artwork_etag());
  char *out = cJSON_PrintUnformatted(json);
  cJSON_Delete(json);
  return out;
}

static void remove_client_locked(int idx) {
  if (idx < s_client_count - 1) {
    s_clients[idx] = s_clients[s_client_count - 1];
  }
  s_client_count--;
}

static void broadcast_snapshot(void) {
  if (!s_server || s_client_count == 0) {
    return;
  }
  char *payload = snapshot_json();
  if (!payload) return;
  size_t len = strlen(payload);

  httpd_ws_frame_t frame = {
      .type = HTTPD_WS_TYPE_TEXT,
      .payload = (uint8_t *)payload,
      .len = len,
  };

  if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    for (int i = s_client_count - 1; i >= 0; i--) {
      esp_err_t err = httpd_ws_send_frame_async(s_server, s_clients[i], &frame);
      if (err != ESP_OK) {
        remove_client_locked(i);
      }
    }
    xSemaphoreGive(s_mutex);
  }
  free(payload);
}

static void on_rtsp_event(rtsp_event_t event,
                          const rtsp_event_data_t *data,
                          void *user_data) {
  (void)event;
  (void)data;
  (void)user_data;
  /* Any RTSP event — connect, playing, paused, disconnected, metadata —
     can move the now_playing snapshot. The cache in now_playing.c is
     the source of truth; we just broadcast on every change. */
  broadcast_snapshot();
}

static esp_err_t ws_handler(httpd_req_t *req) {
  if (req->method == HTTP_GET) {
    int fd = httpd_req_to_sockfd(req);
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      if (s_client_count < MAX_NP_WS_CLIENTS) {
        s_clients[s_client_count++] = fd;
        xSemaphoreGive(s_mutex);
        ESP_LOGI(TAG, "client connected (fd=%d, total=%d)", fd, s_client_count);
        /* Send the current snapshot immediately so the page renders
           without waiting for the next event. */
        char *payload = snapshot_json();
        if (payload) {
          httpd_ws_frame_t frame = {
              .type = HTTPD_WS_TYPE_TEXT,
              .payload = (uint8_t *)payload,
              .len = strlen(payload),
          };
          httpd_ws_send_frame_async(s_server, fd, &frame);
          free(payload);
        }
      } else {
        xSemaphoreGive(s_mutex);
        ESP_LOGW(TAG, "max clients reached, rejecting fd=%d", fd);
        return ESP_FAIL;
      }
    }
    return ESP_OK;
  }
  /* Ignore client → server frames; this is a server-push-only stream. */
  httpd_ws_frame_t frame = {.type = HTTPD_WS_TYPE_TEXT};
  return httpd_ws_recv_frame(req, &frame, 0);
}

esp_err_t now_playing_ws_init(void) {
  if (s_mutex) return ESP_OK;
  s_mutex = xSemaphoreCreateMutex();
  if (!s_mutex) return ESP_ERR_NO_MEM;
  s_client_count = 0;
  rtsp_events_register(on_rtsp_event, NULL);
  return ESP_OK;
}

esp_err_t now_playing_ws_register(httpd_handle_t server) {
  s_server = server;
  httpd_uri_t uri = {
      .uri = "/ws/now_playing",
      .method = HTTP_GET,
      .handler = ws_handler,
      .is_websocket = true,
  };
  esp_err_t err = httpd_register_uri_handler(server, &uri);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to register /ws/now_playing: %s",
             esp_err_to_name(err));
    return err;
  }
  ESP_LOGI(TAG, "Now-playing push on /ws/now_playing");
  return ESP_OK;
}
