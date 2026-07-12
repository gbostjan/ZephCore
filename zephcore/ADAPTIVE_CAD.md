# Adaptive CAD — Site-Calibrated Listen-Before-Talk

ZephCore's listen-before-talk (LBT) runs a hardware Channel Activity
Detection (CAD) before every transmission: the radio's LoRa correlator
searches the channel for chirps, and TX is deferred while activity is
detected. How sensitive that search is comes down to one register value,
`cadDetPeak` — and the right value depends on where the node lives.

- **Too sensitive** (detPeak too low): chirp-like interference — other
  LoRa networks, other spreading factors on the same frequency — triggers
  *false positives*. The node keeps deferring TX for nothing, wasting
  100–200 ms retry cycles and adding latency; in bad cases it hits the
  4-second CAD watchdog.
- **Not sensitive enough** (detPeak too high): real transmissions are
  *missed* and the node transmits over ongoing packets, causing on-air
  collisions.

A quiet valley node and a 50-network hilltop node need different values.
Adaptive CAD measures the site and finds the value, instead of shipping
one hardcoded number to everyone.

Important physics note: `cadDetPeak` is a correlation peak-to-noise
threshold inside the radio's despreader — **not** a dBm level. The RSSI
noise floor cannot be converted into a detPeak value, which is why this
feature measures CAD behaviour directly instead of deriving it from RSSI.

## How it works

Every `cad.probe.interval` seconds (default 15), when the radio is idle
in receive mode, the firmware runs one **calibration CAD probe** and
immediately re-arms RX. A probe takes one CAD duration — about 4 ms at
SF7/250 kHz, up to ~130 ms at SF12/125 kHz — so the added radio deaf time
is roughly 0.1%. Probes also run in RX duty-cycle (sniff) mode: the
duty cycle is briefly interrupted and re-armed, within the same
preamble-catch budget philosophy the sniff-mode math already accepts.

Each probe tests one **level**: a signed offset from the chip family's
per-SF base detPeak (SX126x: `SF+13`; LR11xx/LR2021: the 56–68 table).
Results accumulate per level:

- `probes` — how many CADs ran at this level
- `busy` — raw "activity detected" verdicts
- `fp` — suspected **false positives**: busy verdicts where no packet
  materialised right after (see filtering below)
- `tp` — **true positives**: busy verdicts confirmed by actual RX
  activity immediately after the probe

