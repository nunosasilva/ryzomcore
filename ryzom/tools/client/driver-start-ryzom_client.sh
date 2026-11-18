#!/usr/bin/env bash
#
# driver-start-ryzom_client.sh - start a Ryzom client for automated tests.
#
# Run it from the root of a client installation (the directory that holds
# client.cfg, client.log and data/):
#
#     DRIVER=Vulkan ACCOUNT=username321 PASSWORD=password123 SLOT=0 \
#         ./driver-start-ryzom_client.sh
#
# It optionally rewrites client.cfg, starts the client in a virtual screen
# (gamescope by default), waits until the client is in game, keeps the personal
# light off through the development hook RYZOM_FORCE_LIGHT_OFF=1 (no fragile
# keyboard toggle), optionally takes a screenshot, then prints the DISPLAY the
# client is running on and exits 0, leaving everything running.
#
# Clients are tracked in a per-account/slot state file.  The harness only ever
# stops a pid recorded there *and* verified through /proc (exact executable and
# the RYZOM_HARNESS_STATE environment variable it passed at launch), so it never
# kills another agent's client, an installed final/opt build, or a --profile 0
# client.  There is no "kill everything named ryzom_client_dev" step.
#
# ---------------------------------------------------------------------------
# Parameters
# ---------------------------------------------------------------------------
#
# All parameters are KEY=VALUE, given as arguments or in the environment.
#
#   DRIVER=Auto|OpenGL|OpenGL3|Vulkan|DirectX
#                          Rewrite Driver3D in client.cfg (both occurrences).
#                          Empty or "keep" leaves the file untouched; the
#                          original is saved once as client.cfg.harness-backup.
#   SCREEN=gamescope|gamescope-headless|xvfb|none
#                          Virtual screen to run in (default: gamescope).
#                          "none" uses the current DISPLAY.
#   HEADLESS=1             Shorthand for SCREEN=gamescope-headless.
#   CLIENT=./ryzom_client_vulkan_opt
#                          Client executable (absolute or relative to cwd).
#   ACCOUNT, PASSWORD, SLOT
#                          Login parameters. Empty ACCOUNT shows the login
#                          screen (WAIT_FOR then defaults to "window").
#   RESOLUTION=1280x800    Screen size (the UI needs at least 1024x800).
#   WAIT_FOR=ingame|login|window
#                          What counts as started (default: ingame).
#   LOG=./client.log       Client log to watch.  Rotated logs (clientNNN.log)
#                          are considered when looking for the in-game banner.
#   TIMEOUT=180            Seconds to wait for WAIT_FOR before giving up.
#   RETRIES=3              Attempts when the account is already logged in.
#   RETRY_DELAY=40         Seconds between attempts.
#   EXTRA_ARGS="--nopatch 1"
#                          Extra client arguments (whitespace separated).
#   ENV="VAR=value ..."    Extra environment for the client.  The same values
#                          can be given after a literal -- argument:
#                              ./driver-start-ryzom_client.sh ACCOUNT=u -- \
#                                  RYZOM_FORCE_LIGHT_HOUR=12
#                          Values cannot contain spaces.  DISPLAY and the
#                          RYZOM_HARNESS_* variables are reserved.
#   LIGHT=off|on|toggle    Personal light handling after reaching the game
#                          (default: off).  "off" adds RYZOM_FORCE_LIGHT_OFF=1
#                          to the client environment: a development hook in
#                          user_entity.cpp that makes light()/lightOn() stay
#                          off for the whole session, so no keyboard input is
#                          needed.  "on" does not add the hook and leaves the
#                          game default (a fresh character starts lit).
#                          "toggle" also omits the hook and presses 'l' once as
#                          the old keyboard fallback (fragile: needs window
#                          focus, working xdotool and no dialog in front).
#   NOLIGHT=1              Leave the personal light alone: do not add the hook
#                          and do not press 'l' (a fresh character stays lit).
#   CAPTURE=<dir>          After reaching the game, take one in-game screenshot
#                          (Ctrl+F10) and copy it to <dir>.  Prints CAPTURE=.
#   CAPTURE_SRC=./screenshots
#                          Where the client writes its screenshots.
#   CAPTURE_TIMEOUT=15     Seconds to wait for a screenshot after Ctrl+F10.
#   SETTLE=2               Seconds to wait after reaching the game before the
#                          screenshot.
#   GAMESCOPE_BACKEND=wayland|x11
#                          gamescope backend (default: wayland when
#                          WAYLAND_DISPLAY is set, otherwise x11).
#   HARNESS_DIR=./tmp/harness
#                          Where gamescope/Xvfb logs, state files and the
#                          pipeline cache go.  Never uses /tmp by default.
#   HARNESS_KEY=<name>     Override the state file key (default:
#                          <account>-slot<slot>, or login-slotany without an
#                          account).  Use it to run two harness clients from
#                          the same installation.
#   STATE_FILE=<path>      Override the whole state file path.
#
# Modes (they do not start a client):
#
#   REUSE=1                Reuse the client recorded in the state file if it is
#                          still alive, is the same executable, and has the
#                          requested ENV.  Otherwise exit 6 so a caller can
#                          start one.
#   STOP=1                 Stop the client (and its screen) recorded in the
#                          state file, then exit 0.  Safe to run twice.
#   DRYRUN=1               Print the resolved command lines and the state file
#                          that would be used; change nothing.
#
# ---------------------------------------------------------------------------
# Output and exit codes
# ---------------------------------------------------------------------------
#
# On success the script prints these KEY=VALUE lines on stdout and exits 0:
#
#   DISPLAY=:3
#   CLIENT_PID=12345
#   LOG=/path/to/client.log
#   LIGHT=off|on|unknown|untouched
#   CAPTURE=/path/to/capture.jpg        (only with CAPTURE=)
#
# Exit codes: 0 started/stopped, 2 usage/config error, 3 timeout, 4 client
# died, 5 no attempt left because the account stayed busy, 6 REUSE=1 found no
# matching tracked client.
#
# The client is left running on exit 0 (including after a failed light press or
# capture); use STOP=1 to stop it later.
#
# Examples:
#   # Login screen, OpenGL, 1280x800
#   DRIVER=OpenGL SCREEN=gamescope ./driver-start-ryzom_client.sh
#
#   # In game with a specific account, Vulkan, then use $DISPLAY with xdotool
#   eval "$(DRIVER=Vulkan ACCOUNT=username321 PASSWORD=password123 SLOT=0 \
#            ./driver-start-ryzom_client.sh)"
#
#   # Headless shadow run: personal light off, stable noon sun, one capture
#   eval "$(HEADLESS=1 DRIVER=Vulkan ACCOUNT=username321 PASSWORD=password123 \
#            SLOT=0 ENV='RYZOM_FORCE_LIGHT_HOUR=12' \
#            CAPTURE=./captures ./driver-start-ryzom_client.sh)"
#
#   # Reuse the tracked client, then stop it
#   eval "$(REUSE=1 ACCOUNT=username321 SLOT=0 ./driver-start-ryzom_client.sh)"
#   STOP=1 ACCOUNT=username321 SLOT=0 ./driver-start-ryzom_client.sh
#
#   # Show what would be run, without touching anything
#   DRYRUN=1 HEADLESS=1 ACCOUNT=username321 SLOT=0 \
#       ./driver-start-ryzom_client.sh
#
set -uo pipefail

