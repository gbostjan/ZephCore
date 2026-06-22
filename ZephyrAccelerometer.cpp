/*
 * SPDX-License-Identifier: MIT
 * QMA6100P Accelerometer Driver — step counting, XYZ reading
 *
 * Register map sources:
 *   Seeed T1000-E qma6100p_defs.h / qma6100p.c  — ODR table, AMD sequence
 *   ESP-BSP qma6100p.c                           — NVM load (0x33 BIT3)
 *   RIOT-OS board.h                              — GPIO pins (P1.02, P1.07)
 *   Meshtastic QMA6100P_Arduino_Library          — chip ID, data registers
 */

#include "ZephyrAccelerometer.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(zephcore_accel, CONFIG_ZEPHCORE_ACCEL_LOG_LEVEL);

/* =========================================================================
 * QMA6100P register addresses
 * ========================================================================= */
#define QMA_ADDR            0x12
#define QMA_REG_DX_L        0x01
#define QMA_REG_CHIP_ID     0x00
#define QMA_REG_STEP_L      0x07
#define QMA_REG_STEP_M      0x08
#define QMA_REG_STEP_H      0x0D
#define QMA_REG_RANGE       0x0F
#define QMA_REG_BW_ODR      0x10
#define QMA_REG_PWR_MGMT    0x11
#define QMA_REG_STEP_CONF0  0x12   /* STEP_EN = BIT7 */
#define QMA_REG_INT_EN0     0x16
#define QMA_REG_AMD_DUR     0x2C
#define QMA_REG_AMD_TH      0x2E
#define QMA_REG_NVM_LOAD    0x33
/* Seeed undocumented registers — required for step-counter and interrupt enable */
#define QMA_REG_UNDOC_4A    0x4A
#define QMA_REG_UNDOC_56    0x56
#define QMA_REG_UNDOC_5F    0x5F
#define QMA_CHIP_ID         0x90

#define QMA_BW_25HZ              0x06
#define QMA_RANGE_2G             0x01
#define QMA_AMD_THRESHOLD_LSB    26    /* ~100 mg at 2G */
#define QMA_AMD_DURATION         0x8B  /* from Seeed qma6100p.c */
#define QMA_INT_EN0_ANYMOTION    0x20  /* bit 5 */

/* =========================================================================
 * DTS references
 * ========================================================================= */
#if DT_NODE_EXISTS(DT_ALIAS(accel_power))
static const struct gpio_dt_spec accel_pwr =
	GPIO_DT_SPEC_GET(DT_ALIAS(accel_power), enable_gpios);
#define HAS_ACCEL_PWR 1
#else
#define HAS_ACCEL_PWR 0
#endif

/* =========================================================================
 * State
 * ========================================================================= */
static const struct device *i2c_dev;
static bool accel_available;
static int  accel_init_rc = -EAGAIN;

/* Init register readback — persisted for accel_get_debug_str() */
static uint8_t init_pwr, init_odr, init_ien;
static int     init_pwr_rc, init_odr_rc, init_ien_rc;

/* Last XYZ sample — updated by accel_read_xyz() */
static int16_t last_ax, last_ay, last_az;

/* =========================================================================
 * I2C helpers
 * ========================================================================= */
static int qma_write(uint8_t reg, uint8_t val)
{
	uint8_t buf[2] = { reg, val };
	return i2c_write(i2c_dev, buf, 2, QMA_ADDR);
}

static int qma_read(uint8_t reg, uint8_t *val)
{
	return i2c_write_read(i2c_dev, QMA_ADDR, &reg, 1, val, 1);
}

/* =========================================================================
 * Hardware init
 * ========================================================================= */
