// vip_regen_hp unit tests — the engine-independent part: the healing step (the original RegenHP()
// body: continue while below max, clamp at max, stop at full health).
//
// Run by ../../test.sh (inside the Steam Runtime image, cwd = build/tests).

#include "heal_logic.hpp"

#include <cstdio>
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

void TestStep() {
	using vip_regen_hp::Step;

	// The production groups.ini uses RegenHP 2..10.
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
}

} // namespace

int main() {
	std::printf("vip_regen_hp tests\n");
	TestStep();
	std::printf("%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