PROG=${0##*/}

die()  { echo "$PROG: $*" >&2; exit 2; }
info() { echo "$PROG: $*" >&2; }
warn() { echo "$PROG: warning: $*" >&2; }

usage() {
	awk 'NR > 1 && /^#/ { sub(/^# ?/, ""); print; next }
	     NR > 1 && !/^#/ { exit }' "$0"
	exit 0
}

# ---------------------------------------------------------------------------
# Parameters
# ---------------------------------------------------------------------------

DRIVER=${DRIVER:-}
SCREEN=${SCREEN:-gamescope}
HEADLESS=${HEADLESS:-0}
CLIENT=${CLIENT:-./ryzom_client_vulkan_opt}
ACCOUNT=${ACCOUNT:-}
PASSWORD=${PASSWORD:-}
SLOT=${SLOT:-}
RESOLUTION=${RESOLUTION:-1280x800}
WAIT_FOR=${WAIT_FOR:-}
LOG=${LOG:-}
TIMEOUT=${TIMEOUT:-180}
RETRIES=${RETRIES:-3}
RETRY_DELAY=${RETRY_DELAY:-40}
EXTRA_ARGS=${EXTRA_ARGS---nopatch 1}
ENV=${ENV:-}
LIGHT=${LIGHT:-off}
NOLIGHT=${NOLIGHT:-0}
CAPTURE=${CAPTURE:-}
CAPTURE_SRC=${CAPTURE_SRC:-}
CAPTURE_TIMEOUT=${CAPTURE_TIMEOUT:-15}
SETTLE=${SETTLE:-2}
GAMESCOPE_BACKEND=${GAMESCOPE_BACKEND:-}
HARNESS_DIR=${HARNESS_DIR:-}
HARNESS_KEY=${HARNESS_KEY:-}
STATE_FILE=${STATE_FILE:-}
REUSE=${REUSE:-0}
STOP=${STOP:-0}
DRYRUN=${DRYRUN:-0}

CONFIG_VARS=" DRIVER SCREEN HEADLESS CLIENT ACCOUNT PASSWORD SLOT RESOLUTION WAIT_FOR LOG TIMEOUT RETRIES RETRY_DELAY EXTRA_ARGS ENV LIGHT NOLIGHT CAPTURE CAPTURE_SRC CAPTURE_TIMEOUT SETTLE GAMESCOPE_BACKEND HARNESS_DIR HARNESS_KEY STATE_FILE REUSE STOP DRYRUN "

EXTRA_ENV=()
after_dashdash=0
for arg in "$@"; do
	if [ "$after_dashdash" = 1 ]; then
		EXTRA_ENV+=("$arg")
		continue
	fi
	case "$arg" in
	-h|--help) usage ;;
	--) after_dashdash=1 ;;
	*=*)
		key=${arg%%=*}
		value=${arg#*=}
		case "$CONFIG_VARS" in
		*" $key "*) printf -v "$key" '%s' "$value" ;;
		*) die "unknown parameter '$key' (see --help)" ;;
		esac
		;;
	*)
		die "unknown argument '$arg' (use KEY=VALUE, -- ENV=value ..., or --help)"
		;;
	esac
done

if [ "${#EXTRA_ENV[@]}" -gt 0 ]; then
	ENV="${ENV:+$ENV }${EXTRA_ENV[*]}"
fi

bool_var() {
	case "$2" in
	1|yes|true|on) printf -v "$1" '%s' 1 ;;
	0|no|false|off|'') printf -v "$1" '%s' 0 ;;
	*) die "$1 must be 0 or 1" ;;
	esac
}
bool_var NOLIGHT "$NOLIGHT"
bool_var REUSE "$REUSE"
bool_var STOP "$STOP"
bool_var DRYRUN "$DRYRUN"

case "$HEADLESS" in
1|yes|true|on)
	case "$SCREEN" in
	gamescope|gamescope-headless) SCREEN=gamescope-headless ;;
	*) die "HEADLESS=1 requires SCREEN=gamescope (or gamescope-headless)" ;;
	esac
	;;
0|no|false|off|'') ;;
*) die "HEADLESS must be 0 or 1" ;;
esac

[ -z "$WAIT_FOR" ] && { [ -n "$ACCOUNT" ] && WAIT_FOR=ingame || WAIT_FOR=window; }
case "$WAIT_FOR" in ingame|login|window) ;; *) die "WAIT_FOR must be ingame, login or window" ;; esac
case "$SCREEN" in gamescope|gamescope-headless|xvfb|none) ;; *) die "SCREEN must be gamescope, gamescope-headless, xvfb or none" ;; esac
case "$LIGHT" in off|on|toggle) ;; *) die "LIGHT must be off, on or toggle" ;; esac

if [ "$STOP" = 1 ] && [ "$REUSE" = 1 ]; then
	die "STOP=1 and REUSE=1 are mutually exclusive"
fi
if [ -n "$CAPTURE" ] && [ "$WAIT_FOR" != ingame ]; then
	die "CAPTURE=... requires WAIT_FOR=ingame"
fi
if [ "$NOLIGHT" = 1 ] && [ "$LIGHT" != off ]; then
	warn "NOLIGHT=1 overrides LIGHT=$LIGHT"
