#pragma once

#include "esp_http_server.h"

/* Register admin HTTP handlers on the given httpd instance. */
void admin_register(httpd_handle_t server);
