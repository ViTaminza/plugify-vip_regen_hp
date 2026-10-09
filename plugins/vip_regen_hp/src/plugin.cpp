// vip_regen_hp — Plugify port of the Pisex VIP module "VIP_RegenHP"
// (ref/src/cs2-vip-modules/VIP_RegenHP/source/vip_regen_hp.cpp).
//
// Behaviour kept identical:
//   * features: "RegenHP" (VIP_BOOL, TOGGLABLE, toggle + display callbacks),
//     "DelayRegenHP" and "IntervalRegenHP" (VIP_INT, HIDE) - the delay before regeneration starts
//     and the interval between ticks, both in seconds, read as floats from the group.
//   * a "player_hurt" pre-hook: when a VIP with the feature (and a dmg_health and a health value)
//     whose pawn is on a real team and alive is hurt, an old regeneration timer is removed and a
//     new one starts with the DelayRegenHP delay.
//   * the timer heals RegenHP health per run and reschedules itself with IntervalRegenHP while it
//     healed something; it stops when the pawn is at full health, when the timer cannot read the
//     pawn, or when another hurt restarts it.
//   * the toggle callback clears the pending regeneration when the feature is switched off and
//     *blocks* the toggle (the original returned true), and the display callback renders
//     "<name> [<RegenHP> HP/<IntervalRegenHP> s.]".
//   * loaded/disconnect/spawn reset the regeneration flag of the slot.
//
// Port notes:
//   * pPawn->m_iHealth()/m_iMaxHealth() + SetStateChanged -> s2sdk::GetClientHealth /
//     GetClientMaxHealth / SetClientHealth plus pisex::SetStateChanged, exactly like
//     plugins/as_players does for the same fields.
//   * CTimer of the original -> pisex::CreateTimer (the callback returns the next delay, negative
//     stops), driven by pisex::Poll().
//   * the pawn/team checks mirror the original: not alive, or m_iTeamNum < 2, stops the healing.
//   * the "player_hurt" handler resolves the victim's slot with IGameEvent::GetPlayerSlot("userid"),
//     the accessor the s2sdk game-events guide prescribes (ref/docs/live/
//     plugins_s2sdk_guides_game-events.txt:31,163). The original read the raw GetInt("userid"),
//     i.e. the engine user id. On the test server a human's user id equals the slot (connect log
//     "userid=8" for slot 8, kickid 8 kicked that slot) while bots carry other ids (256+ in
//     status), so the original worked for the VIP players it serves; GetPlayerSlot is correct for
//     every player.

#include <pisex/pisex.hpp>
#include <plg/plugin.hpp>
#include <plugin_export.h>
#include <s2sdk/s2sdk.hpp>

#include "heal_logic.hpp"
#include "vip_api_compat.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <string>
#include <string_view>

namespace {

// The ported modules keep the original code's unqualified spellings (VIP_INT, TOGGLABLE,
// DISABLED, ...): the consumer wrapper declares them as plain enums in namespace vip.
using namespace vip;

IVIPApi* g_pVIPCore = &IVIPApi::Get();

constexpr int kMaxSlots = 64;
constexpr const char* kFeature = "RegenHP";
constexpr const char* kDelayFeature = "DelayRegenHP";
constexpr const char* kIntervalFeature = "IntervalRegenHP";

// The delay/interval come from the group's features. A 0 second delay is FATAL on s2sdk:
// TimerSystem::RunFrame() re-inserts a repeat timer with executeTime = universalTime + delay and
// keeps looping while any timer is due, so a 0 delay stays due forever (the server frame never
// returns -> "WatchDog! Server took too long to process" -> fatal). The shipped groups use
// 4..7 seconds; a misconfigured 0 (or a negative value) is clamped to 50 ms instead of hanging.
constexpr float kMinTimerInterval = 0.05f;

float SafeTimerInterval(float seconds) {
	return seconds > kMinTimerInterval ? seconds : kMinTimerInterval;
}

static_assert(kMinTimerInterval > 0.0f, "a 0 second interval hangs s2sdk's TimerSystem::RunFrame loop");

void LogInfo(std::string_view message) {
	if (plg::Log) {
		plg::Log(message, plg::Severity::Info, plg::source_location::current());
	}
}

void LogError(std::string_view message) {
	if (plg::Log) {
		plg::Log(message, plg::Severity::Error, plg::source_location::current());
	}
}

class RegenHpPlugin final : public plg::Plugin {
public:
	plg::PluginResult OnPluginStart() override;
	plg::PluginResult OnPluginUpdate(std::chrono::milliseconds dt) override;
	plg::PluginResult OnPluginEnd() override;

private:
	// RegenHP() of the original: one healing step; true = healed (keep the timer alive).
	bool Regen(int iSlot);
	void CreateRegenTimer(int iSlot);
	void StopTimer(int iSlot);
	void ResetSlot(int iSlot);
	void OnPlayerHurt(const char* szName, pisex::IGameEvent* pEvent, bool bDontBroadcast);