fi

case "$RESOLUTION" in
*x*) SCREEN_W=${RESOLUTION%x*}; SCREEN_H=${RESOLUTION#*x} ;;
*) die "RESOLUTION must be WIDTHxHEIGHT, e.g. 1280x800" ;;
esac
[[ "$SCREEN_W" =~ ^[0-9]+$ && "$SCREEN_H" =~ ^[0-9]+$ ]] || die "bad RESOLUTION '$RESOLUTION'"

case "$TIMEOUT" in ''|*[!0-9]*) die "TIMEOUT must be a number" ;; esac
case "$RETRIES" in ''|*[!0-9]*) die "RETRIES must be a number" ;; esac
case "$RETRY_DELAY" in ''|*[!0-9]*) die "RETRY_DELAY must be a number" ;; esac
case "$CAPTURE_TIMEOUT" in ''|*[!0-9]*) die "CAPTURE_TIMEOUT must be a number" ;; esac
case "$SETTLE" in ''|*[!0-9]*) die "SETTLE must be a number" ;; esac

# ---------------------------------------------------------------------------
# Environment passed to the client
# ---------------------------------------------------------------------------

ENV_ASSIGNMENTS=()
if [ -n "$ENV" ]; then
	read -r -a _env_words <<<"$ENV"
	for kv in "${_env_words[@]}"; do
		case "$kv" in
		*=*) ;;
		*) die "ENV entry '$kv' is not NAME=value" ;;
		esac
		name=${kv%%=*}
		[[ "$name" =~ ^[A-Za-z_][A-Za-z0-9_]*$ ]] \
			|| die "ENV entry '$kv' is not NAME=value"
		case "$name" in
		DISPLAY|RYZOM_HARNESS_TOKEN|RYZOM_HARNESS_STATE)
			die "ENV cannot set $name (reserved by the harness)" ;;
		esac
		ENV_ASSIGNMENTS+=("$kv")
	done
fi

# The personal light is kept off deterministically by the client-side
# development hook, not by pressing 'l'.  "off" (the default) adds the hook;
# NOLIGHT=1 and LIGHT=on leave it out; LIGHT=toggle is the keyboard fallback.
FORCE_LIGHT_OFF=0
for kv in "${ENV_ASSIGNMENTS[@]}"; do
	case "$kv" in RYZOM_FORCE_LIGHT_OFF=*) FORCE_LIGHT_OFF=1 ;; esac
done
if [ "$NOLIGHT" != 1 ] && [ "$LIGHT" = off ] && [ "$FORCE_LIGHT_OFF" != 1 ]; then
	ENV_ASSIGNMENTS+=("RYZOM_FORCE_LIGHT_OFF=1")
	FORCE_LIGHT_OFF=1
fi
if [ "$FORCE_LIGHT_OFF" = 1 ]; then
	if [ "$NOLIGHT" = 1 ]; then
		warn "NOLIGHT=1 but RYZOM_FORCE_LIGHT_OFF is set; the client will keep the light off"
	else
		if [ "$LIGHT" = toggle ]; then
			die "LIGHT=toggle conflicts with RYZOM_FORCE_LIGHT_OFF (the hook would ignore the 'l' press)"
		fi
		if [ "$LIGHT" = on ]; then
			die "LIGHT=on conflicts with RYZOM_FORCE_LIGHT_OFF (the hook keeps the light off)"
		fi
	fi
fi

# ---------------------------------------------------------------------------
# Paths and state file
# ---------------------------------------------------------------------------

CLIENT_ABS=$(readlink -f "$CLIENT" 2>/dev/null || true)
[ -n "$CLIENT_ABS" ] || CLIENT_ABS=$CLIENT

HARNESS_DIR=${HARNESS_DIR:-$PWD/tmp/harness}
STATE_KEY=${HARNESS_KEY:-${ACCOUNT:-login}-slot${SLOT:-any}}
STATE_KEY=$(printf '%s' "$STATE_KEY" | tr -c 'A-Za-z0-9._-' '_')
STATE_FILE=${STATE_FILE:-$HARNESS_DIR/$STATE_KEY.pid}
STATE_FILE=$(readlink -m "$STATE_FILE")

LOG=${LOG:-$PWD/client.log}
LOG=$(readlink -m "$LOG")
CAPTURE_SRC=${CAPTURE_SRC:-$PWD/screenshots}
CAPTURE_SRC=$(readlink -m "$CAPTURE_SRC")
[ -n "$CAPTURE" ] && CAPTURE=$(readlink -m "$CAPTURE")

if [ -x "$CLIENT" ]; then
	: # resolved above
elif [ "$STOP" != 1 ]; then
	die "client '$CLIENT' not found or not executable (set CLIENT=...)"
fi

# A client for the login screen has no account to key on; it still gets a
# stable state file so STOP=1 can find it.
if [ -n "$DRIVER" ] && [ "$DRIVER" != keep ]; then
	case "$DRIVER" in Auto|OpenGL|OpenGL3|Vulkan|DirectX) ;; *)
		die "DRIVER must be Auto, OpenGL, OpenGL3, Vulkan or DirectX" ;;
	esac
fi

GAMESCOPE_ARGS=()
if [ "$SCREEN" = gamescope ] || [ "$SCREEN" = gamescope-headless ]; then
	if [ -z "$GAMESCOPE_BACKEND" ]; then
		[ -n "${WAYLAND_DISPLAY:-}" ] && GAMESCOPE_BACKEND=wayland || GAMESCOPE_BACKEND=x11
	fi
	GAMESCOPE_ARGS=(-W "$SCREEN_W" -H "$SCREEN_H" -w "$SCREEN_W" -h "$SCREEN_H")
	if [ "$SCREEN" = gamescope-headless ]; then
		GAMESCOPE_ARGS+=(--backend headless)
	else
		GAMESCOPE_ARGS+=(--force-windows-fullscreen --backend "$GAMESCOPE_BACKEND")
	fi
fi

# Build the client command line and environment now: dry runs print them, and
# the real start uses the same arrays.
CLIENT_ARGS=()
if [ -n "$ACCOUNT" ]; then
	CLIENT_ARGS+=("$ACCOUNT" "$PASSWORD")
	if [ -n "$SLOT" ]; then
		CLIENT_ARGS+=(slot "$SLOT")
	fi
