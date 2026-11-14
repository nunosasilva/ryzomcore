#!/usr/bin/env bash
#
# driver-start-ryzom_client.sh - start a Ryzom client for automated tests.
#
# Run it from the root of a client installation:
#
#     DRIVER=Vulkan ACCOUNT=username321 PASSWORD=password123 SLOT=0 \
#         ./driver-start-ryzom_client.sh
#
# It optionally rewrites client.cfg, starts the client in a virtual screen
# (gamescope by default), waits until the client is in game, then prints the
# DISPLAY the client is running on and exits 0, leaving everything running.
# If the server reports that the account is already logged in (a killed client
# keeps it for the network timeout), it kills the client, waits RETRY_DELAY
# seconds and starts another one.
#
# All parameters are KEY=VALUE, given as arguments or in the environment:
#
#   DRIVER=Auto|OpenGL|OpenGL3|Vulkan|DirectX
#                          Rewrite Driver3D in client.cfg (both occurrences).
#                          Empty or "keep" leaves the file untouched.
#   SCREEN=gamescope|gamescope-headless|xvfb|none
#                          Virtual screen to run in (default: gamescope).
#                          "none" uses the current DISPLAY.
#   CLIENT=./ryzom_client_vulkan_opt
#                          Client executable (absolute or relative to cwd).
#   ACCOUNT, PASSWORD, SLOT
#                          Login parameters. Empty ACCOUNT shows the login
#                          screen (WAIT_FOR then defaults to "window").
#   RESOLUTION=1280x800    Screen size (the UI needs at least 1024x800).
#   WAIT_FOR=ingame|login|window
#                          What counts as started (default: ingame).
#   LOG=./client.log       Client log to watch.
#   TIMEOUT=180            Seconds to wait for WAIT_FOR before giving up.
#   RETRIES=3              Attempts when the account is already logged in.
#   RETRY_DELAY=40         Seconds between attempts.
#   EXTRA_ARGS="--nopatch 1"
#                          Extra client arguments.
#   GAMESCOPE_BACKEND=wayland|x11
#                          gamescope backend (default: wayland when
#                          WAYLAND_DISPLAY is set, otherwise x11).
#   HARNESS_DIR=./tmp/harness
#                          Where gamescope/Xvfb logs go. Never uses /tmp.
#
# On success it prints three KEY=VALUE lines and exits 0:
#
#   DISPLAY=:3
#   CLIENT_PID=12345
#   LOG=/path/to/client.log
#
# Exit codes: 0 started, 2 usage/config error, 3 timeout, 4 client died,
# 5 no attempt left because the account stayed busy.
#
# Examples:
#   # Login screen, OpenGL, 1280x800
#   DRIVER=OpenGL SCREEN=gamescope ./driver-start-ryzom_client.sh
#
#   # In game with a specific account, Vulkan, then use $DISPLAY with xdotool
#   eval "$(DRIVER=Vulkan ACCOUNT=username321 PASSWORD=password123 SLOT=0 \
#            ./driver-start-ryzom_client.sh)"
#
set -uo pipefail

PROG=${0##*/}

die()  { echo "$PROG: $*" >&2; exit 2; }
info() { echo "$PROG: $*" >&2; }

usage() { sed -n '2,60p' "$0" | sed 's/^# \{0,1\}//'; exit 0; }

# ---------------------------------------------------------------------------
# Parameters
# ---------------------------------------------------------------------------

DRIVER=${DRIVER:-}
SCREEN=${SCREEN:-gamescope}
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
GAMESCOPE_BACKEND=${GAMESCOPE_BACKEND:-}
HARNESS_DIR=${HARNESS_DIR:-}

for arg in "$@"; do
	case "$arg" in
	-h|--help) usage ;;
	DRIVER=*|SCREEN=*|CLIENT=*|ACCOUNT=*|PASSWORD=*|SLOT=*|RESOLUTION=*|\
	WAIT_FOR=*|LOG=*|TIMEOUT=*|RETRIES=*|RETRY_DELAY=*|EXTRA_ARGS=*|\
	GAMESCOPE_BACKEND=*|HARNESS_DIR=*)
		key=${arg%%=*}
		value=${arg#*=}
		printf -v "$key" '%s' "$value"
		;;
	*)
		die "unknown argument '$arg' (use KEY=VALUE or --help)"
		;;
	esac
done

[ -z "$WAIT_FOR" ] && { [ -n "$ACCOUNT" ] && WAIT_FOR=ingame || WAIT_FOR=window; }
case "$WAIT_FOR" in ingame|login|window) ;; *) die "WAIT_FOR must be ingame, login or window";; esac
case "$SCREEN" in gamescope|gamescope-headless|xvfb|none) ;; *) die "SCREEN must be gamescope, gamescope-headless, xvfb or none";; esac

