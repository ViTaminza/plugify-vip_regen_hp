// vip_regen_hp — Plugify port of the Pisex VIP module "VIP_RegenHP"
// (the original's vip_regen_hp.cpp).
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
//   * loaded/disconnect/spawn reset the regeneration state of the slot.
//
// Pure s2sdk (docs/PLUGIN_STANDARD.md §7): there is no pisexcompat and no s2guard. Every safety
// rule the plugin needs lives in this file and in regen_hp_core.hpp:
//   * PITFALLS B1/B2 - the map gate g_mapReady. The OnMapStart/OnMapEnd listeners are registered
//     in OnPluginStart, readiness is latched by OnMapStart and by the first OnPluginUpdate of a
//     plugin loaded into a running map (when no OnMapEnd was seen since OnPluginStart), and
//     dropped by OnMapEnd, which also cancels every per-slot regeneration countdown. No client,
//     entity or schema call is made while the gate is closed: s2sdk dereferences a torn-down
//     client list in the OnMapEnd..OnMapStart window and a null serializer map before the first
//     map.
//   * PITFALLS C12/C13 - "player_hurt" is hooked with s2sdk::HookEvent(HookMode::Pre) in
//     OnPluginStart and unhooked in OnPluginEnd; the handler answers Continue on every path and
//     reads the fields with GetEventPlayerSlot/GetEventInt when the event arrives (nothing about
//     an entity is cached across a changelevel).
//   * PITFALLS C2 - IsAlive() layers the gate, the slot range, IsClientConnected, IsClientInGame,
//     !IsClientSourceTV, a non-null controller pointer and IsClientAlive. GetPlayerTeam() reads
//     the controller's m_iTeamNum (1 byte) at a map-gated, cached schema offset, exactly like
//     pisexcompat's GetPlayerTeam did.
//   * PITFALLS C8 - the schema offset is resolved only with a live map and only a successful
//     lookup is cached.
//   * PITFALLS C6 - m_iHealth is marked changed through s2sdk::NetworkStateChanged on the pawn's
//     entity handle, resolved from the pawn pointer GetClientPawn answers in the same call (no
//     raw entity pointer is kept anywhere).
//   * PITFALLS C9/B4 - the original's CTimer per slot becomes a per-slot countdown driven by
//     OnPluginUpdate (regen_hp_core.hpp). The delay and every later interval are clamped to
//     >= 1/64 s, and OnMapEnd/OnClientDisconnect/OnPluginEnd clear the countdowns.
//
// Port notes:
//   * the original's health/max-health field read and write -> s2sdk::GetClientHealth /
//     GetClientMaxHealth / SetClientHealth, followed by the network state change the original's
//     SetStateChanged() made, so the clients see the healed health immediately.
//   * the slot resets stop the pending regeneration as well (the original only cleared the flag),
//     and a hurt stops the previous timer unconditionally before starting a new one: the flag can
//     already be false while a timer is still alive (a spawn/load/disconnect reset or the toggle
//     callback clears the flag), and the original then overwrote the live handle - two timers
//     healing the same slot, and the first one to stop cleared the handle of the other.
//   * the optional per-group "RegenHP_round_min" key (round_rule.hpp) gates the module by round:
//     missing/negative behaves like 1 (non-pistol rounds from one completed round on), 0 allows
//     every round and >=2 keeps the vip_bf/items convention. The current round is re-checked both
//     by the per-frame pump (which disarms an armed countdown in a denied round) and at the write/
//     arm itself; a denied round writes nothing, schedules nothing and owes nothing later.
//   * Regen() stops on a non-positive feature value: the core answers -1 once there is no VIP
//     record or the feature is gone (expiry, removal, group change, reload, the menu toggle) and
//     0 for a group value of "0", and the original's Step() arithmetic turned the -1 into one
//     lost HP per run until death.
//   * the "player_hurt" handler resolves the victim's slot with GetEventPlayerSlot("userid"), the
//     accessor the s2sdk game-events guide prescribes. The original read the raw "userid" field,
//     i.e. the engine user id, which for a human player equals the slot, so the original worked
//     for the VIP players it serves; GetPlayerSlot is correct for every player.
//   * OnPluginStart never returns Failed (PITFALLS A1: that stops every Plugify plugin) and
//     OnPluginEnd removes every hook/listener/timer it registered, so `plg reload` is safe.

