#pragma once
// vip_regen_hp — the engine-free core of the plugin: the healing step, the per-slot regeneration
// countdown and the delay clamp. Shared verbatim by src/plugin.cpp and the unit test
// (tests/vip_regen_hp_tests.cpp), so the arithmetic the countdown depends on is tested without
// the engine and without a single s2sdk/pisexcompat symbol.
//
// The plugin drives these countdowns from OnPluginUpdate instead of s2sdk::CreateTimer: the
// original used one CTimer per slot whose callback returned the next delay (or a negative value
// to stop), and the countdown reproduces exactly that (Arm = the first delay, Advance = the
// timer firing, Reschedule = the callback's positive return, Disarm = the callback's -1).

#include <cstdint>

namespace vip_regen_hp {

// CS2 has exactly 64 player slots (PITFALLS C2); the original iterated 0..MAXPLAYERS-1.
constexpr int kMaxSlots = 64;

// PITFALLS C9: a timer delay below one server tick is dangerous on s2sdk - a repeating timer
// with delay 0 re-arms itself inside TimerSystem::RunFrame forever, the main thread hangs and the
// engine watchdog kills the server. The port counts down in OnPluginUpdate, but keeps the same
// floor so a bad groups.ini value ("DelayRegenHP" 0, a negative value, a NaN) can never make a
// slot due on every single frame: the first delay and every later interval are at least 1/64 s.
constexpr float kMinTickSeconds = 1.0f / 64.0f;

static_assert(kMinTickSeconds > 0.0f, "a non-positive countdown would spin OnPluginUpdate forever");

constexpr bool IsSlotInRange(int slot) {
	return slot >= 0 && slot < kMaxSlots;
}

// PITFALLS C1: an s2sdk entity int is a signed CEntityHandle (`index | (serial << 15)`). Only `0`
// (the world, never a player pawn) and `-1` (the invalid sentinel) are unusable; any other signed
// value — including negative handles once the serial counter reaches bit 31 — is a valid handle
// candidate that must be passed to `s2sdk::IsValidEntHandle`.
constexpr bool IsUsableEntityHandle(int32_t handle) {
	return handle != 0 && handle != -1;
}

// A delay/interval from the group's features, clamped to the tick floor. The comparison is
// inverted on purpose: NaN fails it as well, so a NaN value also becomes the floor instead of a
// countdown that never expires.
inline float SafeTickSeconds(float seconds) {
	return seconds >= kMinTickSeconds ? seconds : kMinTickSeconds;
}

// --- the healing step (the original RegenHP() body) --------------------------------------------

struct HealStep {
	int healed = 0;
	bool continueRegen = false;
};

// Whether a pending regeneration may still run. The core answers -1 when there is nothing to
// regenerate by (no VIP record any more - expiry, removal, a reused slot - a feature the group
// does not set, or a player who toggled it off) and 0 for a group value of "0"; Step() would then
// "heal" by that non-positive amount on every tick (minus one HP per run for -1).
inline bool HasRegenAmount(int amount) {
	return amount > 0;
}

// hp < maxHP:  hp += amount; if (hp > maxHP) { set maxHP; continue = false; } else continue = true
// otherwise:   nothing happens (continue = false)
inline HealStep Step(int currentHp, int maxHp, int amount) {
	if (currentHp >= maxHp) {
		return {currentHp, false};
	}
	const int healed = currentHp + amount;
	if (healed > maxHp) {
		return {maxHp, false};
	}
	return {healed, true};
}

// --- the per-slot regeneration countdown -------------------------------------------------------

class RegenTimer {
public:
	// Start the countdown with the group's DelayRegenHP (clamped to the tick floor).
	void Arm(float delaySeconds) {
		m_active = true;
		m_remaining = SafeTickSeconds(delaySeconds);
	}

	// Cancel the countdown: the slot no longer regenerates.
	void Disarm() {
		m_active = false;
		m_remaining = 0.0f;
	}

	// Keep the countdown alive for another interval (the original's callback returning a delay).
	void Reschedule(float intervalSeconds) {
		m_active = true;
		m_remaining = SafeTickSeconds(intervalSeconds);
	}

	// Counts the frame's time down; true when the healing step is due. A non-positive or NaN dt
	// does not move the countdown (the host has not run a real frame, e.g. the first update after
	// a load or a stalled server). A due timer stays due until Reschedule() or Disarm() is called,
	// so a huge dt heals at most once per frame per slot instead of looping here.
	bool Advance(float dtSeconds) {
		if (!m_active || !(dtSeconds > 0.0f)) {
			return false;
		}
		m_remaining -= dtSeconds;
		return m_remaining <= 0.0f;
	}

	bool Active() const {
		return m_active;
	}

	float Remaining() const {
		return m_remaining;
	}

private:
	bool m_active = false;
	float m_remaining = 0.0f;
};

} // namespace vip_regen_hp
