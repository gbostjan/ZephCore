/*
 * SPDX-License-Identifier: MIT
 * MeshTimeSync - mesh clock consensus from Ed25519-signed advert timestamps.
 *
 * Role-agnostic estimator: owns no clock. Each role feeds it signature-
 * verified adverts (onAdvertHeard), calls tick() from its loop, and applies
 * STEP verdicts under its own step policy (repeater/observer: bidirectional;
 * room server/companion: forward-only; GPS-synced boards: sense only).
 * ZephCore-only divergence from Arduino MeshCore — design rationale in
 * ARCHITECTURE.md, user-facing doc in MESHTIMESYNC.md.
 *
 * All policy timers anchor on uptime, never wall clock, so the very steps
 * they govern cannot distort them.
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef CONFIG_ZEPHCORE_TIMESYNC_TABLE_SIZE
  #define MESHTIMESYNC_TABLE_SIZE  CONFIG_ZEPHCORE_TIMESYNC_TABLE_SIZE
#else
  #define MESHTIMESYNC_TABLE_SIZE  32
#endif

#ifdef CONFIG_ZEPHCORE_TIMESYNC_QUORUM
  #define MESHTIMESYNC_QUORUM  CONFIG_ZEPHCORE_TIMESYNC_QUORUM
#else
  #define MESHTIMESYNC_QUORUM  6
#endif

#ifndef FIRMWARE_BUILD_EPOCH
  /* Injected by CMakeLists.txt (build-time UNIX epoch, the "provably dead
   * clock" floor). 0 disables bootstrap mode entirely. */
  #define FIRMWARE_BUILD_EPOCH 0u
#endif

class MeshTimeSync {
public:
	static constexpr uint8_t  HOP_CAP                  = 3;
	static constexpr int32_t  RADIUS_BASE_SECS         = 150;
	static constexpr int32_t  RADIUS_PER_HOP_SECS      = 15;
	static constexpr uint32_t TENURE_SECS              = 60 * 60;
	static constexpr uint16_t TENURE_MIN_ADVERTS       = 2;
	static constexpr uint32_t MAX_SAMPLE_AGE_SECS      = 5 * 24 * 3600;
	static constexpr uint32_t MATURE_SILENT_EVICT_SECS = 24 * 3600;
	static constexpr int64_t  CONSISTENCY_BASE_SECS    = 45;
	static constexpr int64_t  CONSISTENCY_PPM          = 150;
	static constexpr uint32_t EVAL_INTERVAL_SECS       = 15 * 60;
	static constexpr int64_t  DEAD_BAND_SECS           = 5 * 60;
	static constexpr int64_t  STEP_TRIGGER_SECS        = 10 * 60;
	static constexpr int64_t  STEP_CAP_SECS            = 3600;
	static constexpr uint32_t STEP_INTERVAL_SECS       = 6 * 3600;
	static constexpr uint32_t SUPPRESS_SECS            = 7 * 24 * 3600;
	static constexpr int64_t  PEDIGREE_PPM             = 300;
	static constexpr int64_t  PEDIGREE_BASE_SECS       = 10 * 60;
	static constexpr uint8_t  BOOTSTRAP_QUORUM         = 3;

	/* 8-byte prefix is a security floor, not a tuning knob: it is the
	 * sender's identity for tenure/votes while signatures verify the full
	 * key, so a shorter prefix lets an attacker grind keypairs to collide
	 * with a tenured honest voter and reset its tenure with validly-signed
	 * adverts. If RAM ever matters, cut slot count instead. */
	struct Slot {
		uint8_t  prefix[8];
		uint32_t advert_ts;        /* latest advert timestamp = the vote */
		uint32_t arrival_uptime;   /* monotonic anchor: skew is recomputed at
		                            * evaluate time, so a local clock step
		                            * never stales stored samples */
		uint32_t first_uptime;     /* tenure start */
		uint16_t count;            /* adverts this tenure */
		uint8_t  hops;             /* precision hint only, never a trust signal */
		uint8_t  used;
	};

	enum VerdictType : uint8_t { VERDICT_NONE = 0, VERDICT_ABSTAIN, VERDICT_STEP };

	enum Reason : uint8_t {
		REASON_NONE = 0,
		REASON_IN_BAND,       /* |skew| below the step trigger */
		REASON_NO_DATA,       /* no eligible voters */
		REASON_NO_QUORUM,
		REASON_NO_MAJORITY,
		REASON_SUPPRESSED,    /* manual clock set less than 7 days ago */
		REASON_RATE_LIMITED,  /* < 6 h since last applied step */
		REASON_PEDIGREE,      /* drift-envelope physics veto */
	};