fi
if [ -n "$EXTRA_ARGS" ]; then
	read -r -a _extra_args <<<"$EXTRA_ARGS"
	CLIENT_ARGS+=("${_extra_args[@]}")
fi

quote_cmd() {
	[ "$#" -gt 0 ] && printf '%q ' "$@"
	return 0
}

# ---------------------------------------------------------------------------
# State helpers
# ---------------------------------------------------------------------------

state_get() {
	[ -r "$STATE_FILE" ] || return 0
	sed -n "s/^$1=//p" "$STATE_FILE" | head -n1
}

write_state() {
	[ "$DRYRUN" = 1 ] && return 0
	[ -n "${CLIENT_PID:-}" ] || return 0
	mkdir -p "$(dirname "$STATE_FILE")" 2>/dev/null || true
	{
		printf 'PID=%s\n' "$CLIENT_PID"
		printf 'SCREEN_PID=%s\n' "${SCREEN_PID:-}"
		printf 'SCREEN=%s\n' "$SCREEN"
		printf 'DISPLAY=%s\n' "${CLIENT_DISPLAY:-}"
		printf 'CLIENT=%s\n' "$CLIENT_ABS"
		printf 'ACCOUNT=%s\n' "$ACCOUNT"
		printf 'SLOT=%s\n' "$SLOT"
		printf 'LOG=%s\n' "$LOG"
		printf 'LIGHT_OFF=%s\n' "${LIGHT_OFF_STATE:-unknown}"
		printf 'KEY_MODE=%s\n' "${KEY_MODE_STATE:-unknown}"
	} >"$STATE_FILE.tmp.$$" && mv -f "$STATE_FILE.tmp.$$" "$STATE_FILE"
}

pid_has_state_env() {
	[ -r "/proc/$1/environ" ] || return 1
	{ tr '\0' '\n' <"/proc/$1/environ"; } 2>/dev/null \
		| grep -a -x -F "RYZOM_HARNESS_STATE=$STATE_FILE" >/dev/null
}

pid_is_our_client() {
	local pid=$1 exe arg prev=
	[ -n "$pid" ] && kill -0 "$pid" 2>/dev/null || return 1
	exe=$(readlink "/proc/$pid/exe" 2>/dev/null) || return 1
	exe=${exe% (deleted)}
	[ "$exe" = "$CLIENT_ABS" ] || return 1
	pid_has_state_env "$pid" || return 1
	# Hard safety net: a profile-0 client is never ours to stop, even if it
	# somehow inherited the state variable.
	while IFS= read -r arg; do
		if [ "$prev" = "--profile" ] && [ "$arg" = "0" ]; then
			return 1
		fi
		case "$arg" in --profile=0) return 1 ;; esac
		prev=$arg
	done < <(tr '\0' '\n' <"/proc/$pid/cmdline" 2>/dev/null)
	return 0
}

stop_pid() {
	local pid=$1 i
	[ -n "$pid" ] || return 0
	if ! pid_is_our_client "$pid"; then
		info "not stopping pid $pid: not this harness's client"
		return 0
	fi
	info "stopping harness client $pid ($(basename "$CLIENT_ABS"))"
	kill "$pid" 2>/dev/null
	for i in $(seq 1 40); do
		kill -0 "$pid" 2>/dev/null || return 0
		sleep 0.25
	done
	kill -9 "$pid" 2>/dev/null
}

stop_screen_pid() {
	local pid=$1 i
	[ -n "$pid" ] || return 0
	kill -0 "$pid" 2>/dev/null || return 0
	pid_has_state_env "$pid" || return 0
	kill "$pid" 2>/dev/null
	for i in $(seq 1 20); do
		kill -0 "$pid" 2>/dev/null || return 0
		sleep 0.25
	done
	kill -9 "$pid" 2>/dev/null
}

stop_tracked() {
	local pid screen
	pid=$(state_get PID)
	screen=$(state_get SCREEN_PID)
	stop_pid "$pid"
	stop_screen_pid "$screen"
	rm -f "$STATE_FILE"
}

# ---------------------------------------------------------------------------
# client.cfg
# ---------------------------------------------------------------------------

if [ -n "$DRIVER" ] && [ "$DRIVER" != keep ] && [ "$STOP" != 1 ] && [ "$REUSE" != 1 ]; then
	if [ "$DRYRUN" = 1 ]; then
		[ -f client.cfg ] || die "client.cfg not found in $PWD"
		printf 'DRYRUN: client.cfg: would set Driver3D="%s" (backup %s/client.cfg.harness-backup)\n' \
			"$DRIVER" "$PWD"
	else
		[ -f client.cfg ] || die "client.cfg not found in $PWD"
		[ -f client.cfg.harness-backup ] || cp -p client.cfg client.cfg.harness-backup
		sed -i -E "s|^([[:space:]]*Driver3D[[:space:]]*=[[:space:]]*\")[^\"]*(\";)|\1${DRIVER}\2|" client.cfg
		grep -qE "^[[:space:]]*Driver3D[[:space:]]*=[[:space:]]*\"${DRIVER}\";" client.cfg \
			|| die "could not set Driver3D=\"$DRIVER\" in client.cfg"
		info "client.cfg: Driver3D=\"$DRIVER\" (backup in client.cfg.harness-backup)"
	fi
fi

# ---------------------------------------------------------------------------
# Modes that do not start anything
# ---------------------------------------------------------------------------

if [ "$STOP" = 1 ]; then
	if [ "$DRYRUN" = 1 ]; then
		printf 'DRYRUN: would stop the client tracked in %s\n' "$STATE_FILE"
	else
		stop_tracked
		info "stopped the client tracked in $STATE_FILE (if any)"
	fi
	exit 0
fi

# Isolate the Vulkan pipeline cache per run dir **and per client build**:
# concurrent test clients, or a rebuilt binary in the same install, must not
# share (or race on) one blob.  Size+mtime changes on every relink.
_CLIENT_TAG=$(stat -c '%s-%Y' "$CLIENT_ABS" 2>/dev/null | tr -d ' ' || true)
[ -n "$_CLIENT_TAG" ] || _CLIENT_TAG=dev
export NLVK_PIPELINE_CACHE_PATH=${NLVK_PIPELINE_CACHE_PATH:-$HARNESS_DIR/pipeline-cache-$_CLIENT_TAG.bin}