#include <plg/plugin.hpp>
#include <plg/string.hpp>
#include <plugin_export.h>
#include <s2sdk/s2sdk.hpp>

#include <vip_api.hpp>

#include "regen_hp_core.hpp"
#include "round_rule.hpp"

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
namespace rhp = vip_regen_hp;

IVIPApi* g_pVIPCore = &IVIPApi::Get();

constexpr const char* kFeature = "RegenHP";
constexpr const char* kDelayFeature = "DelayRegenHP";
constexpr const char* kIntervalFeature = "IntervalRegenHP";
// The optional per-group round gate in the existing vip/groups.ini (see round_rule.hpp). It is read
// with the existing VIP_GetClientFeatureInt, so a group without the key answers -1, which the policy
// treats exactly like 1.
constexpr const char* kRoundMinFeature = "RegenHP_round_min";

// --- PITFALLS B1/B2: the map gate --------------------------------------------------------------

bool g_mapReady = false;       // a map is up: s2sdk client/entity/schema calls are safe
bool g_mapStartSeen = false;   // OnMapStart arrived since start / the last OnMapEnd
bool g_mapEndSeen = false;     // OnMapEnd arrived since start
bool g_mapListeners = false;   // OnMapStart/OnMapEnd are registered with s2sdk

// --- PITFALLS C12/C13: the game-event hook -----------------------------------------------------

bool g_playerHurtHooked = false; // "player_hurt" is hooked (and must be unhooked on unload)

void LogMessage(std::string_view message, plg::Severity severity) {
	if (plg::Log) {
		plg::Log(message, severity, plg::source_location::current());
	}
}

void LogInfo(std::string_view message) {
	LogMessage(message, plg::Severity::Info);
}

void LogError(std::string_view message) {
	LogMessage(message, plg::Severity::Error);
}

// PITFALLS C8: the s2sdk export must only run with a live map, and a failed lookup must not be
// cached (the caller keeps asking until the class is available).
int SchemaOffset(const char* className, const char* fieldName) {
	if (!__s2sdk_GetSchemaOffset) {
		return -1;
	}
	try {
		return s2sdk::GetSchemaOffset(className, fieldName);
	} catch (...) {
		return -1;
	}
}

// The mid-map latch of the gate: a plugin loaded into a running map never saw OnMapStart, so its
// first update is the "a map is up" signal (the host pumps updates from the server frame). After
// an OnMapEnd only a new OnMapStart re-opens the gate: the host keeps pumping updates through a
// changelevel, so `!g_mapEndSeen` alone must not be enough once one was seen.
void PollMapGate() {
	if (!g_mapListeners) {
		return;
	}
	if (!g_mapReady && (g_mapStartSeen || !g_mapEndSeen)) {
		g_mapReady = true;
	}
}

// --- PITFALLS C2: the client checks ------------------------------------------------------------

// Alive means every layer s2sdk does not check itself: a live map, a slot in range, a connected
// client in game that is not the SourceTV proxy, a controller entity, and the engine's alive flag
// (which needs the pawn). s2sdk::IsClientAlive dereferences the pawn, so it is asked last.
bool IsAlive(int iSlot) {
	if (!g_mapReady || !rhp::IsSlotInRange(iSlot)) {
		return false;
	}
	if (!__s2sdk_IsClientConnected || !__s2sdk_IsClientInGame || !__s2sdk_IsClientSourceTV ||
		!__s2sdk_PlayerSlotToEntPointer || !__s2sdk_IsClientAlive) {
		return false;
	}
	try {
		return s2sdk::IsClientConnected(iSlot) && s2sdk::IsClientInGame(iSlot) &&
			   !s2sdk::IsClientSourceTV(iSlot) &&
			   s2sdk::PlayerSlotToEntPointer(iSlot) != nullptr && s2sdk::IsClientAlive(iSlot);
	} catch (...) {
		return false;
	}
}