	struct Consensus {
		bool valid;            /* >= 1 eligible vote, intersection computed */
		uint8_t eligible;
		uint8_t votes_for;     /* votes inside the best intersection */
		uint8_t votes_against;
		int64_t mid;           /* consensus skew midpoint (+ = our clock is behind) */
		int32_t radius;        /* intersection half-width */
	};

	struct Verdict {
		VerdictType type;
		Reason reason;
		int64_t delta;         /* seconds to add to the clock (STEP only) */
		bool bootstrap;
		Consensus consensus;
	};

	explicit MeshTimeSync(uint32_t build_epoch = 0) { reset(build_epoch); }

	void reset(uint32_t build_epoch);

	/* Feed one signature-verified advert. hops = flood path length (0 = heard
	 * direct). Samples beyond HOP_CAP are dropped. */
	void onAdvertHeard(const uint8_t *pubkey, uint32_t advert_ts, uint8_t hops,
	                   uint32_t uptime_secs);

	/* Paced evaluation — returns VERDICT_NONE/REASON_NONE unless
	 * EVAL_INTERVAL_SECS elapsed since the last real evaluation. The caller
	 * applies STEP verdicts under its role policy and reports the outcome
	 * via noteStepApplied() (the step rate limit counts applied steps only). */
	Verdict tick(uint32_t local_time, uint32_t uptime_secs);

	/* Unpaced consensus computation (CLI dry-run view). */
	Consensus computeConsensus(uint32_t local_time, uint32_t uptime_secs,
	                           bool bootstrap) const;

	/* Unpaced full policy evaluation (no counter/pacing side effects) —
	 * what tick() would decide right now. Used by the CLI dry-run. */
	Verdict evaluateNow(uint32_t local_time, uint32_t uptime_secs) const;

	bool isBootstrap(uint32_t local_time) const { return local_time < _build_epoch; }

	/* Manual clock set (CLI time/clock sync, app time set): arms the 7-day
	 * suppression window AND drift-envelope pedigree. Suppression gates
	 * bootstrap too. */
	void noteManualSync(uint32_t uptime_secs);
	/* GPS time sync: arms pedigree only (stepping is gated off by the role
	 * whenever GPS is available and enabled, so no suppression needed). */
	void noteGPSSync(uint32_t uptime_secs);
	/* Report an applied step (rate-limit anchor + counters). local_time is
	 * the wall clock AFTER the step (display only). */
	void noteStepApplied(int64_t delta, uint32_t local_time, uint32_t uptime_secs,
	                     bool bootstrap);
	/* Forward-only roles report a refused backward verdict. */
	void noteBackwardSkipped() { _backward_skips++; }

	bool isSuppressed(uint32_t uptime_secs) const;
	uint32_t suppressRemaining(uint32_t uptime_secs) const;

	/* Table access for the CLI evidence dump. */
	static int tableSize() { return MESHTIMESYNC_TABLE_SIZE; }
	const Slot &slotAt(int i) const { return _slots[i]; }
	bool slotEligible(const Slot &s, uint32_t uptime_secs) const;
	int64_t slotSkew(const Slot &s, uint32_t local_time, uint32_t uptime_secs) const;

	/* Counters for CLI/stats. */
	uint32_t evalCount() const { return _evals; }
	uint32_t abstainCount() const { return _abstains; }
	uint32_t stepCount() const { return _steps; }
	uint32_t bootstrapStepCount() const { return _bootstrap_steps; }
	uint32_t backwardSkipCount() const { return _backward_skips; }
	int64_t lastStepDelta() const { return _last_step_delta; }
	uint32_t lastStepWall() const { return _last_step_wall; }
	bool hasStepped() const { return _stepped_once; }

	/* Compact status + evidence-table formatter shared by all role CLIs.
	 * Writes at most `cap` bytes (NUL-terminated), summary first, then as
	 * many per-sender entries as fit. */
	int formatStatus(char *out, size_t cap, uint32_t local_time,
	                 uint32_t uptime_secs, bool enabled) const;

	static const char *reasonStr(Reason r);

private:
	Slot *findSlot(const uint8_t *prefix);
	bool slotTenured(const Slot &s, uint32_t uptime_secs) const;

	Slot _slots[MESHTIMESYNC_TABLE_SIZE];
	uint32_t _build_epoch;

	/* Policy state — uptime-anchored (see header comment). */
	uint32_t _next_eval_uptime;
	uint32_t _suppress_uptime;
	bool _suppressed;
	uint32_t _pedigree_uptime;
	bool _pedigree;
	uint32_t _last_step_uptime;
	bool _stepped_once;

	/* Counters. */
	uint32_t _evals, _abstains, _steps, _bootstrap_steps, _backward_skips;
	int64_t _last_step_delta;
	uint32_t _last_step_wall;
};