if [ "$DRYRUN" = 1 ]; then
	printf 'DRYRUN: client=%s\n' "$CLIENT_ABS"
	printf 'DRYRUN: account=%s slot=%s screen=%s wait_for=%s\n' \
		"${ACCOUNT:-<login screen>}" "${SLOT:-<default>}" "$SCREEN" "$WAIT_FOR"
	printf 'DRYRUN: state_file=%s\n' "$STATE_FILE"
	printf 'DRYRUN: log=%s\n' "$LOG"
	printf 'DRYRUN: pipeline_cache=%s\n' "$NLVK_PIPELINE_CACHE_PATH"
	printf 'DRYRUN: client_env=RYZOM_HARNESS_TOKEN=<per-run> RYZOM_HARNESS_STATE=%s ' "$STATE_FILE"
	quote_cmd "${ENV_ASSIGNMENTS[@]}"
	printf '\n'
	if [ "$SCREEN" = gamescope ] || [ "$SCREEN" = gamescope-headless ]; then
		printf 'DRYRUN: start: '
		quote_cmd env "${ENV_ASSIGNMENTS[@]}" gamescope "${GAMESCOPE_ARGS[@]}" \
			-- "$CLIENT_ABS" "${CLIENT_ARGS[@]}"
		printf '\n'
	else
		printf 'DRYRUN: start: '
		quote_cmd env "${ENV_ASSIGNMENTS[@]}" "$CLIENT_ABS" "${CLIENT_ARGS[@]}"
		printf '\n'
	fi
	printf 'DRYRUN: wait: WAIT_FOR=%s TIMEOUT=%s RETRIES=%s RETRY_DELAY=%s\n' \
		"$WAIT_FOR" "$TIMEOUT" "$RETRIES" "$RETRY_DELAY"
	if [ "$NOLIGHT" = 1 ]; then
		printf 'DRYRUN: light: NOLIGHT=1 (no hook, no key press)\n'
	elif [ "$WAIT_FOR" != ingame ]; then
		printf 'DRYRUN: light: skipped (not in game)\n'
	else
		case "$LIGHT" in
		off) printf 'DRYRUN: light: LIGHT=off -> RYZOM_FORCE_LIGHT_OFF=1 in the client env (no key press)\n' ;;
		on) printf 'DRYRUN: light: LIGHT=on (no hook; game default, on for a fresh character)\n' ;;
		toggle) printf 'DRYRUN: light: LIGHT=toggle (keyboard fallback, presses l once)\n' ;;
		esac
	fi
	if [ -n "$CAPTURE" ]; then
		printf 'DRYRUN: capture: would take one screenshot into %s\n' "$CAPTURE"
	fi
	if [ "$REUSE" = 1 ]; then
		printf 'DRYRUN: reuse: would reuse the tracked client if it matches\n'
	fi
	exit 0
fi

mkdir -p "$HARNESS_DIR" || die "cannot create '$HARNESS_DIR'"

# ---------------------------------------------------------------------------
# Runtime state
# ---------------------------------------------------------------------------

TOKEN="ryzom-harness-$$-$RANDOM"
SCREEN_PID=
CLIENT_PID=
SCREEN_DISPLAY=
CLIENT_DISPLAY=
STARTED_SCREEN=0
HARNESS_OK=0
REUSED=0
WIN=
NEW_SHOT=
LIGHT_OFF_STATE=unknown
KEY_MODE_STATE=unknown
LAUNCH_EPOCH=0
LAST_FULL_SCAN=-100

cleanup() {
	local rc=$?
	# A reused client belongs to an earlier invocation: on a failed wait it is
	# left running, exactly like a client started on purpose outside the wait.
	if [ "$HARNESS_OK" != 1 ] && [ "$REUSED" != 1 ]; then
		stop_pid "${CLIENT_PID:-}"
		stop_screen_pid "${SCREEN_PID:-}"
		rm -f "$STATE_FILE"
	fi
	exit "$rc"
}
trap cleanup EXIT INT TERM

# ---------------------------------------------------------------------------
# Virtual screen and client process
# ---------------------------------------------------------------------------

find_client_pid() {
	local p pid exe
	for p in /proc/[0-9]*; do
		pid=${p#/proc/}
		[ "$pid" = "$$" ] && continue
		[ -r "$p/environ" ] || continue
		{ tr '\0' '\n' <"$p/environ"; } 2>/dev/null \
			| grep -a -x -F "RYZOM_HARNESS_TOKEN=$TOKEN" >/dev/null || continue
		exe=$(readlink "$p/exe" 2>/dev/null) || continue
		exe=${exe% (deleted)}
		[ "$exe" = "$CLIENT_ABS" ] && { printf '%s\n' "$pid"; return 0; }
	done
	return 1
}

client_display() {
	if [ -n "$CLIENT_PID" ]; then
		{ tr '\0' '\n' <"/proc/$CLIENT_PID/environ"; } 2>/dev/null \
			| sed -n 's/^DISPLAY=//p' | head -n1
	fi
}

start_client() {
	local client_env=(
		"RYZOM_HARNESS_TOKEN=$TOKEN"
		"RYZOM_HARNESS_STATE=$STATE_FILE"
		"${ENV_ASSIGNMENTS[@]}"
	)

	[ -f "$LOG" ] && mv -f "$LOG" "$LOG.previous"
	LAUNCH_EPOCH=$(date +%s)

	case "$SCREEN" in
	gamescope|gamescope-headless)
		env "${client_env[@]}" \
			gamescope "${GAMESCOPE_ARGS[@]}" -- "$CLIENT_ABS" "${CLIENT_ARGS[@]}" \
			>>"$HARNESS_DIR/gamescope.log" 2>&1 &
		SCREEN_PID=$!
		STARTED_SCREEN=1
		;;
	xvfb)
		local n=99
		while [ -e "/tmp/.X11-unix/X$n" ] || [ -e "/tmp/.X$n-lock" ]; do
			n=$((n + 1))
		done
		SCREEN_DISPLAY=":$n"
		env "${client_env[@]}" Xvfb "$SCREEN_DISPLAY" \
			-screen 0 "${SCREEN_W}x${SCREEN_H}x24" -nolisten tcp \
			>>"$HARNESS_DIR/xvfb.log" 2>&1 &
		SCREEN_PID=$!
		STARTED_SCREEN=1
		for _ in $(seq 1 40); do
			[ -e "/tmp/.X11-unix/X$n" ] && break
			sleep 0.25
		done
		env DISPLAY="$SCREEN_DISPLAY" "${client_env[@]}" \
			"$CLIENT_ABS" "${CLIENT_ARGS[@]}" >>"$HARNESS_DIR/client.out" 2>&1 &
		CLIENT_PID=$!
		;;
	none)
		[ -n "${DISPLAY:-}" ] || die "SCREEN=none needs DISPLAY to be set"
		env "${client_env[@]}" "$CLIENT_ABS" "${CLIENT_ARGS[@]}" \
			>>"$HARNESS_DIR/client.out" 2>&1 &
		CLIENT_PID=$!
		;;
	esac

	# gamescope starts the client itself; find it through the token.
	if [ -z "${CLIENT_PID:-}" ]; then
		for _ in $(seq 1 80); do
			CLIENT_PID=$(find_client_pid) && break
			CLIENT_PID=
			sleep 0.25
		done
		[ -n "${CLIENT_PID:-}" ] \
			|| die "client did not start in the virtual screen (see $HARNESS_DIR/gamescope.log)"
	fi

	CLIENT_DISPLAY=$(client_display)
	[ "$FORCE_LIGHT_OFF" = 1 ] && LIGHT_OFF_STATE=1 || LIGHT_OFF_STATE=0
	KEY_MODE_STATE=unknown
	write_state
}