// The team lives on the controller (CBaseEntity::m_iTeamNum, 1 byte at the schema offset), what
// pisexcompat's GetPlayerTeam read: s2sdk's GetClientTeam goes through the pawn and answered 0 -
// with a console warning - for every client without a current pawn. The offset is resolved only
// with a live map (PITFALLS C8) and only a successful lookup is cached.
int GetPlayerTeam(int iSlot) {
	if (!g_mapReady || !rhp::IsSlotInRange(iSlot)) {
		return 0;
	}
	static int s_teamOffset = -1;
	if (s_teamOffset < 0) {
		if (!__s2sdk_GetSchemaOffset || !__s2sdk_PlayerSlotToEntPointer || !__s2sdk_GetEntData2) {
			return 0;
		}
		s_teamOffset = SchemaOffset("CBaseEntity", "m_iTeamNum");
	}
	if (s_teamOffset < 0 || !__s2sdk_PlayerSlotToEntPointer || !__s2sdk_GetEntData2) {
		return 0;
	}
	try {
		void* controller = s2sdk::PlayerSlotToEntPointer(iSlot);
		return controller != nullptr
				   ? static_cast<int>(s2sdk::GetEntData2(controller, s_teamOffset, 1) & 0xFF)
				   : 0;
	} catch (...) {
		return 0;
	}
}

// The SetClientHealth write above only changes the server-side field; the original's
// SetStateChanged(iPawn, "CBaseEntity", "m_iHealth") told the engine to send it to the clients.
// The pawn handle is resolved from the pawn pointer GetClientPawn answers in this same call and
// validated before s2sdk's export dereferences it (PITFALLS C1/C6): an entity int is a
// CEntityHandle (index | serial << 15), -1 means invalid and 0 the world.
//
// s2sdk 3.0's NetworkStateChanged(handle, className, memberName) resolves the field offset and the
// class chain offset itself (export/schema.cpp: NetworkStateChanged -> NetworkStateChanged2 ->
// SafeNetworkStateChanged with FindChainOffset), so the chain offset pisexcompat passed as 0 does
// not exist as a parameter of the s2sdk export.
void MarkHealthChanged(int iSlot) {
	if (!g_mapReady || !rhp::IsSlotInRange(iSlot)) {
		return;
	}
	if (!__s2sdk_GetClientPawn || !__s2sdk_EntPointerToEntHandle || !__s2sdk_IsValidEntPointer ||
		!__s2sdk_IsValidEntHandle || !__s2sdk_NetworkStateChanged) {
		return;
	}
	if (!IsAlive(iSlot)) {
		return;
	}
	try {
		void* pawn = s2sdk::GetClientPawn(iSlot);
		if (pawn == nullptr || !s2sdk::IsValidEntPointer(pawn)) {
			return;
		}
		const int32_t pawnHandle = s2sdk::EntPointerToEntHandle(pawn);
		if (rhp::IsUsableEntityHandle(pawnHandle) && s2sdk::IsValidEntHandle(pawnHandle)) {
			s2sdk::NetworkStateChanged(pawnHandle, "CBaseEntity", "m_iHealth");
		}
	} catch (...) {
	}
}

class RegenHpPlugin final : public plg::Plugin {
public:
	plg::PluginResult OnPluginStart() override;
	plg::PluginResult OnPluginUpdate(std::chrono::milliseconds dt) override;
	plg::PluginResult OnPluginEnd() override;

