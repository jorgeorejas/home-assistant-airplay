#pragma once

/**
 * Push-driven now-playing over WebSocket. Mirrors `log_stream.c`'s
 * pattern: register a `/ws/now_playing` handler with the existing
 * httpd, broadcast a JSON snapshot to every connected client whenever
 * the RTSP event bus fires.
 *
 * The dashboard uses this in place of polling `/api/now_playing` every
 * 2 s — the visible "play tap → UI update" latency drops from ~1 s
 * median to <50 ms.
 *
 * Up to MAX_NP_WS_CLIENTS concurrent clients (3, same budget as the
 * log stream). Failed sends drop the client transparently.
 */

#include "esp_err.h"
#include "esp_http_server.h"

esp_err_t now_playing_ws_init(void);
esp_err_t now_playing_ws_register(httpd_handle_t server);
