#pragma once

#include <stdint.h>

#include "esp_err.h"

/* Bind to the setup AP's IPv4 address in network byte order. Runs until reboot. */
esp_err_t portal_dns_start(uint32_t ipv4_be);