static int qma6100p_hw_init(void)
{
	int rc;
	uint8_t chip_id = 0;

#if HAS_ACCEL_PWR
	/* Power-cycle P1.07: writing active mode while already active triggers
	 * NVM re-load which permanently silences I2C until VDD is cycled. */
	gpio_pin_configure_dt(&accel_pwr, GPIO_OUTPUT_INACTIVE);
	k_sleep(K_MSEC(100));
	gpio_pin_configure_dt(&accel_pwr, GPIO_OUTPUT_ACTIVE);
	k_sleep(K_MSEC(200));
	printk("[accel] P1.07 power cycled\n");
#else
	printk("[accel] no accel-power alias — chip assumed powered\n");
	k_sleep(K_MSEC(5));
#endif

	rc = qma_read(QMA_REG_CHIP_ID, &chip_id);
	if (rc) {
		printk("[accel] chip ID read failed: %d\n", rc);
		return rc;
	}
	if (chip_id != QMA_CHIP_ID) {
		printk("[accel] unexpected chip ID 0x%02x (expected 0x90)\n", chip_id);
		return -ENODEV;
	}
	printk("[accel] QMA6100P found chip_id=0x%02x\n", chip_id);

	/* Phase 1: active → NVM load trigger → standby
	 * Chip silences I2C during the 200 ms NVM load window. */
	rc = qma_write(QMA_REG_PWR_MGMT, 0x80);
	if (rc) { printk("[accel] nvm-active failed: %d\n", rc); return rc; }
	k_sleep(K_MSEC(20));
	rc = qma_write(QMA_REG_NVM_LOAD, BIT(3));
	if (rc) { printk("[accel] nvm_load write failed: %d\n", rc); return rc; }
	k_sleep(K_MSEC(200));
	rc = qma_write(QMA_REG_PWR_MGMT, 0x00);
	if (rc) { printk("[accel] standby failed: %d\n", rc); return rc; }
	k_sleep(K_MSEC(20));
	printk("[accel] NVM load done\n");

	/* Phase 2: configure in standby */
	rc = qma_write(QMA_REG_UNDOC_4A, 0x08);
	if (rc) { printk("[accel] undoc_4A failed: %d\n", rc); return rc; }
	k_sleep(K_MSEC(5));
	rc = qma_write(QMA_REG_UNDOC_56, 0x01);
	if (rc) { printk("[accel] undoc_56 failed: %d\n", rc); return rc; }
	k_sleep(K_MSEC(5));
	rc = qma_write(QMA_REG_UNDOC_5F, 0x80);
	if (rc) { printk("[accel] undoc_5F failed: %d\n", rc); return rc; }
	k_sleep(K_MSEC(5));
	rc = qma_write(QMA_REG_BW_ODR, QMA_BW_25HZ);
	if (rc) { printk("[accel] odr write failed: %d\n", rc); return rc; }
	k_sleep(K_MSEC(5));
	rc = qma_write(QMA_REG_RANGE, QMA_RANGE_2G);
	if (rc) { printk("[accel] range write failed: %d\n", rc); return rc; }
	k_sleep(K_MSEC(5));
	rc = qma_write(QMA_REG_STEP_CONF0, BIT(7));
	if (rc) { printk("[accel] step_en write failed: %d\n", rc); return rc; }
	k_sleep(K_MSEC(5));
	rc = qma_write(QMA_REG_AMD_TH, QMA_AMD_THRESHOLD_LSB);
	if (rc) { printk("[accel] amd_th write failed: %d\n", rc); return rc; }
	k_sleep(K_MSEC(5));
	rc = qma_write(QMA_REG_AMD_DUR, QMA_AMD_DURATION);
	if (rc) { printk("[accel] amd_dur write failed: %d\n", rc); return rc; }
	k_sleep(K_MSEC(5));
	rc = qma_write(QMA_REG_INT_EN0, QMA_INT_EN0_ANYMOTION);
	if (rc) { printk("[accel] int_en0 write failed: %d\n", rc); return rc; }
	k_sleep(K_MSEC(5));

	/* Phase 3: final active mode */
	rc = qma_write(QMA_REG_PWR_MGMT, 0x80);
	if (rc) { printk("[accel] pwr_mgmt write failed: %d\n", rc); return rc; }
	k_sleep(K_MSEC(100));
	printk("[accel] init sequence complete\n");

	/* Readback for diagnostics */
	{
		uint8_t r = 0, s = 0;
		init_pwr_rc = qma_read(QMA_REG_PWR_MGMT,  &init_pwr);
		              qma_read(QMA_REG_RANGE,      &r);
		init_odr_rc = qma_read(QMA_REG_BW_ODR,    &init_odr);
		              qma_read(QMA_REG_STEP_CONF0, &s);
		init_ien_rc = qma_read(QMA_REG_INT_EN0,   &init_ien);
		printk("[accel] init: pwr=%02x(%d) rng=%02x odr=%02x(%d) step=%02x ien=%02x(%d)\n",
			init_pwr, init_pwr_rc, r, init_odr, init_odr_rc,
			s, init_ien, init_ien_rc);
	}

	return 0;
}