reuse_client() {
	local pid screen env_ok=1 kv
	pid=$(state_get PID)
	if [ -z "$pid" ]; then
		info "REUSE=1: no client is tracked in $STATE_FILE"
		return 6
	fi
	if ! pid_is_our_client "$pid"; then
		info "REUSE=1: tracked pid $pid is not this harness's client; clearing the stale state"
		rm -f "$STATE_FILE"
		return 6
	fi
	# From here on the client is ours: a failed reuse must not delete its state
	# file or stop it (cleanup checks this flag).
	REUSED=1
	for kv in "${ENV_ASSIGNMENTS[@]}"; do
		{ tr '\0' '\n' <"/proc/$pid/environ"; } 2>/dev/null \
			| grep -a -x -F "$kv" >/dev/null || { env_ok=0; break; }
	done
	if [ "$env_ok" != 1 ]; then
		info "REUSE=1: tracked client $pid does not have the requested ENV"
		return 6
	fi

	CLIENT_PID=$pid
	SCREEN_PID=$(state_get SCREEN_PID)
	SCREEN=$(state_get SCREEN)
	LOG=$(state_get LOG)
	[ -n "$LOG" ] || LOG=$PWD/client.log
	LAUNCH_EPOCH=$(pid_start_epoch "$pid" 2>/dev/null || echo 0)
	LAST_FULL_SCAN=-100
	CLIENT_DISPLAY=$(client_display)
	[ -n "$CLIENT_DISPLAY" ] || CLIENT_DISPLAY=$(state_get DISPLAY)
	LIGHT_OFF_STATE=$(state_get LIGHT_OFF)
	[ -n "$LIGHT_OFF_STATE" ] || LIGHT_OFF_STATE=unknown
	# The ENV match above guarantees the reused client has the hook when it was
	# requested, so state 1 is authoritative in that case.
	[ "$FORCE_LIGHT_OFF" = 1 ] && LIGHT_OFF_STATE=1
	KEY_MODE_STATE=$(state_get KEY_MODE)
	[ -n "$KEY_MODE_STATE" ] || KEY_MODE_STATE=unknown
	info "reusing tracked client $pid (DISPLAY=${CLIENT_DISPLAY:-unknown}, log=$LOG)"
	return 0
}

# ---------------------------------------------------------------------------
# Log monitoring
# ---------------------------------------------------------------------------
#
# In-game detection scans the current client.log for "initInGame" after the
# last "Log Starting".  Rotated logs (clientNNN.log, findNewFile order) are
# included when the client has rotated during this session; the newer builds
# only log a single line for the modern shadows, so rotation is rare.
#
# Rotated logs are ordered by mtime, oldest first.  For a fresh start only logs
# touched after the launch are considered, so a previous session's initInGame
# in an old rotated file cannot be mistaken for the new client.

# Wall-clock start time of a process, used to ignore rotated logs from older
# sessions when REUSE=1.  /proc/<pid>/stat field 22 is starttime in clock ticks
# since boot; fields after comm are counted after stripping "pid (comm) ".
pid_start_epoch() {
	local pid=$1 line rest ticks hz btime
	line=$(cat "/proc/$pid/stat" 2>/dev/null) || return 1
	rest=${line##*) }
	ticks=$(awk '{print $20}' <<<"$rest")
	hz=$(getconf CLK_TCK 2>/dev/null || echo 100)
	btime=$(awk '/^btime /{print $2}' /proc/stat 2>/dev/null)
	[ -n "$ticks" ] && [ -n "$btime" ] || return 1
	printf '%s\n' "$((btime + ticks / hz))"
}

log_files_since() {
	local base=${LOG%.log} f mtime
	if [ "$base" = "$LOG" ]; then
		printf '%s\n' "$LOG"
		return 0
	fi
	while IFS= read -r f; do
		[ -e "$f" ] || continue
		if [ "$LAUNCH_EPOCH" -gt 0 ]; then
			mtime=$(stat -c %Y "$f" 2>/dev/null || echo 0)
			[ "$mtime" -ge "$((LAUNCH_EPOCH - 2))" ] || continue
		fi
		printf '%s\n' "$f"
	done < <(ls -1tr "$base"[0-9][0-9][0-9].log 2>/dev/null)
	printf '%s\n' "$LOG"
}

log_session_in_game() {
	local files=() f
	while IFS= read -r f; do
		files+=("$f")
	done < <(log_files_since)
	cat "${files[@]}" 2>/dev/null | awk '
		/Log Starting/ { in_game = 0 }
		/initInGame/ { in_game = 1 }
		END { exit !in_game }'
}

in_game_now() {
	# The full scan can read a few MB; do it at most every two seconds.
	if [ $((SECONDS - LAST_FULL_SCAN)) -lt 2 ]; then
		return 1
	fi
	LAST_FULL_SCAN=$SECONDS
	log_session_in_game
}

# Returns 0 started, 2 account busy (retry), 3 timeout, 4 client died.
# The second argument is 0 for REUSE=1: the current log is not fresh, so old
# "already online" lines from a previous session must not abort the wait.
wait_for_client() {
	local check_fail=${1:-1}
	local deadline=$((SECONDS + TIMEOUT))
	local fail_re='already (in online state|logged|online|connected)|another user uses your account|disconnected by the server|connection of ryzom failed|login failed'

	while [ "$SECONDS" -lt "$deadline" ]; do
		if ! kill -0 "$CLIENT_PID" 2>/dev/null; then
			return 4
		fi
		if [ "$check_fail" = 1 ] && [ -f "$LOG" ] && grep -aqiE "$fail_re" "$LOG"; then
			return 2
		fi
		case "$WAIT_FOR" in
		ingame)
			in_game_now && return 0
			;;
		login)
			if grep -aq 'Connection of Ryzom success' "$LOG" 2>/dev/null; then
				# A busy account can disconnect right after the handshake.
				sleep 3
				grep -aqiE "$fail_re" "$LOG" && return 2
				return 0
			fi
			;;
		window)
			if command -v xdotool >/dev/null 2>&1; then
				local display
				display=$(client_display)
				[ -n "$display" ] \
					&& [ -n "$(DISPLAY=$display xdotool search --name Ryzom 2>/dev/null | head -n1)" ] \
					&& return 0
			elif grep -aq 'display created with' "$LOG" 2>/dev/null; then
				return 0
			fi
			;;
		esac
		sleep 0.5
	done
	return 3
}

