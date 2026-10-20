# vip_regen_hp (Plugify)

Health regeneration for VIPs: 'RegenHP' (HP per tick), 'DelayRegenHP' (seconds after taking damage before regeneration starts) and 'IntervalRegenHP' (seconds between ticks) come from groups.ini. A player_hurt hook starts the timer; every tick heals RegenHP until the pawn is at full health. A port of the Metamod:Source module 'VIP_RegenHP' of the Pisex cs2-vip plugin.

Plugify port (fork) of **VIP_RegenHP** by Pisex (Metamod (C++)).
Original: https://github.com/Pisex/cs2-vip-modules
The code was rewritten for Plugify / s2sdk; behaviour follows the original.

## Requirements
- [Plugify](https://github.com/untrustedmodders/plugify) with [s2sdk](https://github.com/untrustedmodders/plugify-plugin-s2sdk)
- Plugify dependencies: s2sdk, vip, hudmenu (optional)

## Installation
Download `vip_regen_hp-1.0.2.zip` from Releases and extract it into `game/csgo/`.

## Building
Requires Docker. `./build.sh` builds in the Steam Runtime 3 (sniper) SDK image; the package
is written to `build/package/vip_regen_hp`.

`plugins/vip` carry only the API headers of the plugins this one uses.

## License
GPL-3.0, see LICENSE.