	// The "player_hurt" handler body (the event callback is a plain function pointer that cannot
	// be a member function, so it forwards here).
	void OnPlayerHurt(void* event);

	// s2sdk map listeners are plain function pointers called on the game thread: they must not
	// throw and must not touch a client (OnMapEnd runs while the client list is torn down).
	static void OnMapStartCallback();
	static void OnMapEndCallback();

private:
	// RegenHP() of the original: one healing step; true = healed (keep the countdown alive).
	bool Regen(int iSlot);
	// Arm the countdown with the group's DelayRegenHP (clamped to the tick floor).
	void StartRegenTimer(int iSlot);
	void StopTimer(int iSlot);
	void ResetSlot(int iSlot);
	void StopAllTimers();

	// --- the optional "RegenHP_round_min" gate (round_rule.hpp) ----------------------------------

	// The current round, sampled once per frame (never once per slot): the round accessors are
	// cross-plugin calls into the vip core.
	rhp::RoundState SampleRoundState();
	// Lazy cached per-slot read of the group's RegenHP_round_min.
	int RoundMin(int iSlot);
	void RefreshRoundMin(int iSlot);
	void InvalidateRoundMin(int iSlot);
	void InvalidateAllRoundMin();
	// The write/arm gate: a fresh threshold read (a group change or vip_reload may have changed it
	// since the timer was armed) against the current round.
	bool RoundAllowsWrite(int iSlot);

	// The round_min of a slot is constant until the group changes, and the per-frame gate must not
	// call into the vip core for all 64 slots: it is cached here. The cache is dropped at every
	// reset (load, spawn, disconnect, map change), on the toggle, and re-read by the write/arm path;
	// the one-second invalidation bounds the staleness of the changes no callback reports (a group
	// swap, vip_reload).
	std::array<int, rhp::kMaxSlots> m_roundMinCache{};
	std::array<bool, rhp::kMaxSlots> m_roundMinKnown{};
	float m_roundMinCacheAge = 0.0f;

	std::array<rhp::RegenTimer, rhp::kMaxSlots> m_timers{};
};

RegenHpPlugin g_regenHpPlugin;

void RegenHpPlugin::OnMapStartCallback() {
	// The map's first live frame: clients, entities and the serializer are in place.
	g_mapReady = true;
	g_mapStartSeen = true;
	g_mapEndSeen = false;
	// Per-map state: the cached thresholds belong to the previous map's groups.
	g_regenHpPlugin.InvalidateAllRoundMin();
}

void RegenHpPlugin::OnMapEndCallback() {
	// PITFALLS B2: from here until the next OnMapStart every s2sdk client query can crash (the
	// client list is being torn down), so this handler only drops state. Every pending
	// regeneration is cancelled as well (PITFALLS B4): a countdown must not survive into the next
	// map and heal a slot that belongs to somebody else by then.
	g_mapReady = false;
	g_mapStartSeen = false;
	g_mapEndSeen = true;
	g_regenHpPlugin.StopAllTimers();
}

// The single "player_hurt" hook. A Pre hook must answer Continue on every path: a result >=
// Handled frees the event for every other plugin on the server (the original's listener returned
// false for the same reason).
s2sdk::ResultType OnPlayerHurtEvent(const plg::string& /*name*/, void* event, bool /*dontBroadcast*/) {
	try {
		g_regenHpPlugin.OnPlayerHurt(event);
	} catch (const std::exception& ex) {
		LogError(std::string("vip_regen_hp: player_hurt failed: ") + ex.what());
	} catch (...) {
		LogError("vip_regen_hp: player_hurt failed with an unknown exception");
	}
	return s2sdk::ResultType::Continue;
}