# ---------------------------------------------------------------------------
# Key injection and screenshots
# ---------------------------------------------------------------------------

find_window() {
	local ids id name
	[ -n "$CLIENT_DISPLAY" ] || CLIENT_DISPLAY=$(client_display)
	[ -n "$CLIENT_DISPLAY" ] || return 1
	ids=$(DISPLAY=$CLIENT_DISPLAY xdotool search --name Ryzom 2>/dev/null || true)
	[ -n "$ids" ] || return 1
	for id in $ids; do
		name=$(DISPLAY=$CLIENT_DISPLAY xdotool getwindowname "$id" 2>/dev/null || true)
		case "$name" in
		*Ryzom*) printf '%s\n' "$id"; return 0 ;;
		esac
	done
	return 1
}

ensure_window() {
	local deadline=$((SECONDS + 30))
	if [ -n "$WIN" ]; then
		return 0
	fi
	while [ "$SECONDS" -lt "$deadline" ]; do
		WIN=$(find_window) && [ -n "$WIN" ] && return 0
		kill -0 "$CLIENT_PID" 2>/dev/null || return 1
		sleep 0.5
	done
	return 1
}

list_shots() {
	find "$CAPTURE_SRC" -maxdepth 1 -type f \( -name '*.jpg' -o -name '*.png' \) \
		-printf '%f\n' 2>/dev/null | sort
}

wait_new_shot() {
	local before=$1 timeout=${2:-$CAPTURE_TIMEOUT}
	local deadline=$((SECONDS + timeout)) new
	while [ "$SECONDS" -lt "$deadline" ]; do
		new=$(comm -13 <(printf '%s\n' "$before") <(list_shots) | tail -n1)
		if [ -n "$new" ]; then
			NEW_SHOT=$new
			return 0
		fi
		sleep 0.2
	done
	return 1
}

send_key() {
	local key=$1
	case "$KEY_MODE_STATE" in
	window)
		DISPLAY=$CLIENT_DISPLAY xdotool key --window "$WIN" --clearmodifiers "$key" 2>/dev/null
		;;
	global)
		DISPLAY=$CLIENT_DISPLAY xdotool windowfocus "$WIN" 2>/dev/null || true
		DISPLAY=$CLIENT_DISPLAY xdotool key --clearmodifiers "$key" 2>/dev/null
		;;
	*) return 1 ;;
	esac
}

probe_key_mode() {
	local before
	ensure_window || return 1
	before=$(list_shots)
	DISPLAY=$CLIENT_DISPLAY xdotool key --window "$WIN" --clearmodifiers ctrl+F10 2>/dev/null || true
	if wait_new_shot "$before"; then
		KEY_MODE_STATE=window
		write_state
		info "input mode: window-targeted keys (probe $NEW_SHOT)"
		return 0
	fi
	before=$(list_shots)
	DISPLAY=$CLIENT_DISPLAY xdotool windowfocus "$WIN" 2>/dev/null || true
	DISPLAY=$CLIENT_DISPLAY xdotool key --clearmodifiers ctrl+F10 2>/dev/null || true
	if wait_new_shot "$before"; then
		KEY_MODE_STATE=global
		write_state
		info "input mode: display focus / XTEST (probe $NEW_SHOT)"
		return 0
	fi
	return 1
}

ensure_key_mode() {
	command -v xdotool >/dev/null 2>&1 || { warn "xdotool not found"; return 1; }
	case "$KEY_MODE_STATE" in
	window|global)
		# The cached mode is only useful while the window still exists.
		ensure_window || { KEY_MODE_STATE=unknown; return 1; }
		return 0
		;;
	esac
	probe_key_mode
}

take_shot() {
	local before
	ensure_key_mode || return 1
	before=$(list_shots)
	send_key ctrl+F10 || true
	wait_new_shot "$before" && return 0
	if [ "$KEY_MODE_STATE" = window ]; then
		warn "window-targeted Ctrl+F10 stopped working; retrying with display focus"
		KEY_MODE_STATE=global
		write_state
		before=$(list_shots)
		send_key ctrl+F10 || true
		wait_new_shot "$before" && return 0
	fi
	return 1
}

wait_shot_stable() {
	local file=$CAPTURE_SRC/$1 a=0 b i
	for i in $(seq 1 15); do
		b=$(stat -c %s "$file" 2>/dev/null || echo 0)
		if [ "$b" -gt 1000 ] && [ "$a" = "$b" ]; then
			return 0
		fi
		a=$b
		sleep 0.2
	done
	[ "$a" -gt 1000 ]
}

# ---------------------------------------------------------------------------
# Personal light and capture
# ---------------------------------------------------------------------------

