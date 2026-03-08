/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start the ONVIF Profile T service
 *
 * Starts two background FreeRTOS tasks:
 *
 *  1. WS-Discovery responder (UDP multicast 239.255.255.250:3702)
 *     - Sends Hello on startup so the device appears in ONVIF scanners
 *     - Responds to Probe messages with ProbeMatch
 *
 *  2. ONVIF HTTP/SOAP server (TCP port CONFIG_ONVIF_HTTP_PORT, default 80)
 *     - /onvif/device_service: GetCapabilities, GetDeviceInformation,
 *       GetServices, GetSystemDateAndTime, GetServiceCapabilities
 *     - /onvif/media_service:  GetProfiles, GetStreamUri, GetVideoSources,
 *       GetVideoSourceConfigurations, GetVideoEncoderConfigurations,
 *       GetVideoEncoderConfigurationOptions, GetServiceCapabilities
 *
 * The H.264 stream URI returned by GetStreamUri points to the RTSP server
 * already running on port CONFIG_ETH_RTSP_PORT (default 554).
 *
 * Both tasks wait up to 30 s for a valid Ethernet IP before starting.
 *
 * @return ESP_OK on success, ESP_ERR_NO_MEM if task creation fails
 */
esp_err_t onvif_server_start(void);

#ifdef __cplusplus
}
#endif