bool RegenHpPlugin::Regen(int iSlot) {
	// The original required the controller's pawn, a real team and "alive".
	if (!IsAlive(iSlot) || GetPlayerTeam(iSlot) < 2) {
		return false;
	}
	// The write itself re-checks the current round and re-reads the group's RegenHP_round_min: a
	// countdown armed in an earlier round (or under a previous group) must not heal in a denied
	// round. A false answer disarms the countdown in the caller, so nothing is ever owed later.
	if (!RoundAllowsWrite(iSlot)) {
		return false;
	}
	// The s2sdk wrappers call their binding slots unchecked; without the engine health accessors
	// the regeneration stops instead of calling through a null pointer.
	if (!__s2sdk_GetClientMaxHealth || !__s2sdk_GetClientHealth || !__s2sdk_SetClientHealth) {
		return false;
	}
	const int iMaxHP = s2sdk::GetClientMaxHealth(iSlot);
	const int iHP = s2sdk::GetClientHealth(iSlot);
	// The countdown can outlive the VIP record (the expiry loop, VIP_RemoveClientVIP, a group
	// change, a core reload, a menu toggle): the core then answers -1 (or 0 for a group value of
	// "0"). The original's RegenHP() arithmetic would turn that into a negative step and drain one
	// HP per run until death, so a non-positive amount stops the regeneration.
	const int iAmount = g_pVIPCore->VIP_GetClientFeatureInt(iSlot, kFeature);
	if (!rhp::HasRegenAmount(iAmount)) {
		return false;
	}
	// The healing step (clamped at max health; "continue" = keep the countdown) is the original's
	// RegenHP() body; regen_hp_core.hpp shares it with the test.
	const rhp::HealStep step = rhp::Step(iHP, iMaxHP, iAmount);
	if (step.healed != iHP) {
		s2sdk::SetClientHealth(iSlot, step.healed);
		MarkHealthChanged(iSlot);
	}
	return step.continueRegen;
}

void RegenHpPlugin::StartRegenTimer(int iSlot) {
	m_timers[static_cast<std::size_t>(iSlot)].Arm(
		g_pVIPCore->VIP_GetClientFeatureFloat(iSlot, kDelayFeature));
}

void RegenHpPlugin::StopTimer(int iSlot) {
	if (rhp::IsSlotInRange(iSlot)) {
		m_timers[static_cast<std::size_t>(iSlot)].Disarm();
	}
}

void RegenHpPlugin::ResetSlot(int iSlot) {
	if (rhp::IsSlotInRange(iSlot)) {
		// Cancel the pending countdown as well: a disconnect/load/spawn must not leave a
		// regeneration armed for the next occupant of the slot (or for a reloaded record).
		StopTimer(iSlot);
		// The next occupant/record may carry a different group: drop the cached round_min too.
		InvalidateRoundMin(iSlot);
	}
}

void RegenHpPlugin::StopAllTimers() {
	for (rhp::RegenTimer& timer : m_timers) {
		timer.Disarm();
	}
	// The per-map threshold cache dies with the timers (a group may differ on the next map).
	InvalidateAllRoundMin();
}

// --- the optional round gate -------------------------------------------------------------------

rhp::RoundState RegenHpPlugin::SampleRoundState() {
	rhp::RoundState round;
	// The vip_api wrappers only check VIP_IsVIPLoaded and then call the binding unchecked: both round
	// exports are checked here (plus the availability gate the wrappers use), so an older core can
	// never be reached through a null pointer and an unavailable round API never enables the effect
	// (round_rule.hpp).
	if (!g_mapReady || !__vip_VIP_IsVIPLoaded || !__vip_VIP_PistolRound ||
		!__vip_VIP_GetTotalRounds) {
		return round;
	}
	try {
		if (!g_pVIPCore->VIP_IsVIPLoaded()) {
			return round;
		}
		round.pistolRound = g_pVIPCore->VIP_PistolRound();
		round.totalRounds = g_pVIPCore->VIP_GetTotalRounds();
		round.available = true;
	} catch (...) {
		round.available = false;
	}
	return round;
}

