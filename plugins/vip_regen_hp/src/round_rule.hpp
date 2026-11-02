#pragma once
// vip_regen_hp — the optional per-group "RegenHP_round_min" key and the round condition that gates
// the health regeneration.
//
// The key lives in the existing vip/groups.ini group section and is read with the existing
// VIP_GetClientFeatureInt, so the core answers -1 when the group does not set it (no VIP record, an
// expired status or a disabled cookie answer -1 as well). The agreed policy:
//
//   -1 / any negative value  treated exactly like 1 (NOT "disabled"): a group that does not set the
//                            key gets the new safe default. This intentionally fixes the reported
//                            bug - the module used to apply the effect during the pistol round.
//   0                        every round, pistol rounds included.
//   1                        non-pistol rounds only, and only once at least one round has COMPLETED.
//                            On the first map before round_prestart VIP_GetTotalRounds() is still 0
//                            while VIP_PistolRound() can already be false: that fails closed.
//   >=2                      the existing vip_bf / vip_items round_min convention: non-pistol and at
//                            least that many completed rounds.
//
// A "pistol round" includes the second-half pistol: VIP_PistolRound() reports the team switch at
// the round reset, not only the first round of the map.
//
// The state is sampled once per frame by the plugin (a cross-plugin call), never once per slot. It
// carries an availability flag: when either __vip_VIP_PistolRound or __vip_VIP_GetTotalRounds is
// missing the round cannot be proven safe, so the effect stays disabled.
//
// Engine-free: tests/vip_regen_hp_tests.cpp includes this header directly.

namespace vip_regen_hp {

struct RoundState {
	bool available = false;   // both VIP round bindings are present and answered
	bool pistolRound = false; // VIP_PistolRound(): first round, second-half pistol, or a restart
	int totalRounds = 0;      // VIP_GetTotalRounds(): completed rounds, 0 before the first one
};

// The value a missing/negative key resolves to (the default of the round_min convention).
inline constexpr int kDefaultRoundMin = 1;

// VIP_GetClientFeatureInt answers -1 for a missing key: treat it (and any other negative value) as
// an explicit minimum of one instead of "always" or "disabled".
inline int NormalizeRoundMin(int roundMin) {
	return roundMin < 0 ? kDefaultRoundMin : roundMin;
}

// Whether the current round allows the regeneration.
inline bool RoundAllowed(int roundMin, const RoundState& round) {
	// An unavailable round API or a nonsensical round count never enables the effect - not even for
	// an explicit 0. The plugin must fail closed when the round cannot be proven safe.
	if (!round.available || round.totalRounds < 0) {
		return false;
	}
	roundMin = NormalizeRoundMin(roundMin);
	if (roundMin == 0) {
		return true;
	}
	if (round.pistolRound) {
		return false;
	}
	// round_min 1 needs one completed round; a larger value keeps the vip_bf/items semantics.
	return round.totalRounds >= roundMin;
}

} // namespace vip_regen_hp
