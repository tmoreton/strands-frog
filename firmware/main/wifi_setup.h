#pragma once

#include "esp_err.h"

esp_err_t wifi_setup_start(esp_err_t (*save)(const char *ssid, const char *password));
