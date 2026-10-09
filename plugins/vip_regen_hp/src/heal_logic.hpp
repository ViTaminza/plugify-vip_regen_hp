#pragma once
// Engine-free healing step of the VIP_RegenHP port (the original RegenHP() body):
//
//   hp < maxHP:  hp += amount; if (hp > maxHP) { set maxHP; continue = false; } else continue = true
//   otherwise:   nothing happens (continue = false)
//
// `healed` is the value to write, `continueRegen` whether the timer keeps running (the original's
// return value).

namespace vip_regen_hp {

struct HealStep {
	int healed = 0;
	bool continueRegen = false;
};

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

} // namespace vip_regen_hp
