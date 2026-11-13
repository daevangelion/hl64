#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# GoldSrc oracle (docs/specs/README.md "Oracles", decision D10): installs
# Valve's Linux dedicated server (steamcmd app 90, anonymous), builds the
# HLSDK probe DLL (Makefile, hlsdk-hooks.patch, probe/), installs it as the
# `hl64probe` game directory next to `valve`, copies the player's maps into
# it, and runs every requested capture on every requested map, one HLDS
# process per map and batch. Fixtures go to $HL64_FIXTURES/goldsrc/<spec>/
# <capture>/<map>.jsonl; they derive from Valve data and are never committed.
#
# usage: test/oracle/run.sh [capture|m2|m3|live|all ...]   (default: m2)
#        m2: M2 captures; m3: M3 captures (map triggers disabled, as m2);
#        live: A-order (map triggers left live; a server of its own);
#        all: m2 m3 live. Live captures always run in a separate batch.
# env:   HL64_MAPS        maps to run (default: Uplink + Hazard Course)
#        HL64_FIXTURES    output root (default /scratch/hl64/fixtures)
#        HL64_HL_DATA     player's valve dir to copy maps from
#                         (default /scratch/hl1/valve, else ~/src/hl_uplink/uplink_data)
#        HL64_PROBE_SCALE sample-count multiplier (default 1)
#        HLDS_DIR STEAMCMD_DIR ORACLE_WORK ORACLE_BUILD  install/scratch paths
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
HLDS_DIR=${HLDS_DIR:-/scratch/hlds}
STEAMCMD_DIR=${STEAMCMD_DIR:-/scratch/steamcmd}
ORACLE_WORK=${ORACLE_WORK:-/scratch/hl64/tmp/oracle}
ORACLE_BUILD=${ORACLE_BUILD:-/scratch/hl64/builds/oracle}
HL64_FIXTURES=${HL64_FIXTURES:-/scratch/hl64/fixtures}
HLSDK_URL=https://github.com/ValveSoftware/halflife.git
HLSDK_REV=b1b5cf5892918535619b2937bb927e46cb097ba1
MAPS=${HL64_MAPS:-"hldemo1 hldemo2 hldemo3 t0a0 t0a0a t0a0b t0a0b1 t0a0b2 t0a0c t0a0d"}
CAPS=${*:-m2}
if [[ -z ${HL64_HL_DATA:-} ]]; then
	if [[ -d /scratch/hl1/valve/maps ]]; then HL64_HL_DATA=/scratch/hl1/valve
	else HL64_HL_DATA=$HOME/src/hl_uplink/uplink_data; fi
fi
export TMPDIR=$ORACLE_WORK/tmp
mkdir -p "$TMPDIR" "$ORACLE_BUILD" "$HL64_FIXTURES/goldsrc/logs"

# 1. HLDS
if [[ ! -x $HLDS_DIR/hlds_linux ]]; then
	HOME=$STEAMCMD_DIR/home "$STEAMCMD_DIR/steamcmd.sh" +force_install_dir "$HLDS_DIR" \
		+login anonymous +app_set_config 90 mod valve +app_update 90 validate +quit
fi
# HLDS needs a steamclient.so under $HOME/.steam/sdk32; keep that HOME private.
steam_home=$ORACLE_WORK/home
mkdir -p "$steam_home/.steam/sdk32"
ln -sf "$STEAMCMD_DIR/linux32/steamclient.so" "$steam_home/.steam/sdk32/steamclient.so"

# 2. HLSDK at the pinned revision, hooks applied
sdk=$ORACLE_WORK/hlsdk
[[ -d $sdk/.git ]] || git clone -q "$HLSDK_URL" "$sdk"
if ! git -C "$sdk" apply --check -R "$here/hlsdk-hooks.patch" 2>/dev/null; then
	git -C "$sdk" checkout -q "$HLSDK_REV" -- dlls
	git -C "$sdk" checkout -q "$HLSDK_REV"
	git -C "$sdk" apply "$here/hlsdk-hooks.patch"
fi
[[ $(git -C "$sdk" rev-parse HEAD) == "$HLSDK_REV" ]] || { echo "hlsdk not at $HLSDK_REV" >&2; exit 1; }
make -C "$here" -j"$(nproc)" HLSDK_DIR="$sdk" BUILD_DIR="$ORACLE_BUILD" >/dev/null

# 3. game directory
mod=$HLDS_DIR/hl64probe
mkdir -p "$mod/dlls" "$mod/maps"
cp "$ORACLE_BUILD/probe.so" "$mod/dlls/probe.so"
cat >"$mod/liblist.gam" <<EOF
game "hl64 GoldSrc probe"
gamedll "dlls/probe.dll"
gamedll_linux "dlls/probe.so"
fallback_dir "valve"
type "singleplayer_only"
EOF
for m in $MAPS; do cp -u "$HL64_HL_DATA/maps/$m.bsp" "$mod/maps/"; done
# server.cfg runs after the game DLL is loaded on every map spawn: retail
# single-player movevars (valve/spserver.cfg and engine defaults; HLDS's
# valve/server.cfg would set sv_maxspeed 270 and sv_clienttrace 3.5), then
# the captures of the batch.
write_cfg() {
	cat >"$mod/server.cfg" <<EOF
sv_maxspeed 320
sv_clienttrace 1
sv_gravity 800
sv_stepsize 18
sv_friction 4
edgefriction 2
sv_stopspeed 100
sv_accelerate 10
sv_airaccelerate 10
sv_wateraccelerate 10
sv_waterfriction 1
sv_bounce 1
sv_maxvelocity 2000
mp_footsteps 1
sv_allow_autoaim 1
probe_quit 1
probe_run $1
EOF
}

# 4. captures: batches, one server per map and batch. `all` runs m2, m3 and
# live in servers of their own, so each group's fixtures do not depend on
# the others (time, entity slots and the engine's random stream carry over
# within one server); live captures always run apart.
normal=() m3=() live=()
for c in $CAPS; do
	case $c in
	live|A-order) live+=("$c") ;;
	all) normal+=(m2); m3+=(m3); live+=(live) ;;
	*) normal+=("$c") ;;
	esac
done
fail=0
run_batch() {	# caps log-suffix
	write_cfg "$1"
	for m in $MAPS; do
		log=$HL64_FIXTURES/goldsrc/logs/$m$2.log
		echo "== $m: $1"
		( cd "$HLDS_DIR" && HOME=$steam_home LD_LIBRARY_PATH=$HLDS_DIR HL64_PROBE_OUT=$HL64_FIXTURES \
			timeout 1800 ./hlds_linux -game hl64probe -norestart -insecure +sv_lan 1 +maxplayers 1 \
			+deathmatch 0 +coop 0 +sys_ticrate 10000 +host_framerate 0.01 +map "$m" \
			</dev/null >"$log" 2>&1 ) || true
		if grep -q "probe: all captures done on $m" "$log"; then
			grep -E '^probe: .* done' "$log" | sed 's/^/   /'
		else
			echo "   FAILED (see $log)"; tail -5 "$log" | sed 's/^/   /'; fail=1
		fi
	done
}
(( ${#normal[@]} )) && run_batch "${normal[*]}" ""
(( ${#m3[@]} )) && run_batch "${m3[*]}" "-m3"
(( ${#live[@]} )) && run_batch "${live[*]}" "-live"
exit $fail
