#pragma once

#include "esp_err.h"
#include "esp_netif.h"

// Start the local CDC-NCM interface after esp_netif_init(), in setup mode only.
// On success the interface remains available until reboot. On failure *netif is NULL.
esp_err_t usb_setup_start(esp_netif_t **netif);