case "$RESOLUTION" in
	*x*) SCREEN_W=${RESOLUTION%x*}; SCREEN_H=${RESOLUTION#*x} ;;
	*) die "RESOLUTION must be WIDTHxHEIGHT, e.g. 1280x800" ;;
esac
[[ "$SCREEN_W" =~ ^[0-9]+$ && "$SCREEN_H" =~ ^[0-9]+$ ]] || die "bad RESOLUTION '$RESOLUTION'"

case "$TIMEOUT" in ''|*[!0-9]*) die "TIMEOUT must be a number";; esac
case "$RETRIES" in ''|*[!0-9]*) die "RETRIES must be a number";; esac
case "$RETRY_DELAY" in ''|*[!0-9]*) die "RETRY_DELAY must be a number";; esac

[ -x "$CLIENT" ] || die "client '$CLIENT' not found or not executable (set CLIENT=...)"
CLIENT_ABS=$(readlink -f "$CLIENT") || die "cannot resolve '$CLIENT'"

HARNESS_DIR=${HARNESS_DIR:-$PWD/tmp/harness}
mkdir -p "$HARNESS_DIR" || die "cannot create '$HARNESS_DIR'"
LOG=${LOG:-$PWD/client.log}
LOG=$(readlink -m "$LOG")

# ---------------------------------------------------------------------------
# client.cfg
# ---------------------------------------------------------------------------

if [ -n "$DRIVER" ] && [ "$DRIVER" != keep ]; then
	case "$DRIVER" in Auto|OpenGL|OpenGL3|Vulkan|DirectX) ;; *)
		die "DRIVER must be Auto, OpenGL, OpenGL3, Vulkan or DirectX" ;;
	esac
	[ -f client.cfg ] || die "client.cfg not found in $PWD"
	[ -f client.cfg.harness-backup ] || cp -p client.cfg client.cfg.harness-backup
	sed -i -E "s|^([[:space:]]*Driver3D[[:space:]]*=[[:space:]]*\")[^\"]*(\";)|\1${DRIVER}\2|" client.cfg
	grep -qE "^[[:space:]]*Driver3D[[:space:]]*=[[:space:]]*\"${DRIVER}\";" client.cfg \
		|| die "could not set Driver3D=\"$DRIVER\" in client.cfg"
	info "client.cfg: Driver3D=\"$DRIVER\" (backup in client.cfg.harness-backup)"
fi

# ---------------------------------------------------------------------------
# State
# ---------------------------------------------------------------------------

TOKEN="ryzom-harness-$$-$RANDOM"
SCREEN_PID=
CLIENT_PID=
SCREEN_DISPLAY=
STARTED_SCREEN=0
HARNESS_OK=0

stop_client() {
	[ -n "$CLIENT_PID" ] || return 0
	if kill -0 "$CLIENT_PID" 2>/dev/null; then
		kill "$CLIENT_PID" 2>/dev/null
		for _ in $(seq 1 20); do
			kill -0 "$CLIENT_PID" 2>/dev/null || break
			sleep 0.25
		done
		kill -9 "$CLIENT_PID" 2>/dev/null
	fi
	CLIENT_PID=
}

stop_screen() {
	[ "$STARTED_SCREEN" = 1 ] || return 0
	[ -n "$SCREEN_PID" ] || return 0
	if kill -0 "$SCREEN_PID" 2>/dev/null; then
		kill "$SCREEN_PID" 2>/dev/null
		for _ in $(seq 1 20); do
			kill -0 "$SCREEN_PID" 2>/dev/null || break
			sleep 0.25
		done
		kill -9 "$SCREEN_PID" 2>/dev/null
	fi
	SCREEN_PID=
	STARTED_SCREEN=0
}

cleanup() {
	local rc=$?
	if [ "$HARNESS_OK" != 1 ]; then
		stop_client
		stop_screen
	fi
	exit "$rc"
}
trap cleanup EXIT INT TERM

# ---------------------------------------------------------------------------
# Virtual screen
# ---------------------------------------------------------------------------

start_screen() {
	case "$SCREEN" in
	gamescope|gamescope-headless)
		local backend=$GAMESCOPE_BACKEND
		if [ -z "$backend" ]; then
			[ -n "${WAYLAND_DISPLAY:-}" ] && backend=wayland || backend=x11
		fi
		local args=(-W "$SCREEN_W" -H "$SCREEN_H" -w "$SCREEN_W" -h "$SCREEN_H"
		            --force-windows-fullscreen -r 60)
		if [ "$SCREEN" = gamescope-headless ]; then
			args+=(--backend headless)
		else
			args+=(--backend "$backend")
		fi
		env RYZOM_HARNESS_TOKEN="$TOKEN" \
			gamescope "${args[@]}" -- "$CLIENT_ABS" "${CLIENT_ARGS[@]}" \
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
		Xvfb "$SCREEN_DISPLAY" -screen 0 "${SCREEN_W}x${SCREEN_H}x24" -nolisten tcp \
			>>"$HARNESS_DIR/xvfb.log" 2>&1 &
		SCREEN_PID=$!
		STARTED_SCREEN=1
		for _ in $(seq 1 40); do
			[ -e "/tmp/.X11-unix/X$n" ] && break
			sleep 0.25
		done
		;;
	none)
		[ -n "${DISPLAY:-}" ] || die "SCREEN=none needs DISPLAY to be set"
		;;
	esac
}

