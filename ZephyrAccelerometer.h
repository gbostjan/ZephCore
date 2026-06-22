/*
 * SPDX-License-Identifier: MIT
 * QMA6100P Accelerometer Driver for T1000-E
 *
 * Hardware (T1000-E):
 *   I2C address:  0x12  (SA0 tied LOW)
 *   Power rail:   P1.07 (alias accel-power) — must be HIGH before I2C access
 *   INT1 pin:     P1.02 — reserved, not used in poll mode
 */

#pragma once

#include <zephyr/kernel.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Returns true if QMA6100P was found and initialized successfully */
bool accel_is_available(void);

/* Initialize QMA6100P: power on P1.07, verify chip ID (0x90),
 * configure 2G range / 25 Hz ODR / step counter / any-motion detection.
 * Returns 0 on success, negative errno on failure. */
int accel_init(void);

/* Read 24-bit hardware step counter (registers 0x07/0x08/0x0d).
 * Accumulates autonomously — never resets unless chip loses power. */
uint32_t accel_read_steps(void);

/* Read instantaneous 14-bit signed XYZ acceleration (2G range, 244 µg/LSB).
 * Any pointer may be NULL. Returns false if chip not available or read failed. */
bool accel_read_xyz(int16_t *x, int16_t *y, int16_t *z);

/* Fill buf with a human-readable debug snapshot: steps, XYZ, init register readback. */
void accel_get_debug_str(char *buf, size_t len);

#ifdef __cplusplus
}
#endif