Filtering, because a "busy" verdict during apparent silence may be a real
below-noise-floor packet (detecting those is CAD's whole purpose):

1. Probes are skipped entirely when the channel is visibly busy (RSSI
   more than 7 dB above the learned noise floor) or a packet is being
   received — those teach nothing about false positives.
2. After a busy verdict, RX is restarted and the firmware waits ~8 symbol
   times: a real transmitter is still on the air and trips the receive
   path. If nothing shows up, the verdict counts as a suspected false
   positive. Residual contamination by real traffic biases the estimate
   *conservative* (higher detPeak), which is the safe direction.

Statistics decay (halve) every 6 hours so the picture stays fresh, and
reset completely whenever the radio parameters change (frequency, SF, BW
— the data is only valid for one configuration).

### Auto mode (staircase) — ON by default

`cad.auto` ships **on**, on repeaters and companions alike. A one-sided
staircase controller acts on the probe stats:

- Probes concentrate on the **frontier** — one level more sensitive than
  the current operating point (3 of every 4 probes), with the remainder
  self-checking the operating level.
- **Step down** (more sensitive) when the frontier has ≥300 samples and
  its false-positive rate is ≤1%.
- **Step up** (less sensitive) quickly when the operating level itself
  shows a false-positive rate above 2% over ≥50 samples — false positives
  at the operating point cost real transmissions.
- The offset is persisted to flash whenever it steps.

At the default 15-second probe interval a step decision lands roughly
every **1–2 hours**, so the node tracks a changing RF environment within
that window without thrashing.

The offset is clamped to **−8…+12** levels around the family base — wide
enough that a dense hilltop can settle much less sensitive and a quiet
valley node much more sensitive. The driver additionally clamps the
absolute detPeak (SX126x 15–40, LR11xx/LR20xx 48–90). That absolute clamp
is a *firmware guardrail*, not a chip limit — `cadDetPeak` is a full
8-bit register (0–255) — it simply stops the staircase from wandering
into "CAD never fires" (detPeak too high → LBT effectively off) or "CAD
always busy" (too low → node can't transmit) territory. Quiet sites
converge *below* the standard value — more sensitive LBT than any
fixed-config firmware, meaning fewer TX-over-RX stomps. Noisy sites
ratchet up until false positives vanish.

To watch or hand-tune instead of letting it self-adjust, set
`cad.auto off` (see below).

## Reading the status

Out of the box the node calibrates itself — you don't have to do
anything. To see what it's doing:

```
get cad
```

Compact output (kept short so it survives a truncated LoRa reply):

```
> a:on o:-1 pk:20(b21/4s) iv:15s
-2(19) 241p 9b 7f 2t 3%
-1(20) 900p 5b 3f 2t 0%
+0(21) 300p 2b 0f 2t 0%
```

Header: `a` auto on/off · `o` operating offset · `pk` operating detPeak ·
`b` family base · `4s` 4 symbols · `iv` probe interval.
Per level: `level(peak) probes busy fp tp fp%%` — `fp` = suspected false
positives, `tp` = busy verdicts confirmed by a real packet, `fp%%` =
integer false-positive rate. Only levels that have been probed print, so
in auto mode you'll usually see the operating level and its frontier.

Here the staircase has already stepped to offset −1 (peak 20): the −1
level is clean (0%) over 900 probes while −2 shows a 3% jump, so −1 is
this site's knee.

### Dry-run / manual tuning

Prefer to observe or pin the value yourself? Turn the staircase off:

```
set cad.auto off
```

Probing continues (stats keep updating) but the operating offset no
longer moves. In dry-run the probe sweep covers levels −4…+4 evenly, so
`get cad` shows the whole false-positive-vs-detPeak curve. Read it
bottom-up: the level where `fp%%` jumps from ~0 is the knee; sit one step
above it. Then pin it:

```
set cad.offset -1
```

Negative offset = more sensitive LBT (catches weaker signals, risks false
busy); positive = less sensitive. Range −8…+12. Re-enable the staircase
any time with `set cad.auto on`. Offset changes (manual or automatic)
appear in the log as `cad: step down/up -> offset N`.

To collect dry-run data faster, drop the interval (probing is temporary
then): `set cad.probe.interval 10`. At the default 15 s, allow ~a day for
a knee to resolve clearly, longer to capture day/night variation.

## Command reference

| Command | Default | Description |
|---|---|---|
| `get cad` | | Status + per-level statistics (see above). |
| `set cad.auto <on\|off>` | **on** | Staircase controller acts on the stats. Off = observe/hand-tune. |
| `set cad.offset <n>` | 0 | Operating offset, −8…12. Negative = more sensitive. Applied live. |
| `set cad.probe.interval <sec>` | 15 | Probe cadence; 0 disables probing (and freezes auto), 10–255 otherwise. |
| `set cad.reset` | | Clear accumulated statistics (RAM only). |

All settings persist in prefs and apply to every role — repeater, room
server, and companion (companions reach the CLI via the v-contact admin
chat or USB serial). Command keywords are case-insensitive (`Get cad`
works); command *values* like passwords and node names are not, so `on`
/`off` must be lowercase.

## Notes and limits

- **SX127x boards** (TTGO LoRa32, T-Beam classic) have no hardware CAD;
  `get cad` reports `not available` and the settings are inert. Their LBT
  remains the RSSI-based `int.thresh` gate.
- **The false-positive side is measured; the miss side mostly is not.**
  A too-high detPeak shows up as on-air collisions, which a single node
  cannot observe. That is why the workflow is "find the lowest clean
  level and sit at it", never "raise it until problems stop being
  visible". The `tp` column is the one local miss-side signal: probes
  that detected real below-noise-floor traffic.
- **detPeak is the only adaptive knob.** `cadDetMin` stays at Semtech's
  universal 10; the symbol count is fixed at 4 (better mid-payload
  detection than 2 — most of a mesh packet's airtime is payload, and
  that is where a pre-TX CAD usually lands). The drivers scale their
  CAD timeout with symbol count and preset automatically.
- Statistics are RAM-only and restart after a reboot; the learned
  *offset* is persisted. Mixed fleets are fine — this feature changes
  only when *this* node decides the channel is busy, not anything on
  the air.
- Related but separate: `int.thresh` (RSSI-above-floor software gate)
  and `dc.restarts` (sniff-mode false-preamble counter) still work as
  before; adaptive CAD complements them.
