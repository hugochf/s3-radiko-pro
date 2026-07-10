/*
 * Minimal HTTPS client over esp_http_client, with proper server-certificate
 * validation via the ESP x509 certificate bundle (NOT setInsecure()).
 *
 * Supports request headers, capturing named response headers, and buffering the
 * body — enough for the Radiko auth1/auth2 flow in Phase 10.
 */
#pragma once

#include <stddef.h>
#include "esp_err.h"
#include "esp_http_client.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *name;
    const char *value;
} http_header_t;

typedef struct {
    // --- request ---
    const char              *url;
    esp_http_client_method_t method;          // e.g. HTTP_METHOD_GET
    const http_header_t     *req_headers;     // may be NULL
    int                      n_req_headers;

    // --- response headers to capture (optional) ---
    const char *const       *want_headers;    // array of header names, may be NULL
    char                   (*resp_values)[128]; // parallel output buffers
    int                      n_want;

    // --- response body (optional) ---
    char                    *body;            // buffer, may be NULL
    size_t                   body_cap;

    // --- outputs ---
    int                      status;          // HTTP status code
    size_t                   body_len;
} http_req_t;

// Perform the request. Returns ESP_OK on a completed transaction (check .status).
esp_err_t httpc_do(http_req_t *req);

#ifdef __cplusplus
}
#endif