int RegenHpPlugin::RoundMin(int iSlot) {
	if (!rhp::IsSlotInRange(iSlot)) {
		return rhp::kDefaultRoundMin;
	}
	const std::size_t index = static_cast<std::size_t>(iSlot);
	if (!m_roundMinKnown[index]) {
		RefreshRoundMin(iSlot);
	}
	// A refresh refused by the closed map gate leaves the slot unknown: fall back to the strict
	// default instead of trusting a stale value.
	return m_roundMinKnown[index] ? m_roundMinCache[index] : rhp::kDefaultRoundMin;
}

void RegenHpPlugin::RefreshRoundMin(int iSlot) {
	// The vip core query is not an engine call, but it is still kept behind the same gate so the
	// cache is never filled with answers from outside a map.
	if (!g_mapReady || !rhp::IsSlotInRange(iSlot)) {
		return;
	}
	const std::size_t index = static_cast<std::size_t>(iSlot);
	m_roundMinCache[index] = g_pVIPCore->VIP_GetClientFeatureInt(iSlot, kRoundMinFeature);
	m_roundMinKnown[index] = true;
}

void RegenHpPlugin::InvalidateRoundMin(int iSlot) {
	if (rhp::IsSlotInRange(iSlot)) {
		m_roundMinKnown[static_cast<std::size_t>(iSlot)] = false;
	}
}

void RegenHpPlugin::InvalidateAllRoundMin() {
	m_roundMinKnown.fill(false);
}

bool RegenHpPlugin::RoundAllowsWrite(int iSlot) {
	// Re-read the group's round_min right before a write/arm: a group swap or a vip_reload can have
	// changed it since the countdown was armed.
	RefreshRoundMin(iSlot);
	return rhp::RoundAllowed(RoundMin(iSlot), SampleRoundState());
}

void RegenHpPlugin::OnPlayerHurt(void* event) {
	// The event system keeps firing during a changelevel (PITFALLS C13); the gate turns those into
	// a no-op before any client or entity is looked up.
	if (event == nullptr || !g_mapReady) {
		return;
	}
	if (!__s2sdk_GetEventPlayerSlot || !__s2sdk_GetEventInt) {
		return;
	}
	const int iSlot = s2sdk::GetEventPlayerSlot(event, "userid");
	if (!rhp::IsSlotInRange(iSlot)) {
		return;
	}
	if (!g_pVIPCore->VIP_IsClientVIP(iSlot) || s2sdk::GetEventInt(event, "dmg_health") == 0 ||
		s2sdk::GetEventInt(event, "health") == 0) {
		return;
	}
	if (!IsAlive(iSlot) || GetPlayerTeam(iSlot) < 2) {
		return;
	}
	if (g_pVIPCore->VIP_GetClientFeatureInt(iSlot, kFeature) == -1) {
		return;
	}
	// The previous countdown is stopped unconditionally (see the file header): the original could
	// leave two timers healing the same slot when the flag was already false.
	StopTimer(iSlot);
	// Scheduling is gated too: the damage may arrive while the current round forbids the effect (or
	// a moment before the transition to the second-half pistol). The fresh read also refreshes the
	// cached threshold, and the StopTimer above already dropped a countdown left from another round.
	if (!RoundAllowsWrite(iSlot)) {
		return;
	}
	StartRegenTimer(iSlot);
}