do_light() {
	if [ "$NOLIGHT" = 1 ]; then
		LIGHT_OFF_STATE=untouched
		info "NOLIGHT=1: leaving the personal light alone"
		return 0
	fi
	if [ "$WAIT_FOR" != ingame ]; then
		LIGHT_OFF_STATE=untouched
		info "not in game (WAIT_FOR=$WAIT_FOR): leaving the personal light alone"
		return 0
	fi

	case "$LIGHT" in
	off)
		if [ "$FORCE_LIGHT_OFF" = 1 ]; then
			LIGHT_OFF_STATE=1
			info "personal light off for the whole session (RYZOM_FORCE_LIGHT_OFF=1, no key press)"
			return 0
		fi
		if [ "$LIGHT_OFF_STATE" = 1 ]; then
			info "personal light already off (state file)"
			return 0
		fi
		# Only possible when the caller deliberately removed the hook; do not
		# fall back to the keyboard here, that is what LIGHT=toggle is for.
		warn "client was started without RYZOM_FORCE_LIGHT_OFF=1; personal light may be on"
		warn "restart without REUSE=1, or use LIGHT=toggle for the keyboard fallback"
		return 1
		;;
	on)
		if [ "$LIGHT_OFF_STATE" = 1 ]; then
			warn "this client was started with RYZOM_FORCE_LIGHT_OFF=1; restart without REUSE=1 to turn the light on"
			return 1
		fi
		LIGHT_OFF_STATE=0
		info "personal light left on (no RYZOM_FORCE_LIGHT_OFF; a fresh character starts lit)"
		return 0
		;;
	toggle)
		if ! ensure_key_mode; then
			warn "cannot determine how to send keys to the client; personal light left alone"
			return 1
		fi
		send_key l || true
		sleep 0.3
		LIGHT_OFF_STATE=unknown
		info "pressed 'l' once (keyboard fallback; personal light toggled)"
		write_state
		return 0
		;;
	esac
}

light_state_label() {
	case "${LIGHT_OFF_STATE:-unknown}" in
	1) printf 'off' ;;
	0) printf 'on' ;;
	*) printf '%s' "$LIGHT_OFF_STATE" ;;
	esac
}

do_capture() {
	local dst stamp ext
	[ -n "$CAPTURE" ] || return 0
	[ "$WAIT_FOR" = ingame ] || die "CAPTURE=... requires WAIT_FOR=ingame"
	mkdir -p "$CAPTURE" || die "cannot create capture directory '$CAPTURE'"
	mkdir -p "$CAPTURE_SRC" 2>/dev/null || true
	sleep "$SETTLE"
	take_shot || die "Ctrl+F10 produced no screenshot in $CAPTURE_SRC"
	wait_shot_stable "$NEW_SHOT" || warn "screenshot $NEW_SHOT is still growing"
	stamp=$(date +%Y%m%d-%H%M%S)
	ext=${NEW_SHOT##*.}
	dst=$CAPTURE/${ACCOUNT:-login}-slot${SLOT:-any}-$stamp-$$.$ext
	cp -- "$CAPTURE_SRC/$NEW_SHOT" "$dst" || die "cannot copy screenshot to '$dst'"
	printf 'CAPTURE=%s\n' "$dst"
	info "captured $dst"
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

if [ "$REUSE" = 1 ]; then
	reuse_client || exit 6
	info "attempt 1/1 (REUSE=1): pid $CLIENT_PID, WAIT_FOR=$WAIT_FOR"
	wait_for_client 0
	result=$?
	if [ "$result" -ne 0 ]; then
		case "$result" in
		4) info "reused client exited before it was ready (see $LOG)"; exit 4 ;;
		*) info "timeout after ${TIMEOUT}s waiting for '$WAIT_FOR' on the reused client (see $LOG)"; exit 3 ;;
		esac
	fi
	CLIENT_DISPLAY=$(client_display)
	[ -n "$CLIENT_DISPLAY" ] || die "reused client $CLIENT_PID reports no DISPLAY"
	HARNESS_OK=1
	write_state
	printf 'DISPLAY=%s\n' "$CLIENT_DISPLAY"
	printf 'CLIENT_PID=%s\n' "$CLIENT_PID"
	printf 'LOG=%s\n' "$LOG"
	do_light || true
	printf 'LIGHT=%s\n' "$(light_state_label)"
	do_capture
	exit 0
fi

# Starting a new client: stop the one this harness recorded for the same
# account/slot first.  Never scan for clients by name.
attempt=1
while :; do
	info "attempt $attempt/$RETRIES: $CLIENT ($SCREEN, Driver3D=${DRIVER:-unchanged}, WAIT_FOR=$WAIT_FOR)"

	if [ "$attempt" -eq 1 ]; then
		stop_tracked
	fi

	start_client

	wait_for_client 1
	result=$?

	if [ "$result" -eq 0 ]; then
		CLIENT_DISPLAY=$(client_display)
		[ -n "$CLIENT_DISPLAY" ] || die "started but no DISPLAY found for pid $CLIENT_PID"
		HARNESS_OK=1
		write_state
		printf 'DISPLAY=%s\n' "$CLIENT_DISPLAY"
		printf 'CLIENT_PID=%s\n' "$CLIENT_PID"
		printf 'LOG=%s\n' "$LOG"
		do_light || true
		printf 'LIGHT=%s\n' "$(light_state_label)"
		do_capture
		info "client started (pid $CLIENT_PID, DISPLAY=$CLIENT_DISPLAY); leaving it running"
		exit 0
	fi

	stop_pid "${CLIENT_PID:-}"
	stop_screen_pid "${SCREEN_PID:-}"
	rm -f "$STATE_FILE"
	CLIENT_PID=
	SCREEN_PID=
	STARTED_SCREEN=0

	case "$result" in
	2)
		if [ "$attempt" -ge "$RETRIES" ]; then
			info "account still busy after $attempt attempts (see $LOG)"
			exit 5
		fi
		info "account is already logged in; waiting ${RETRY_DELAY}s before retrying"
		sleep "$RETRY_DELAY"
		attempt=$((attempt + 1))
		;;
	4)
		info "client exited before it was ready (see $LOG and $HARNESS_DIR)"
		exit 4
		;;
	*)
		info "timeout after ${TIMEOUT}s waiting for '$WAIT_FOR' (see $LOG)"
		exit 3
		;;
	esac
done