# Find the client process by the environment token given to its screen.
find_client_pid() {
	local p pid exe
	for p in /proc/[0-9]*; do
		pid=${p#/proc/}
		[ "$pid" = "$$" ] && continue
		[ -r "$p/environ" ] || continue
		{ tr '\0' '\n' <"$p/environ"; } 2>/dev/null | grep -qx "RYZOM_HARNESS_TOKEN=$TOKEN" || continue
		exe=$(readlink "$p/exe" 2>/dev/null) || continue
		[ "$exe" = "$CLIENT_ABS" ] && { echo "$pid"; return 0; }
	done
	return 1
}

start_client() {
	CLIENT_ARGS=()
	if [ -n "$ACCOUNT" ]; then
		CLIENT_ARGS+=("$ACCOUNT" "$PASSWORD")
		if [ -n "$SLOT" ]; then
			CLIENT_ARGS+=(slot "$SLOT")
		fi
	fi
	# shellcheck disable=SC2206  # word splitting of EXTRA_ARGS is intended
	CLIENT_ARGS+=($EXTRA_ARGS)

	[ -f "$LOG" ] && mv -f "$LOG" "$LOG.previous"

	case "$SCREEN" in
	gamescope|gamescope-headless) start_screen ;;
	xvfb)
		start_screen
		env DISPLAY="$SCREEN_DISPLAY" RYZOM_HARNESS_TOKEN="$TOKEN" \
			"$CLIENT_ABS" "${CLIENT_ARGS[@]}" >>"$HARNESS_DIR/client.out" 2>&1 &
		CLIENT_PID=$!
		;;
	none)
		env RYZOM_HARNESS_TOKEN="$TOKEN" "$CLIENT_ABS" "${CLIENT_ARGS[@]}" \
			>>"$HARNESS_DIR/client.out" 2>&1 &
		CLIENT_PID=$!
		;;
	esac

	# gamescope starts the client itself; find it through the token.
	if [ -z "${CLIENT_PID:-}" ]; then
		for _ in $(seq 1 80); do
			CLIENT_PID=$(find_client_pid) && break
			sleep 0.25
		done
		[ -n "${CLIENT_PID:-}" ] || die "client did not start in the virtual screen (see $HARNESS_DIR/gamescope.log)"
	fi
}

client_display() {
	if [ -n "$SCREEN_DISPLAY" ]; then
		echo "$SCREEN_DISPLAY"
		return 0
	fi
	tr '\0' '\n' <"/proc/$CLIENT_PID/environ" 2>/dev/null | sed -n 's/^DISPLAY=//p' | head -1
}

# ---------------------------------------------------------------------------
# Log monitoring
# ---------------------------------------------------------------------------

# Returns 0 started, 2 account busy (retry), 3 timeout, 4 client died.
wait_for_client() {
	local deadline=$((SECONDS + TIMEOUT))
	local fail_re='already (logged|connected|in online state)|disconnected by the server|connection of ryzom failed|login failed'

	while [ "$SECONDS" -lt "$deadline" ]; do
		if ! kill -0 "$CLIENT_PID" 2>/dev/null; then
			return 4
		fi
		if [ -f "$LOG" ] && grep -aqiE "$fail_re" "$LOG"; then
			return 2
		fi
		case "$WAIT_FOR" in
		ingame)
			grep -aq 'initInGame' "$LOG" 2>/dev/null && return 0
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
				DISPLAY=$(client_display)
				[ -n "$DISPLAY" ] && [ -n "$(DISPLAY=$DISPLAY xdotool search --name Ryzom 2>/dev/null | head -1)" ] && return 0
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
# Main
# ---------------------------------------------------------------------------

attempt=1
while :; do
	info "attempt $attempt/$RETRIES: $CLIENT ($SCREEN, Driver3D=${DRIVER:-unchanged}, WAIT_FOR=$WAIT_FOR)"

	start_client

	wait_for_client
	result=$?

	if [ "$result" -eq 0 ]; then
		display=$(client_display)
		[ -n "$display" ] || die "started but no DISPLAY found for pid $CLIENT_PID"
		HARNESS_OK=1
		echo "DISPLAY=$display"
		echo "CLIENT_PID=$CLIENT_PID"
		echo "LOG=$LOG"
		info "client started (pid $CLIENT_PID, DISPLAY=$display); leaving it running"
		exit 0
	fi

	stop_client
	stop_screen

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