plg::PluginResult RegenHpPlugin::OnPluginStart() {
	try {
		g_mapReady = false;
		g_mapStartSeen = false;
		g_mapEndSeen = false;
		StopAllTimers();

		// PITFALLS B1: registering the map listeners touches no client, so it is safe here.
		// Without them nothing could close the gate again, so a missing binding keeps it shut
		// (every client call stays refused) instead of opening it blindly.
		if (!g_mapListeners && __s2sdk_OnMapStart_Register && __s2sdk_OnMapEnd_Register) {
			s2sdk::OnMapStart_Register(&RegenHpPlugin::OnMapStartCallback);
			s2sdk::OnMapEnd_Register(&RegenHpPlugin::OnMapEndCallback);
			g_mapListeners = true;
		} else if (!__s2sdk_OnMapStart_Register || !__s2sdk_OnMapEnd_Register) {
			LogError("vip_regen_hp: the s2sdk map listeners are unavailable - client calls stay disabled");
		}

		g_pVIPCore->VIP_RegisterFeature(kFeature, VIP_BOOL, TOGGLABLE, nullptr,
										[this](int iSlot, const char* /*szFeature*/,
											   VIP_ToggleState /*eOldStatus*/, VIP_ToggleState& eNewStatus) {
											if (eNewStatus == DISABLED && rhp::IsSlotInRange(iSlot)) {
												StopTimer(iSlot);
											}
											// Re-read the group's round_min on the next use (the toggle
											// may coincide with a group/feature change).
											InvalidateRoundMin(iSlot);
											return true; // the original blocked the toggle
										},
										[](int iSlot, const char* szFeature) {
											char buffer[128];
											std::snprintf(buffer, sizeof(buffer), "%s [%i HP/%i s.]",
														  g_pVIPCore->VIP_GetTranslate(szFeature),
														  g_pVIPCore->VIP_GetClientFeatureInt(iSlot, kFeature),
														  g_pVIPCore->VIP_GetClientFeatureInt(iSlot, kIntervalFeature));
											return std::string(buffer);
										});
		g_pVIPCore->VIP_RegisterFeature(kDelayFeature, VIP_INT, HIDE);
		g_pVIPCore->VIP_RegisterFeature(kIntervalFeature, VIP_INT, HIDE);

		// PITFALLS C12/C13: one Pre hook on "player_hurt"; s2sdk only accepts plain function
		// pointers, and the callback above answers Continue on every path.
		if (!g_playerHurtHooked && __s2sdk_HookEvent) {
			if (s2sdk::HookEvent("player_hurt", &OnPlayerHurtEvent, s2sdk::HookMode::Pre) ==
				s2sdk::EventHookError::Okay) {
				g_playerHurtHooked = true;
			} else {
				LogError("vip_regen_hp: hooking 'player_hurt' failed - regeneration is disabled");
			}
		} else if (!__s2sdk_HookEvent) {
			LogError("vip_regen_hp: the s2sdk event bindings are unavailable - 'player_hurt' is not hooked");
		}

		// Loaded/spawn/disconnect reset the slot. The core dispatches VIP_OnClientDisconnect for
		// every disconnecting slot (VIP or not), from its own s2sdk OnClientDisconnect listener,
		// so a pending countdown never survives into the next occupant of the slot.
		g_pVIPCore->VIP_OnClientLoaded([this](int iSlot, bool /*bIsVIP*/) { ResetSlot(iSlot); });
		g_pVIPCore->VIP_OnClientDisconnect([this](int iSlot, bool /*bIsVIP*/) { ResetSlot(iSlot); });
		g_pVIPCore->VIP_OnPlayerSpawn(
			[this](int iSlot, int /*iTeam*/, bool /*bIsVIP*/) { ResetSlot(iSlot); });

		LogInfo("vip_regen_hp: ready");
	} catch (const std::exception& ex) {
		LogError(std::string("vip_regen_hp: OnPluginStart failed: ") + ex.what());
	} catch (...) {
		LogError("vip_regen_hp: OnPluginStart failed with an unknown exception");
	}
	return {}; // never Failed (PITFALLS A1)
}

