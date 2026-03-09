/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief Initialize PU control bridge (opens cached ISP fd)
     *
     * Must be called once at startup before any PU control callbacks fire.
     */
    esp_err_t uvc_ctrl_init(void);

    /**
     * @brief Deinitialize PU control bridge (closes cached ISP fd)
     */
    void uvc_ctrl_deinit(void);

    /**
     * @brief Set H.264 encoder parameters via V4L2 controls
     *
     * @param m2m_fd    H.264 M2M device fd
     * @param bitrate   Target bitrate in bps
     * @param i_period  I-frame period
     * @param min_qp    Minimum QP
     * @param max_qp    Maximum QP
     */
    esp_err_t uvc_ctrl_set_h264_params(int m2m_fd, int bitrate, int i_period,
                                       int min_qp, int max_qp);

    /**
     * @brief Set JPEG encoder quality
     *
     * @param m2m_fd    JPEG M2M device fd
     * @param quality   Quality 1-100
     */
    esp_err_t uvc_ctrl_set_jpeg_quality(int m2m_fd, int quality);

    /**
     * @brief Snapshot of active UVC/ISP control values
     */
    typedef struct
    {
        int16_t brightness;
        int16_t contrast;
        int16_t hue;
        int16_t saturation;
        int16_t sharpness;
        int16_t gain;
        uint8_t profile;
    } uvc_ctrl_state_t;

    /**
     * @brief Set a Processing Unit control using UVC control selector code
     *
     * Supported selectors: 0x02, 0x03, 0x04, 0x06, 0x07, 0x10
     */
    esp_err_t uvc_ctrl_set_pu(uint8_t cs, int16_t value);

    /**
     * @brief Set ISP color profile (0..5)
     */
    esp_err_t uvc_ctrl_set_profile(uint8_t profile_idx);

    /**
     * @brief Get current control state cached by firmware
     */
    void uvc_ctrl_get_state(uvc_ctrl_state_t *out_state);

    /**
     * @brief Reset controls to firmware defaults and apply immediately
     */
    esp_err_t uvc_ctrl_reset_defaults(void);

#ifdef __cplusplus
}
#endif
