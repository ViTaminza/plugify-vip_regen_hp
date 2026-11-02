// vip_regen_hp unit tests — the engine-independent part of the plugin (src/regen_hp_core.hpp and
// src/round_rule.hpp): the healing step (the original RegenHP() body: continue while below max,
// clamp at max, stop at full health), the regen-amount guard, the slot range, the per-slot
// countdown that replaced the original's CTimer (delay -> tick -> interval -> stop, with the
// PITFALLS C9 clamp) and the "RegenHP_round_min" round rule.
//
// No pisexcompat and no s2sdk binding is linked here: the test only includes the header.
//
// Run by ../../test.sh (inside the Steam Runtime image, cwd = build/tests).

#include "regen_hp_core.hpp"
#include "round_rule.hpp"

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>

namespace {

int g_checks = 0;
int g_failures = 0;

void CheckEq(int actual, int expected, const std::string& what) {
	++g_checks;
	if (actual != expected) {
		++g_failures;
		std::printf("FAIL: %s (expected %d, got %d)\n", what.c_str(), expected, actual);
	}
}

void CheckEq(bool actual, bool expected, const std::string& what) {
	++g_checks;
	if (actual != expected) {
		++g_failures;
		std::printf("FAIL: %s (expected %d, got %d)\n", what.c_str(), expected ? 1 : 0, actual ? 1 : 0);
	}
}

void CheckNear(float actual, float expected, const std::string& what) {
	++g_checks;
	if (std::fabs(actual - expected) > 1e-6f) {
		++g_failures;
		std::printf("FAIL: %s (expected %f, got %f)\n", what.c_str(), static_cast<double>(expected),
					static_cast<double>(actual));
	}
}

// --- the original RegenHP() body ---------------------------------------------------------------

void TestStep() {
	using vip_regen_hp::Step;

	// The shipped groups.ini uses RegenHP 2..10.
	const auto below = Step(50, 100, 2);
	CheckEq(below.healed, 52, "heals the step");
	CheckEq(below.continueRegen, true, "keeps regenerating below max");

	const auto atMax = Step(100, 100, 2);
	CheckEq(atMax.healed, 100, "full health is left alone");
	CheckEq(atMax.continueRegen, false, "full health stops the timer");

	const auto clamped = Step(99, 100, 10);
	CheckEq(clamped.healed, 100, "the over-heal is clamped to max");
	CheckEq(clamped.continueRegen, false, "the clamped step stops the timer");

	const auto zero = Step(50, 100, 0);
	CheckEq(zero.healed, 50, "a zero step heals nothing");
	CheckEq(zero.continueRegen, true, "a zero step keeps the original's timer alive");

	const auto over = Step(120, 100, 5);
	CheckEq(over.healed, 120, "above max nothing is written");
	CheckEq(over.continueRegen, false, "above max the timer stops");

	// A 1 HP pawn healed by 1 reaches max exactly: the original continued (hp == max was only
	// checked at the *start* of the next run).
	const auto exact = Step(99, 100, 1);
	CheckEq(exact.healed, 100, "an exact heal reaches max");
	CheckEq(exact.continueRegen, true, "an exact heal keeps the timer for one more run");
}

void TestHasRegenAmount() {
	using vip_regen_hp::HasRegenAmount;

	// The shipped groups.ini uses RegenHP 2..10: a positive value keeps the timer alive.
	CheckEq(HasRegenAmount(2), true, "a positive amount keeps regenerating");

	// The core answers -1 once there is no VIP record or no feature (expiry, removal, group
	// change, reload, a toggled-off player) and 0 for a group value of "0": Regen() stops on both.
	CheckEq(HasRegenAmount(-1), false, "no VIP record stops the timer");
	CheckEq(HasRegenAmount(0), false, "zero group value stops the timer");

	// What Step() did with the -1 the core reports for an ex-VIP: one HP drained per run, forever.
	const auto drained = vip_regen_hp::Step(50, 100, -1);
	CheckEq(drained.healed, 49, "a -1 step drains one HP");
	CheckEq(drained.continueRegen, true, "a -1 step keeps the timer alive");
}

// --- slots and the delay clamp -----------------------------------------------------------------

void TestSlots() {
	using vip_regen_hp::IsSlotInRange;
	using vip_regen_hp::IsUsableEntityHandle;
	using vip_regen_hp::kMaxSlots;

	CheckEq(kMaxSlots, 64, "CS2 has 64 player slots");
	CheckEq(IsSlotInRange(0), true, "slot 0 is valid");
	CheckEq(IsSlotInRange(63), true, "slot 63 is valid");
	CheckEq(IsSlotInRange(64), false, "slot 64 is out of range");
	CheckEq(IsSlotInRange(-1), false, "slot -1 is out of range");

	CheckEq(IsUsableEntityHandle(0), false, "entity handle 0 (world) is not a pawn");
	CheckEq(IsUsableEntityHandle(-1), false, "entity handle -1 is the invalid sentinel");
	CheckEq(IsUsableEntityHandle(42), true, "positive entity handle is usable");
	CheckEq(IsUsableEntityHandle(-4242), true, "negative high-serial entity handle is usable (PITFALLS C1)");
	CheckEq(IsUsableEntityHandle(std::numeric_limits<int32_t>::min()), true, "INT32_MIN entity handle is usable");
}

void TestSafeTickSeconds() {
	using vip_regen_hp::kMinTickSeconds;
	using vip_regen_hp::SafeTickSeconds;

	CheckNear(kMinTickSeconds, 1.0f / 64.0f, "the tick floor is 1/64 s (PITFALLS C9)");
	CheckNear(SafeTickSeconds(5.0f), 5.0f, "a real delay is kept");
	CheckNear(SafeTickSeconds(0.0f), kMinTickSeconds, "a zero delay becomes the tick floor");
	CheckNear(SafeTickSeconds(-3.0f), kMinTickSeconds, "a negative delay becomes the tick floor");
	CheckNear(SafeTickSeconds(kMinTickSeconds), kMinTickSeconds, "the floor itself is kept");
	// A NaN comparison is false: it must become the floor too, never a countdown that never ends.
	const float nan = std::numeric_limits<float>::quiet_NaN();
	CheckNear(SafeTickSeconds(nan), kMinTickSeconds, "a NaN delay becomes the tick floor");
}

// --- the per-slot countdown --------------------------------------------------------------------

void TestTimerLifecycle() {
	using vip_regen_hp::RegenTimer;

	RegenTimer timer;
	CheckEq(timer.Active(), false, "a fresh timer is disarmed");
	CheckEq(timer.Advance(1.0f), false, "a disarmed timer never fires");

	// The original: CreateTimer(Delay) -> first run after the delay.
	timer.Arm(5.0f);
	CheckEq(timer.Active(), true, "Arm activates the timer");
	CheckNear(timer.Remaining(), 5.0f, "Arm starts from the delay");
	CheckEq(timer.Advance(2.0f), false, "not due after 2 of 5 seconds");
	CheckEq(timer.Advance(2.0f), false, "not due after 4 of 5 seconds");
	CheckEq(timer.Advance(1.0f), true, "due after the whole delay");

	// A due timer stays due until the callback answers (the original returned a delay or -1).
	CheckEq(timer.Advance(0.016f), true, "a due timer stays due until rescheduled");

	// The callback answered Interval: wait another interval, then fire again.
	timer.Reschedule(2.0f);
	CheckNear(timer.Remaining(), 2.0f, "Reschedule starts a new interval");
	CheckEq(timer.Advance(1.0f), false, "not due after half the interval");
	CheckEq(timer.Advance(1.0f), true, "due after the whole interval");

	// The callback answered -1: the timer stops for good.
	timer.Disarm();
	CheckEq(timer.Active(), false, "Disarm stops the timer");
	CheckNear(timer.Remaining(), 0.0f, "Disarm clears the countdown");
	CheckEq(timer.Advance(100.0f), false, "a disarmed timer does not fire again");
}

void TestTimerClamp() {
	using vip_regen_hp::kMinTickSeconds;
	using vip_regen_hp::RegenTimer;

	// A misconfigured "DelayRegenHP" 0 must not make the slot due on the first frame either: the
	// countdown starts at the 1/64 s floor (PITFALLS C9).
	RegenTimer timer;
	timer.Arm(0.0f);
	CheckNear(timer.Remaining(), kMinTickSeconds, "a zero delay arms at the tick floor");
	CheckEq(timer.Advance(kMinTickSeconds / 2.0f), false, "the clamped delay is not due at once");
	CheckEq(timer.Advance(kMinTickSeconds / 2.0f), true, "the clamped delay expires after 1/64 s");

	// Same floor for a later interval: an IntervalRegenHP of 0 cannot spin OnPluginUpdate.
	timer.Reschedule(0.0f);
	CheckNear(timer.Remaining(), kMinTickSeconds, "a zero interval reschedules at the tick floor");
	CheckEq(timer.Advance(kMinTickSeconds / 2.0f), false, "the clamped interval is not due at once");
	CheckEq(timer.Advance(kMinTickSeconds / 2.0f), true, "the clamped interval expires after 1/64 s");

	timer.Reschedule(3.0f);
	CheckNear(timer.Remaining(), 3.0f, "a real interval is kept");
}

void TestTimerRejectsBadDelta() {
	using vip_regen_hp::RegenTimer;

	// A stalled or mis-signalled frame must not count down (and a NaN dt must never make the timer
	// due on every frame).
	RegenTimer timer;
	timer.Arm(0.125f); // exact in binary, so the arithmetic below is deterministic
	CheckEq(timer.Advance(0.0f), false, "a zero dt does not advance");
	CheckEq(timer.Advance(-1.0f), false, "a negative dt does not advance");
	const float nan = std::numeric_limits<float>::quiet_NaN();
	CheckEq(timer.Advance(nan), false, "a NaN dt does not advance");
	CheckNear(timer.Remaining(), 0.125f, "the countdown is untouched by a bad dt");
	CheckEq(timer.Advance(0.0625f), false, "still not due after half the delay");
	CheckEq(timer.Advance(0.0625f), true, "due after the whole delay");
}

void TestTimerLargeDelta() {
	using vip_regen_hp::RegenTimer;

	// A huge frame (a hitch) makes the timer due once, not once per elapsed interval: the plugin
	// heals at most once per frame and reschedules, so OnPluginUpdate can never loop here.
	RegenTimer timer;
	timer.Arm(0.0625f);
	CheckEq(timer.Advance(10.0f), true, "a 10 s frame is due");
	timer.Reschedule(2.0f);
	CheckNear(timer.Remaining(), 2.0f, "Reschedule rewinds to the interval, not below zero");
	CheckEq(timer.Advance(10.0f), true, "the next huge frame is due once");
}

// --- the optional "RegenHP_round_min" round rule -----------------------------------------------

vip_regen_hp::RoundState RoundRuleState(bool available, bool pistolRound, int totalRounds) {
	vip_regen_hp::RoundState state;
	state.available = available;
	state.pistolRound = pistolRound;
	state.totalRounds = totalRounds;
	return state;
}

void TestRoundRule() {
	using vip_regen_hp::RoundAllowed;

	// Missing key: the core answers -1, which the policy treats exactly like 1. This is the
	// reported regression - the module used to apply during the pistol round.
	CheckEq(RoundAllowed(-1, RoundRuleState(true, false, 1)), true,
			"missing round_min applies from the second round on");
	CheckEq(RoundAllowed(-1, RoundRuleState(true, false, 0)), false,
			"missing round_min fails closed on the first map before round_prestart (total 0)");
	CheckEq(RoundAllowed(-1, RoundRuleState(true, true, 0)), false,
			"missing round_min never applies on the first pistol round");
	CheckEq(RoundAllowed(-1, RoundRuleState(true, false, 5)), true,
			"missing round_min applies in an ordinary later round");
	CheckEq(RoundAllowed(-1, RoundRuleState(true, true, 12)), false,
			"missing round_min never applies on the second-half pistol round (total 12)");
	CheckEq(RoundAllowed(-5, RoundRuleState(true, false, 1)), true,
			"any negative value is normalised to 1");

	// 0 explicitly allows every round, pistol rounds included.
	CheckEq(RoundAllowed(0, RoundRuleState(true, true, 0)), true,
			"round_min 0 allows the first pistol round");
	CheckEq(RoundAllowed(0, RoundRuleState(true, true, 12)), true,
			"round_min 0 allows the second-half pistol round");
	CheckEq(RoundAllowed(0, RoundRuleState(true, false, 0)), true,
			"round_min 0 allows the first map before round_prestart");

	// 1 is non-pistol and needs at least one COMPLETED round (totalRounds, not the displayed one).
	CheckEq(RoundAllowed(1, RoundRuleState(true, false, 0)), false,
			"round_min 1 fails closed before round_prestart even when the pistol flag is false");
	CheckEq(RoundAllowed(1, RoundRuleState(true, true, 0)), false,
			"round_min 1 never applies on the first pistol round");
	CheckEq(RoundAllowed(1, RoundRuleState(true, false, 1)), true,
			"round_min 1 applies from the second regular round (one completed)");
	CheckEq(RoundAllowed(1, RoundRuleState(true, false, 5)), true,
			"round_min 1 applies in ordinary later rounds");
	CheckEq(RoundAllowed(1, RoundRuleState(true, true, 12)), false,
			"round_min 1 never applies on the second-half pistol round");

	// A higher minimum keeps the existing vip_bf / vip_items semantics.
	CheckEq(RoundAllowed(3, RoundRuleState(true, false, 2)), false,
			"round_min 3 does not apply with only 2 completed rounds");
	CheckEq(RoundAllowed(3, RoundRuleState(true, false, 3)), true,
			"round_min 3 applies with 3 completed rounds");
	CheckEq(RoundAllowed(3, RoundRuleState(true, true, 12)), false,
			"round_min 3 never applies on a pistol round");
	CheckEq(RoundAllowed(2, RoundRuleState(true, false, 1)), false,
			"round_min 2 does not apply with only 1 completed round");

	// The round API must be available: without it nothing is enabled, not even an explicit 0.
	CheckEq(RoundAllowed(1, RoundRuleState(false, false, 12)), false,
			"an unavailable round API disables the effect");
	CheckEq(RoundAllowed(0, RoundRuleState(false, false, 12)), false,
			"an unavailable round API disables the effect even for an explicit 0");
	CheckEq(RoundAllowed(-1, RoundRuleState(false, true, 12)), false,
			"an unavailable round API never enables the effect");

	// A nonsensical completed-round count fails closed as well.
	CheckEq(RoundAllowed(0, RoundRuleState(true, false, -1)), false,
			"a negative completed-round count fails closed for round_min 0");
	CheckEq(RoundAllowed(1, RoundRuleState(true, false, -1)), false,
			"a negative completed-round count fails closed for round_min 1");
}

void TestDeniedRoundNoCatchUp() {
	using vip_regen_hp::RegenTimer;
	using vip_regen_hp::RoundAllowed;

	// OnPluginUpdate disarms the armed countdown when the current round denies it; the later
	// allowed rounds must not fire it (no debt from the skipped regeneration).
	RegenTimer timer;
	timer.Arm(1.0f);
	CheckEq(timer.Active(), true, "the countdown is armed before the denied round");
	CheckEq(RoundAllowed(-1, RoundRuleState(true, true, 12)), false,
			"the second-half pistol round denies the countdown");
	timer.Disarm(); // what the per-frame gate does on a denied round
	CheckEq(timer.Active(), false, "a denied round disarms the countdown");
	CheckEq(timer.Advance(10.0f), false, "the disarmed countdown never fires");
	CheckEq(RoundAllowed(-1, RoundRuleState(true, false, 13)), true,
			"the next regular round would allow the effect");
	CheckEq(timer.Advance(100.0f), false, "no catch-up heal once the round turns allowed again");
}

} // namespace

int main() {
	std::printf("vip_regen_hp tests\n");
	TestStep();
	TestHasRegenAmount();
	TestSlots();
	TestSafeTickSeconds();
	TestTimerLifecycle();
	TestTimerClamp();
	TestTimerRejectsBadDelta();
	TestTimerLargeDelta();
	TestRoundRule();
	TestDeniedRoundNoCatchUp();
	std::printf("%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