plg::PluginResult RegenHpPlugin::OnPluginUpdate(std::chrono::milliseconds dt) {
	try {
		PollMapGate();
		if (!g_mapReady) {
			return {};
		}
		const float fSeconds = static_cast<float>(dt.count()) / 1000.0f;
		if (!(fSeconds > 0.0f)) {
			return {};
		}
		// The current round is sampled once per frame, not once per slot: the round accessors are
		// cross-plugin calls into the vip core.
		const rhp::RoundState round = SampleRoundState();
		// Bound the staleness of the cached per-slot thresholds (a group swap or vip_reload reports
		// nothing): invalidate them once a second, the armed slots re-read them on the next frame.
		m_roundMinCacheAge += fSeconds;
		if (m_roundMinCacheAge >= 1.0f) {
			m_roundMinCacheAge = 0.0f;
			InvalidateAllRoundMin();
		}
		for (int iSlot = 0; iSlot < rhp::kMaxSlots; ++iSlot) {
			rhp::RegenTimer& timer = m_timers[static_cast<std::size_t>(iSlot)];
			if (!timer.Active()) {
				continue;
			}
			// The current round re-gates every armed countdown: a denied round disarms it here, so
			// the due step never fires (no write and no debt to catch up on later).
			if (!rhp::RoundAllowed(RoundMin(iSlot), round)) {
				StopTimer(iSlot);
				continue;
			}
			if (!timer.Advance(fSeconds)) {
				continue;
			}
			// Due: heal once; a successful step (the original's callback returning a delay) keeps
			// the countdown alive with IntervalRegenHP, any other answer stops it.
			if (Regen(iSlot)) {
				timer.Reschedule(g_pVIPCore->VIP_GetClientFeatureFloat(iSlot, kIntervalFeature));
			} else {
				StopTimer(iSlot);
			}
		}
	} catch (const std::exception& ex) {
		LogError(std::string("vip_regen_hp: OnPluginUpdate failed: ") + ex.what());
	} catch (...) {
		LogError("vip_regen_hp: OnPluginUpdate failed with an unknown exception");
	}
	return {};
}

plg::PluginResult RegenHpPlugin::OnPluginEnd() {
	try {
		StopAllTimers();

		// PITFALLS A7/C12: every registration of this library is removed again, so a `plg reload`
		// leaves nothing that points into the unloaded .so.
		if (g_playerHurtHooked) {
			g_playerHurtHooked = false;
			if (__s2sdk_UnhookEvent) {
				s2sdk::UnhookEvent("player_hurt", &OnPlayerHurtEvent, s2sdk::HookMode::Pre);
			}
		}
		if (g_mapListeners) {
			g_mapListeners = false;
			if (__s2sdk_OnMapStart_Unregister) {
				s2sdk::OnMapStart_Unregister(&RegenHpPlugin::OnMapStartCallback);
			}
			if (__s2sdk_OnMapEnd_Unregister) {
				s2sdk::OnMapEnd_Unregister(&RegenHpPlugin::OnMapEndCallback);
			}
		}

		g_pVIPCore->VIP_OnClientLoadedUnregister();
		g_pVIPCore->VIP_OnClientDisconnectUnregister();
		g_pVIPCore->VIP_OnPlayerSpawnUnregister();
		// The vip API has no feature unregister: re-registering the feature without callbacks
		// replaces the core's entry with null delegates, so the core keeps no pointer into this
		// library after it is unloaded (same value/feature type: the menu entry is unchanged).
		g_pVIPCore->VIP_RegisterFeature(kFeature, VIP_BOOL, TOGGLABLE);

		// A stale callback (the engine may still hold a pointer for a moment) can no longer reach
		// a client: the gate stays closed and the countdowns are gone.
		g_mapReady = false;
		g_mapStartSeen = false;
		g_mapEndSeen = true;
	} catch (const std::exception& ex) {
		LogError(std::string("vip_regen_hp: OnPluginEnd failed: ") + ex.what());
	} catch (...) {
		LogError("vip_regen_hp: OnPluginEnd failed with an unknown exception");
	}
	return {};
}

} // namespace

PLUGIFY_PLUGIN(PLUGIN_API, &g_regenHpPlugin)
