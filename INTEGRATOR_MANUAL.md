# QMA6100P Accelerometer Driver — Integrator Manual

**Target hardware:** Seeed Tracker T1000-E (nRF52840 + QMA6100P)  
**RTOS:** Zephyr RTOS 4.x  
**Driver files:** `ZephyrAccelerometer.h` / `ZephyrAccelerometer.cpp`

---

## Table of Contents

1. [Overview](#1-overview)
2. [Hardware Description](#2-hardware-description)
3. [Prerequisites](#3-prerequisites)
4. [File Placement](#4-file-placement)
5. [Device Tree Configuration](#5-device-tree-configuration)
6. [Kconfig Integration](#6-kconfig-integration)
7. [CMake Integration](#7-cmake-integration)
8. [Application Code Integration](#8-application-code-integration)
9. [API Reference](#9-api-reference)
10. [Register Map](#10-register-map)
11. [Initialization Sequence Explained](#11-initialization-sequence-explained)
12. [Critical Implementation Notes](#12-critical-implementation-notes)
13. [Troubleshooting](#13-troubleshooting)

---

## 1. Overview

This driver provides bare-metal access to the **QMA6100P** 3-axis MEMS accelerometer
over I2C. It is written in C++ with a pure-C extern "C" API so it is callable from
both C and C++ application code.

**What the driver provides:**
- Hardware initialization with the full Seeed-validated 3-phase NVM load sequence
- 24-bit autonomous hardware step counter (read on demand, no interrupts required)
- Instantaneous 14-bit signed XYZ acceleration read (2G range, 244 µg/LSB)
- Debug snapshot string for CLI output

**What the driver does NOT provide:**
- Interrupts / GPIO INT1 pin handling (poll-on-demand only)
- Motion detection state machine
- GPS duty-cycle integration
- Zephyr sensor subsystem (`struct sensor_driver_api`) — this is a direct I2C driver

---

## 2. Hardware Description

### Sensor

| Parameter         | Value                         |
|-------------------|-------------------------------|
| Part              | QMA6100P (CEVA/mCube)         |
| I2C address       | `0x12` (SA0 tied LOW on T1000-E) |
| I2C bus           | `i2c0`, 400 kHz (FAST mode)   |
| SDA pin           | P0.26                         |
| SCL pin           | P0.27                         |
| Power enable      | P1.07 — active HIGH, must be driven HIGH before any I2C access |
| INT1 pin          | P1.02 — wired but not used by this driver |
| Chip ID register  | `0x00` → expected value `0x90` |

### Power rail behaviour

The QMA6100P VDD is gated by a GPIO (P1.07) on the T1000-E. This is **not** an
optional feature — the chip will not respond on I2C if P1.07 is LOW, and **writing
active mode (0x80) while the chip is already active triggers an NVM re-load that
permanently silences I2C until VDD is power-cycled**. The driver power-cycles P1.07
unconditionally at init to guarantee a clean starting state regardless of previous
firmware.

### Measurement range

| Range | LSB resolution | Full scale |
|-------|----------------|------------|
| ±2G   | 244 µg/LSB     | ±2G        |

The driver is hardcoded to 2G for maximum sensitivity. To change range, modify
`QMA_RANGE_2G` and `QMA_REG_RANGE` in `ZephyrAccelerometer.cpp`.

### ODR

Hardcoded to **25 Hz** (`QMA_BW_25HZ = 0x06`). The chip outputs data at 25 Hz;
each call to `accel_read_xyz()` reads the most recent sample instantly.

---

## 3. Prerequisites

### Zephyr version

Zephyr **4.0 or later** is required. The driver uses:
- `i2c_write_read()` — available since Zephyr 2.x
- `GPIO_DT_SPEC_GET` macro — available since Zephyr 3.x
- `DT_NODE_EXISTS` / `DT_ALIAS` — standard DTS macros

### Zephyr Kconfig dependencies

The following Kconfig symbols must be enabled (usually automatic via board config):

```
CONFIG_I2C=y
CONFIG_GPIO=y
CONFIG_LOG=y          # optional but strongly recommended
```

### Toolchain

Any Zephyr-supported ARM toolchain (Zephyr SDK 0.16+ or GNU Arm Embedded 12+).

---

## 4. File Placement

Place both driver files anywhere under your application source tree that is
included in the CMake build. The recommended location mirrors the ZephCore project:

```
<app_root>/
  adapters/
    sensors/
      ZephyrAccelerometer.h     ← driver header
      ZephyrAccelerometer.cpp   ← driver implementation
```

The header is included by application code as:

```c
#include <ZephyrAccelerometer.h>
```

This works as long as the `adapters/sensors/` directory is listed in
`target_include_directories()` — see Section 7.

---

## 5. Device Tree Configuration

Three DTS changes are required. All are specific to the T1000-E board files.

### 5.1 I2C pinctrl — add bias-pull-up (CRITICAL)

**File:** `boards/nrf52840/t1000_e/t1000_e_nrf52840-pinctrl.dtsi`

The nRF52840 TWIM controller requires internal pull-ups on SDA and SCL when the
board does not have external pull-up resistors. Without `bias-pull-up` every I2C
transaction will return `-ETIMEDOUT (-116)`.

```dts
/* BEFORE — missing pull-ups, will fail at runtime */
i2c0_default: i2c0_default {
    group1 {
        psels = <NRF_PSEL(TWIM_SDA, 0, 26)>,
                <NRF_PSEL(TWIM_SCL, 0, 27)>;
    };
};

/* AFTER — correct */
i2c0_default: i2c0_default {
    group1 {
        psels = <NRF_PSEL(TWIM_SDA, 0, 26)>,
                <NRF_PSEL(TWIM_SCL, 0, 27)>;
        bias-pull-up;
    };
};
```

### 5.2 Accelerometer power node

**File:** `boards/nrf52840/t1000_e/t1000_e_nrf52840.dts`

Add a `regulator-fixed` node for the P1.07 power enable GPIO. Place it alongside
the other fixed regulator nodes (e.g. `sensor-power`, `buzzer-enable`):

```dts
accel_power: accel-power {
    compatible = "regulator-fixed";
    regulator-name = "accel-power";
    enable-gpios = <&gpio1 7 GPIO_ACTIVE_HIGH>;
};
```

### 5.3 Alias

**File:** `boards/nrf52840/t1000_e/t1000_e_nrf52840.dts`

Add the `accel-power` alias inside the existing `aliases { }` block:

```dts
aliases {
    led0 = &led_green;
    sw0  = &user_button;
    /* ... existing aliases ... */
    accel-power = &accel_power;   /* ← add this */
};
```

The driver resolves the GPIO at compile time via `DT_ALIAS(accel_power)`. If the
alias is absent the driver will skip the power-cycle step and assume the chip is
already powered — which will work only if VDD is tied permanently HIGH.

### 5.4 Complete minimal DTS snippet

Below is a self-contained overlay that can be used for bring-up on any nRF52840
board where P1.07 controls QMA6100P VDD and I2C0 (SDA=P0.26, SCL=P0.27) is used:

```dts
/* accel_bringup.overlay */
&pinctrl {
    i2c0_default: i2c0_default {
        group1 {
            psels = <NRF_PSEL(TWIM_SDA, 0, 26)>,
                    <NRF_PSEL(TWIM_SCL, 0, 27)>;
            bias-pull-up;
        };
    };
    i2c0_sleep: i2c0_sleep {
        group1 {
            psels = <NRF_PSEL(TWIM_SDA, 0, 26)>,
                    <NRF_PSEL(TWIM_SCL, 0, 27)>;
            low-power-enable;
        };
    };
};

&i2c0 {
    status = "okay";
    clock-frequency = <I2C_BITRATE_FAST>;
    pinctrl-0 = <&i2c0_default>;
    pinctrl-1 = <&i2c0_sleep>;
    pinctrl-names = "default", "sleep";
};

/ {
    accel_power: accel-power {
        compatible = "regulator-fixed";
        regulator-name = "accel-power";
        enable-gpios = <&gpio1 7 GPIO_ACTIVE_HIGH>;
    };

    aliases {
        accel-power = &accel_power;
    };
};
```

---

## 6. Kconfig Integration

### 6.1 Add to your project Kconfig

Create or extend your project `Kconfig` with:

```kconfig
menu "Accelerometer"

config ZEPHCORE_ACCEL
    bool "Enable QMA6100P accelerometer"
    default n
    depends on I2C
    help
      Enable QMA6100P accelerometer driver (step counting, XYZ reading).
      Requires I2C0 and the accel-power GPIO alias (P1.07 on T1000-E).

config ZEPHCORE_ACCEL_LOG_LEVEL
    int "Accelerometer log level (0=off 1=err 2=warn 3=inf 4=dbg)"
    default 3
    range 0 4
    depends on ZEPHCORE_ACCEL

endmenu
```

### 6.2 Enable in board config

In your board-specific `board.conf` (or `prj.conf`):

```
CONFIG_ZEPHCORE_ACCEL=y
```

### 6.3 What CONFIG_ZEPHCORE_ACCEL_LOG_LEVEL controls

The driver calls `LOG_MODULE_REGISTER(zephcore_accel, CONFIG_ZEPHCORE_ACCEL_LOG_LEVEL)`.
Zephyr's logging subsystem uses this as the compile-time maximum level. Values:

| Value | Meaning        | Output                              |
|-------|----------------|-------------------------------------|
| 0     | Off            | No log output at all                |
| 1     | Error only     | Only `LOG_ERR()`                    |
| 2     | Warning+       | `LOG_ERR()` + `LOG_WRN()`           |
| 3     | Info (default) | + `LOG_INF()` — normal operation    |
| 4     | Debug          | + `LOG_DBG()` — per-read verbose    |

Note: `printk()` calls in the driver bypass the log subsystem and always print
regardless of `CONFIG_LOG`. They are intentional — init output must be visible
even in production builds where `CONFIG_LOG=n`.

---

## 7. CMake Integration

In your project `CMakeLists.txt`, add the source file to your target and the
include path for the header:

```cmake
# Add include directory so #include <ZephyrAccelerometer.h> resolves
target_include_directories(app PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/adapters/sensors
)

# Compile the driver only when CONFIG_ZEPHCORE_ACCEL is enabled
target_sources(app PRIVATE
    $<$<BOOL:${CONFIG_ZEPHCORE_ACCEL}>:adapters/sensors/ZephyrAccelerometer.cpp>
)
```

The generator expression `$<$<BOOL:...>:...>` means the file is only compiled
when the Kconfig symbol is `y`. This avoids build errors on boards without I2C
or the `accel-power` alias.

---

## 8. Application Code Integration

### 8.1 Minimal integration

```c
#include <ZephyrAccelerometer.h>

int main(void)
{
    /* ... other init ... */

    if (accel_init() == 0) {
        printk("Accelerometer ready\n");
    } else {
        printk("Accelerometer init failed\n");
    }

    /* ... rest of main ... */
}
```

### 8.2 Reading step count

```c
uint32_t steps = accel_read_steps();
printk("Steps: %u\n", steps);
```

The step counter is a 24-bit hardware accumulator inside the QMA6100P. It
increments autonomously and **never resets unless the chip loses power** (P1.07
goes LOW). Take a baseline snapshot at boot and subtract to get relative steps:

```c
static uint32_t steps_at_boot;

void on_boot(void)
{
    accel_init();
    steps_at_boot = accel_read_steps();
}

uint32_t get_steps_since_boot(void)
{
    return accel_read_steps() - steps_at_boot;
}
```

### 8.3 Reading XYZ acceleration

```c
int16_t x, y, z;
if (accel_read_xyz(&x, &y, &z)) {
    /* Each LSB = 244 µg at 2G range */
    /* +4096 LSB ≈ +1G, -4096 LSB ≈ -1G */
    printk("XYZ: %d  %d  %d\n", (int)x, (int)y, (int)z);
}
```

Any pointer argument may be NULL if that axis is not needed:

```c
int16_t z_only;
accel_read_xyz(NULL, NULL, &z_only);
```

### 8.4 Debug CLI command

```c
char dbg[128];
accel_get_debug_str(dbg, sizeof(dbg));
printk("accel: %s\n", dbg);
/* Output: "steps=1234 xyz=(12,-8,4087) | pwr=80(0) odr=06(0) ien=20(0)" */
```

### 8.5 Guard with IS_ENABLED (recommended)

When using the Kconfig guard pattern used in ZephCore's `main_companion.cpp`:

```c
#if IS_ENABLED(CONFIG_ZEPHCORE_ACCEL)
#include <ZephyrAccelerometer.h>
#endif

/* In main(): */
#if IS_ENABLED(CONFIG_ZEPHCORE_ACCEL)
    if (accel_init() == 0) {
        LOG_INF("Accelerometer initialized");
    } else {
        LOG_WRN("Accelerometer init failed — step counting disabled");
    }
#endif
```

---

## 9. API Reference

### `bool accel_is_available(void)`

Returns `true` after a successful `accel_init()`. Safe to call before init — 
returns `false`. Use this to guard read calls when init might have failed.

---

### `int accel_init(void)`

Initializes the QMA6100P.

**Actions performed:**
1. Obtains the `i2c0` device handle and verifies it is ready.
2. Power-cycles P1.07 (100 ms LOW → 200 ms HIGH) if `accel-power` alias exists.
3. Reads chip ID from register `0x00`, verifies it equals `0x90`.
4. Runs the 3-phase NVM + configuration sequence (see Section 11).
5. Reads back key registers and stores them for `accel_get_debug_str()`.

**Returns:**
- `0` — success, chip is active and responding
- `-ENODEV` — I2C device not ready, or chip ID mismatch
- Any negative `errno` — I2C transaction failure during init

**Thread safety:** Not thread-safe. Call once from a single thread at boot before
any other driver function.

**Timing:** Blocks the calling thread for approximately **700 ms** total (dominated
by the 200 ms NVM load silence window and the 300 ms power-cycle settle time).

---

### `uint32_t accel_read_steps(void)`

Reads the 24-bit hardware step counter.

**Registers read:** `0x07` (STEP_L) + `0x08` (STEP_M) in a single burst transaction,
then `0x0D` (STEP_H) separately. The burst read of L+M is atomic at the I2C level,
preventing a rollover race between the two low bytes.

**Returns:** Step count 0–16,777,215. Returns `0` if the chip is not available.

**Thread safety:** Safe to call from any thread or work queue after `accel_init()`
completes, as long as only one caller is active at a time.

---

### `bool accel_read_xyz(int16_t *x, int16_t *y, int16_t *z)`

Reads one instantaneous XYZ acceleration sample.

**Registers read:** `0x01`–`0x06` (DX_L through DZ_H) in a single 6-byte burst
transaction using `i2c_write_read()` (repeated START). This is atomic — all three
axes are sampled at the same ODR tick.

**Data format:** Each axis is a 14-bit signed two's complement value stored in the
upper 14 bits of a 16-bit little-endian word (bits [15:2]). The driver right-shifts
by 2 to produce a signed `int16_t` in the range −8192 to +8191.

**Scale:** 1 LSB = 244 µg at 2G range.  
At rest on a flat surface expect Z ≈ +4096 (1G), X ≈ 0, Y ≈ 0.

**Parameters:** Any pointer may be NULL.

**Returns:** `true` on success, `false` if chip unavailable or I2C read failed.

---

### `void accel_get_debug_str(char *buf, size_t len)`

Fills `buf` with a human-readable snapshot of the accelerometer state. Calls
`accel_read_steps()` and `accel_read_xyz()` internally — do not call from an
interrupt context.

**Example output (chip working):**
```
steps=1234 xyz=(12,-8,4087) | pwr=80(0) odr=06(0) ien=20(0)
```

**Example output (init failed):**
```
init failed rc=-19
```

The `pwr/odr/ien` fields show the register value and the return code of the readback
I2C transaction (0 = success). Use these to diagnose configuration problems.

---

## 10. Register Map

| Address | Name         | Reset | Written value | Description                                      |
|---------|--------------|-------|---------------|--------------------------------------------------|
| `0x00`  | CHIP_ID      | 0x90  | —             | Read-only chip ID; driver verifies = `0x90`      |
| `0x01`–`0x06` | DX_L–DZ_H | — | —           | 14-bit XYZ data, LE, bits[15:2]                  |
| `0x07`  | STEP_L       | —     | —             | Step counter bits [7:0]                          |
| `0x08`  | STEP_M       | —     | —             | Step counter bits [15:8]                         |
| `0x0D`  | STEP_H       | —     | —             | Step counter bits [23:16]                        |
| `0x0F`  | RANGE        | ?     | `0x01`        | Measurement range: 0x01 = ±2G                    |
| `0x10`  | BW_ODR       | ?     | `0x06`        | Output data rate: 0x06 = 25 Hz                   |
| `0x11`  | PWR_MGMT     | 0x00  | `0x00`/`0x80` | 0x00 = standby, 0x80 = active mode               |
| `0x12`  | STEP_CONF0   | 0x00  | `0x80`        | BIT7 = STEP_EN; must be set to enable step counter |
| `0x16`  | INT_EN0      | 0x00  | `0x20`        | BIT5 = any-motion global enable                  |
| `0x2C`  | AMD_DUR      | ?     | `0x8B`        | Any-motion duration (Seeed-validated value)       |
| `0x2E`  | AMD_TH       | ?     | `26`          | Any-motion threshold ≈ 100 mg at 2G              |
| `0x33`  | NVM_LOAD     | —     | `BIT(3)`      | Writing BIT3 triggers OTP/NVM load into shadow registers |
| `0x4A`  | *(undoc)*    | —     | `0x08`        | Required for step counter and interrupt operation |
| `0x56`  | *(undoc)*    | —     | `0x01`        | Required for step counter and interrupt operation |
| `0x5F`  | *(undoc)*    | —     | `0x80`        | Required for step counter and interrupt operation |

### Undocumented registers 0x4A / 0x56 / 0x5F

These three registers do not appear in the public QMA6100P datasheet. They were
reverse-engineered from the Seeed T1000-E Arduino reference firmware
(`qma6100p.c`). **Omitting any of these writes causes the step counter to stay at
zero and any-motion interrupt to never fire**, even though all other registers read
back correctly. They must be written in standby mode (PWR_MGMT = 0x00) as part of
Phase 2 of the init sequence.

---

## 11. Initialization Sequence Explained

The QMA6100P requires a specific 3-phase sequence on every power-up. Deviating from
this sequence (e.g., Arduino-style `write(RANGE); write(PWR_MGMT, 0x80)`) results
in either a non-functional step counter or a chip that freezes I2C.

### Phase 0 — Power cycle (if accel-power alias present)

```
P1.07 → LOW   (100 ms)   chip completely off, internal capacitors discharge
P1.07 → HIGH  (200 ms)   chip POR, I2C slave FSM resets to known state
```

This step is unconditional. It prevents the most common failure mode: a previous
firmware run left the chip in active mode; writing `PWR_MGMT = 0x80` again triggers
a second NVM load which locks I2C until VDD is cycled.

### Phase 1 — NVM load trigger

```
Write PWR_MGMT = 0x80   (active mode)
Wait 20 ms
Write NVM_LOAD = 0x08   (BIT3 = NVM load)
Wait 200 ms             ← I2C is silent during this window; do NOT send any transactions
Write PWR_MGMT = 0x00   (back to standby)
Wait 20 ms
```

The 200 ms silent window is mandatory. The chip reads its factory calibration and
configuration from OTP into shadow registers during this time. Any I2C traffic
during this window will be NAK'd or corrupt the load.

### Phase 2 — Register configuration (in standby)

All registers are written while `PWR_MGMT = 0x00` (standby). A 5 ms delay between
each write is recommended (matches Seeed reference firmware) to ensure register
writes settle before the next:

```
Write 0x4A = 0x08   (undocumented — step counter enable)
Write 0x56 = 0x01   (undocumented — interrupt path)
Write 0x5F = 0x80   (undocumented — step counter path)
Write BW_ODR  = 0x06  (25 Hz)
Write RANGE   = 0x01  (±2G)
Write STEP_CONF0 = 0x80  (STEP_EN = BIT7)
Write AMD_TH  = 26    (~100 mg threshold)
Write AMD_DUR = 0x8B  (duration from Seeed)
Write INT_EN0 = 0x20  (any-motion enable)
```

### Phase 3 — Final active mode

```
Write PWR_MGMT = 0x80   (active mode)
Wait 100 ms             chip stabilizes, ODR starts
```

This second active-mode write is safe because NVM has already been loaded in Phase 1.
The chip enters measurement mode without re-triggering the NVM load.

---

## 12. Critical Implementation Notes

### 12.1 Use i2c_write_read() for all reads — never i2c_write() + i2c_read()

The QMA6100P I2C slave FSM **requires a repeated START** between the address-phase
write and the data-phase read. Using a separate `i2c_write()` followed by
`i2c_read()` (which produces a STOP + START) causes the slave FSM to reset between
the two transactions, and the read returns garbage or times out with `-ETIMEDOUT`.

```c
/* CORRECT — repeated START */
i2c_write_read(i2c_dev, QMA_ADDR, &reg, 1, val, 1);

/* WRONG — produces STOP between write and read */
i2c_write(i2c_dev, &reg, 1, QMA_ADDR);
i2c_read(i2c_dev, val, 1, QMA_ADDR);
```

This is the single most common integration failure with this chip.

### 12.2 bias-pull-up is mandatory on nRF52840 TWIM

The T1000-E PCB does not have external I2C pull-up resistors on SDA/SCL. The
nRF52840's TWIM controller requires them. Without `bias-pull-up` in the pinctrl
configuration, all I2C reads return `-ETIMEDOUT (-116)`. This looks identical to
an unpowered chip and is easy to misdiagnose.

### 12.3 STEP_EN default is 0

The QMA6100P datasheet states `STEP_CONF0` (register `0x12`) defaults to `0x00`,
meaning the step counter is **disabled at power-up**. It must be explicitly enabled
by writing `BIT(7)` to `STEP_CONF0`. The driver does this in Phase 2. If this write
is missed the step counter stays permanently at zero.

### 12.4 Power cycle is not optional

If your application calls `accel_init()` more than once (e.g., after a soft reset
that does not cut VDD), omitting the P1.07 power cycle risks writing `PWR_MGMT =
0x80` to a chip already in active mode, which silences I2C until the next hard
power cycle. If you cannot power-cycle, add a soft-reset via `QMA_REG_RESET`
(`0x36`) before the NVM load step.

### 12.5 No concurrent access

The driver has no internal mutex. If your application reads the accelerometer from
multiple threads or work queues, protect all API calls with a Zephyr mutex or
semaphore.

### 12.6 Step counter is cumulative, not resettable

The 24-bit step counter in hardware cannot be reset by software. It wraps at
16,777,215 and continues from 0. Applications must record a baseline at boot and
compute deltas. The counter **is reset** to 0 when P1.07 goes LOW (chip loses VDD),
which happens during `accel_init()`.

---

## 13. Troubleshooting

### Symptom: chip ID read fails with -ETIMEDOUT (-116)

**Cause A:** `bias-pull-up` missing from `i2c0_default` pinctrl.  
**Fix:** Add `bias-pull-up;` to the TWIM_SDA/TWIM_SCL pinctrl group (Section 5.1).

**Cause B:** P1.07 not driven HIGH before the I2C read.  
**Fix:** Ensure the `accel-power` alias resolves correctly. Check with:
```
printk("accel_pwr pin: %d\n", gpio_pin_get_dt(&accel_pwr));
```

**Cause C:** I2C0 not initialized (no `status = "okay"` in DTS).  
**Fix:** Add `&i2c0 { status = "okay"; ... };` to your overlay.

---

### Symptom: chip ID read returns wrong value (not 0x90)

**Cause:** The bus has electrical issues (floating lines, multiple devices not
responding). Scope SDA/SCL to verify pull-ups are working.

**Check:** With `CONFIG_I2C_SHELL=y` you can scan the bus:
```
uart:~$ i2c scan i2c@40003000
     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f
00:              -- -- -- -- -- -- -- -- -- -- -- --
10: -- -- 12 -- -- -- -- -- -- -- -- -- -- -- -- --
```
The QMA6100P should appear at address `0x12`.

---

### Symptom: chip ID reads 0x90 but step counter is always 0

**Cause A:** `STEP_CONF0` write to `BIT(7)` is missing or was skipped.  
**Check:** `accel_get_debug_str()` will show `step=XX` in the init readback. If
`step=00`, the write failed or was skipped.

**Cause B:** Undocumented registers 0x4A/0x56/0x5F not written.  
**Fix:** These three writes must be present in Phase 2 — they are non-negotiable
despite not appearing in the datasheet.

---

### Symptom: NVM load phase hangs or subsequent writes fail with -EIO

**Cause:** An I2C transaction was sent during the 200 ms NVM silence window in
Phase 1. The chip NAK'd it, but if your I2C driver does not handle NAK gracefully
it may put the bus into an error state.  
**Fix:** Ensure no other driver is accessing I2C0 during `accel_init()`. The
`zephyr,deferred-init` DTS property can be used on other I2C devices to delay
their initialization past `accel_init()`.

---

### Symptom: XYZ reads all return (0, 0, 0) after init

**Cause:** Chip is still in standby (Phase 3 `PWR_MGMT = 0x80` write failed or
was skipped). The chip outputs zero data in standby.  
**Check:** `accel_get_debug_str()` shows `pwr=00` when in standby; it should show
`pwr=80`.

---

### Symptom: init succeeds but all data is garbage after a firmware re-flash

**Cause:** UF2/hex flashing did not reset P1.07, so the chip was left in active
mode from the previous firmware run. `accel_init()` wrote `PWR_MGMT = 0x80` to an
already-active chip, triggering a second NVM load and locking I2C.  
**Fix:** This is the exact scenario the P1.07 power cycle in `accel_init()` is
designed to prevent. Confirm the `accel-power` alias is present and the GPIO
direction is correct.

---

*End of Integrator Manual*
