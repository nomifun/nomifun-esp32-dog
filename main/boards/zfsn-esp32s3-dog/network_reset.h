#ifndef DOG_NETWORK_RESET_H
#define DOG_NETWORK_RESET_H

#include <esp_err.h>

// Clear connection credentials and request the configuration AP on the next
// boot. The caller must restart immediately after ESP_OK: SsidManager may still
// hold the previous credentials in RAM. No device calibration or identity is
// erased. A failure may leave earlier namespaces cleared; retry is safe.
esp_err_t ResetDogNetworkSettings();

#endif