	std::array<bool, kMaxSlots> m_regen{};
	std::array<pisex::CTimer*, kMaxSlots> m_timers{};
};

RegenHpPlugin g_regenHpPlugin;

plg::PluginResult RegenHpPlugin::OnPluginStart() {
	try {
		// Never fatal: only logs when the s2sdk bindings are missing.
		pisex::Init("vip_regen_hp");

		g_pVIPCore->VIP_RegisterFeature(kFeature, VIP_BOOL, TOGGLABLE, nullptr,
										[this](int iSlot, const char* /*szFeature*/, VIP_ToggleState /*eOldStatus*/,
											   VIP_ToggleState& eNewStatus) {
											if (eNewStatus == DISABLED && iSlot >= 0 && iSlot < kMaxSlots) {
												m_regen[static_cast<std::size_t>(iSlot)] = false;
											}
											return true; // the original blocked the toggle
										},
										[this](int iSlot, const char* szFeature) {
											char buffer[128];
											std::snprintf(buffer, sizeof(buffer), "%s [%i HP/%i s.]",
														  g_pVIPCore->VIP_GetTranslate(szFeature),
														  g_pVIPCore->VIP_GetClientFeatureInt(iSlot, kFeature),
														  g_pVIPCore->VIP_GetClientFeatureInt(iSlot, kIntervalFeature));
											return std::string(buffer);
										});
		g_pVIPCore->VIP_RegisterFeature(kDelayFeature, VIP_INT, HIDE);
		g_pVIPCore->VIP_RegisterFeature(kIntervalFeature, VIP_INT, HIDE);

		pisex::HookEvent("player_hurt", [this](const char* szName, pisex::IGameEvent* pEvent, bool bDontBroadcast) {
			OnPlayerHurt(szName, pEvent, bDontBroadcast);
		});

		g_pVIPCore->VIP_OnClientLoaded([this](int iSlot, bool /*bIsVIP*/) { ResetSlot(iSlot); });
		g_pVIPCore->VIP_OnClientDisconnect([this](int iSlot, bool /*bIsVIP*/) { ResetSlot(iSlot); });
		g_pVIPCore->VIP_OnPlayerSpawn([this](int iSlot, int /*iTeam*/, bool /*bIsVIP*/) { ResetSlot(iSlot); });

		LogInfo("vip_regen_hp: ready");
	} catch (const std::exception& ex) {
		LogError(std::string("vip_regen_hp: OnPluginStart failed: ") + ex.what());
	} catch (...) {
		LogError("vip_regen_hp: OnPluginStart failed with an unknown exception");
	}
	return {};
}

plg::PluginResult RegenHpPlugin::OnPluginUpdate(std::chrono::milliseconds /*dt*/) {
	pisex::Poll(); // timer reschedules and event pump
	return {};
}

plg::PluginResult RegenHpPlugin::OnPluginEnd() {
	for (pisex::CTimer*& timer : m_timers) {
		if (timer != nullptr) {
			pisex::RemoveTimer(timer);
			timer = nullptr;
		}
	}
	pisex::RemoveAllEvents();
	g_pVIPCore->VIP_OnClientLoadedUnregister();
	g_pVIPCore->VIP_OnClientDisconnectUnregister();
	g_pVIPCore->VIP_OnPlayerSpawnUnregister();
	// The vip API has no feature unregister: re-registering the feature without callbacks
	// replaces the core's entry with null delegates, so the core keeps no pointer into this
	// library after it is unloaded (same value/feature type: the menu entry is unchanged).
	g_pVIPCore->VIP_RegisterFeature(kFeature, VIP_BOOL, TOGGLABLE);
	pisex::Shutdown();
	return {};
}

bool RegenHpPlugin::Regen(int iSlot) {
	// The original required the controller's pawn, a real team and "alive".
	if (!pisex::IsAlive(iSlot) || pisex::GetPlayerTeam(iSlot) < 2) {
		return false;
	}
	// The s2sdk wrappers call their binding slots unchecked; without the engine health accessors
	// the regeneration stops instead of calling through a null pointer.
	if (!__s2sdk_GetClientMaxHealth || !__s2sdk_GetClientHealth || !__s2sdk_SetClientHealth) {
		return false;
	}
	const int iMaxHP = s2sdk::GetClientMaxHealth(iSlot);
	const int iHP = s2sdk::GetClientHealth(iSlot);
	// The healing step (clamped at m_iMaxHealth; "continue" = keep the timer) is the original's
	// RegenHP() body; heal_logic.hpp shares it with the test.
	const vip_regen_hp::HealStep step =
		vip_regen_hp::Step(iHP, iMaxHP, g_pVIPCore->VIP_GetClientFeatureInt(iSlot, kFeature));
	if (step.healed != iHP) {
		const int iPawn = pisex::GetPawnEntity(iSlot);
		s2sdk::SetClientHealth(iSlot, step.healed);
		pisex::SetStateChanged(iPawn, "CBaseEntity", "m_iHealth");
	}
	return step.continueRegen;
}

void RegenHpPlugin::CreateRegenTimer(int iSlot) {
	const float delay = SafeTimerInterval(g_pVIPCore->VIP_GetClientFeatureFloat(iSlot, kDelayFeature));
	m_timers[static_cast<std::size_t>(iSlot)] =
		pisex::CreateTimer(delay, [this, iSlot]() -> float {
			m_regen[static_cast<std::size_t>(iSlot)] = Regen(iSlot);
			if (m_regen[static_cast<std::size_t>(iSlot)]) {
				return SafeTimerInterval(g_pVIPCore->VIP_GetClientFeatureFloat(iSlot, kIntervalFeature));
			}
			m_timers[static_cast<std::size_t>(iSlot)] = nullptr;
			return -1.0f;
		});
}

void RegenHpPlugin::StopTimer(int iSlot) {
	pisex::CTimer*& timer = m_timers[static_cast<std::size_t>(iSlot)];
	if (timer != nullptr) {
		pisex::RemoveTimer(timer);
		timer = nullptr;
	}
}

void RegenHpPlugin::ResetSlot(int iSlot) {
	if (iSlot >= 0 && iSlot < kMaxSlots) {
		m_regen[static_cast<std::size_t>(iSlot)] = false;
	}
}

void RegenHpPlugin::OnPlayerHurt(const char* /*szName*/, pisex::IGameEvent* pEvent, bool /*bDontBroadcast*/) {
	// int iSlot = pEvent->GetInt("userid"); - the original's raw read. GetPlayerSlot is the
	// documented accessor (s2sdk game-events guide) and answers -1 when the key is not a player.
	const int iSlot = pEvent->GetPlayerSlot("userid");
	if (iSlot < 0 || iSlot >= kMaxSlots) {
		return;
	}
	if (!g_pVIPCore->VIP_IsClientVIP(iSlot) || pEvent->GetInt("dmg_health") == 0 || pEvent->GetInt("health") == 0) {
		return;
	}
	if (!pisex::IsAlive(iSlot) || pisex::GetPlayerTeam(iSlot) < 2) {
		return;
	}
	if (g_pVIPCore->VIP_GetClientFeatureInt(iSlot, kFeature) == -1) {
		return;
	}
	if (m_regen[static_cast<std::size_t>(iSlot)]) {
		StopTimer(iSlot);
	}
	m_regen[static_cast<std::size_t>(iSlot)] = true;
	CreateRegenTimer(iSlot);
}

} // namespace

PLUGIFY_PLUGIN(PLUGIN_API, &g_regenHpPlugin)