/* =========================================================================
 * Public API
 * ========================================================================= */
bool accel_is_available(void)
{
	return accel_available;
}

int accel_init(void)
{
	printk("[accel] init start\n");
	i2c_dev = DEVICE_DT_GET(DT_NODELABEL(i2c0));
	if (!device_is_ready(i2c_dev)) {
		printk("[accel] I2C0 not ready\n");
		return -ENODEV;
	}
	printk("[accel] I2C0 ready\n");

	int rc = qma6100p_hw_init();
	accel_init_rc = rc;
	if (rc) {
		printk("[accel] init failed rc=%d\n", rc);
		return rc;
	}

	accel_available = true;
	LOG_INF("accel: QMA6100P ready, steps=%u", accel_read_steps());
	return 0;
}

uint32_t accel_read_steps(void)
{
	if (!accel_available || !i2c_dev) {
		return 0;
	}
	/* Burst-read STEP_L+STEP_M in one transaction to avoid rollover race */
	uint8_t reg = QMA_REG_STEP_L;
	uint8_t lm[2] = {0, 0};
	uint8_t sh = 0;
	i2c_write_read(i2c_dev, QMA_ADDR, &reg, 1, lm, 2);
	qma_read(QMA_REG_STEP_H, &sh);
	uint32_t steps = ((uint32_t)sh << 16) | ((uint32_t)lm[1] << 8) | lm[0];
	LOG_DBG("steps=%u (H=%02x M=%02x L=%02x)", steps, sh, lm[1], lm[0]);
	return steps;
}

bool accel_read_xyz(int16_t *x, int16_t *y, int16_t *z)
{
	if (!accel_available || !i2c_dev) {
		return false;
	}
	uint8_t raw[6];
	uint8_t start_reg = QMA_REG_DX_L;
	if (i2c_write_read(i2c_dev, QMA_ADDR, &start_reg, 1, raw, sizeof(raw)) != 0) {
		return false;
	}
	/* 14-bit signed: data in bits[15:2] of 16-bit little-endian word */
	last_ax = (int16_t)(((uint16_t)raw[1] << 8) | raw[0]) >> 2;
	last_ay = (int16_t)(((uint16_t)raw[3] << 8) | raw[2]) >> 2;
	last_az = (int16_t)(((uint16_t)raw[5] << 8) | raw[4]) >> 2;
	if (x) *x = last_ax;
	if (y) *y = last_ay;
	if (z) *z = last_az;
	LOG_DBG("xyz=(%d,%d,%d)", (int)last_ax, (int)last_ay, (int)last_az);
	return true;
}

void accel_get_debug_str(char *buf, size_t len)
{
	if (!accel_available) {
		snprintf(buf, len, "init failed rc=%d", accel_init_rc);
		return;
	}
	uint32_t steps = accel_read_steps();
	int16_t ax = 0, ay = 0, az = 0;
	accel_read_xyz(&ax, &ay, &az);
	snprintf(buf, len,
		 "steps=%u xyz=(%d,%d,%d) | pwr=%02x(%d) odr=%02x(%d) ien=%02x(%d)",
		 (unsigned)steps, (int)ax, (int)ay, (int)az,
		 init_pwr, init_pwr_rc, init_odr, init_odr_rc, init_ien, init_ien_rc);
}
