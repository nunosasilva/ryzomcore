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
#   ENV="NLVK_STATS=1 RYZOM_VEG_STATE=1"
#                          Environment variables exported to the client
#                          (through the virtual screen). DISPLAY and
#                          RYZOM_HARNESS_* are reserved: the harness owns
#                          them, so they are rejected here.
#   GAMESCOPE_BACKEND=wayland|x11
#                          gamescope backend (default: wayland when
#                          WAYLAND_DISPLAY is set, otherwise x11).
#   HARNESS_DIR=./tmp/harness
#                          Where gamescope/Xvfb logs go. Never uses /tmp.
#   REUSE=1              Return the tracked client for this account/slot if it
#                          is still alive with the same executable and ENV; exit
#                          6 when there is none.
#   STOP=1               Stop only that tracked client, then exit 0.
#   DRYRUN=1             Print the resolved command line, change nothing.
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
ENV=${ENV:-}
REUSE=${REUSE:-}
STOP=${STOP:-}
DRYRUN=${DRYRUN:-}
GAMESCOPE_BACKEND=${GAMESCOPE_BACKEND:-}
HARNESS_DIR=${HARNESS_DIR:-}

for arg in "$@"; do
	case "$arg" in
	-h|--help) usage ;;
	DRIVER=*|SCREEN=*|CLIENT=*|ACCOUNT=*|PASSWORD=*|SLOT=*|RESOLUTION=*|\
	WAIT_FOR=*|LOG=*|TIMEOUT=*|RETRIES=*|RETRY_DELAY=*|EXTRA_ARGS=*|ENV=*|REUSE=*|STOP=*|DRYRUN=*|\
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

# Extra environment for the client: space separated VAR=value words. The
# harness owns DISPLAY and RYZOM_HARNESS_*, so they may not be overridden.
ENV_PASS=()
for word in $ENV; do
	[ "$word" = "${word#*=}" ] && die "ENV: '$word' has no VAR=value form"
	case "${word%%=*}" in
		DISPLAY|RYZOM_HARNESS_*) die "ENV: ${word%%=*} is reserved for the harness" ;;
	esac
	ENV_PASS+=("$word")
done

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
# Isolate the Vulkan pipeline cache per run dir **and per client build**:
# concurrent test clients, or a rebuilt binary in the same install, must not
# share (or race on) one blob. Size+mtime changes on every relink.
_CLIENT_TAG=$(stat -c '%s-%Y' "$CLIENT_ABS" 2>/dev/null | tr -d ' ' || true)
[ -n "$_CLIENT_TAG" ] || _CLIENT_TAG=dev
export NLVK_PIPELINE_CACHE_PATH=${NLVK_PIPELINE_CACHE_PATH:-$HARNESS_DIR/pipeline-cache-$_CLIENT_TAG.bin}
LOG=${LOG:-$PWD/client.log}
LOG=$(readlink -m "$LOG")

# ---------------------------------------------------------------------------
# Client command line, and the one client we own
# ---------------------------------------------------------------------------

CLIENT_ARGS=()
if [ -n "$ACCOUNT" ]; then
	CLIENT_ARGS+=("$ACCOUNT" "$PASSWORD")
	if [ -n "$SLOT" ]; then
		CLIENT_ARGS+=(slot "$SLOT")
	fi
fi
# shellcheck disable=SC2206  # word splitting of EXTRA_ARGS is intended
CLIENT_ARGS+=($EXTRA_ARGS)

# Each account/slot gets one tracked client. The token in its environment is
# what proves a pid is ours: we never look for ryzom_client* by name, and we
# never touch a client that was started some other way (notably --profile 0).
STATE_ID="$ACCOUNT-slot${SLOT:-none}"
STATE_FILE="$HARNESS_DIR/$STATE_ID.pid"
STATE_TAG="ryzom-harness-$STATE_ID"

# The tracked client, verified through /proc by our token. The token is unique
# per account/slot launch, so it alone proves the pid is ours; requiring the
# executable too would make STOP/REUSE depend on passing the same CLIENT= as the
# launch did, which is a trap (STOP reports 'nothing tracked' for a client the
# harness itself started).
find_tracked() {
	local pid
	[ -f "$STATE_FILE" ] || return 1
	read -r pid _ <"$STATE_FILE" 2>/dev/null
	[ -n "$pid" ] || return 1
	[ -r "/proc/$pid/environ" ] || return 1
	{ tr '\0' '\n' <"/proc/$pid/environ"; } 2>/dev/null | grep -qx "RYZOM_HARNESS_STATE=$STATE_TAG" || return 1
	echo "$pid"
}

# REUSE additionally requires the same executable: reusing a client that runs
# another binary would silently test the wrong build.
find_tracked_same_exe() {
	local pid exe
	pid=$(find_tracked) || return 1
	exe=$(readlink "/proc/$pid/exe" 2>/dev/null)
	[ "$exe" = "$CLIENT_ABS" ] || return 1
	echo "$pid"
}

stop_tracked() {
	local pid token p
	pid=$(find_tracked) || { info "nothing tracked for $STATE_ID"; return 0; }
	token=$(cut -d' ' -f2 "$STATE_FILE")
	# the virtual screen was started with the same token: stop it through /proc,
	# never by matching a process name.
	for p in /proc/[0-9]*; do
		[ -r "$p/environ" ] || continue
		{ tr '\0' '\n' <"$p/environ"; } 2>/dev/null | grep -qx "RYZOM_HARNESS_TOKEN=$token" || continue
		kill -TERM "${p#/proc/}" 2>/dev/null
	done
	for _ in $(seq 1 40); do
		[ -r "/proc/$pid" ] || break
		sleep 0.25
	done
	rm -f "$STATE_FILE"
	info "stopped the tracked client for $STATE_ID (pid $pid)"
}

if [ -n "$DRYRUN" ]; then
	printf 'DRYRUN: %s' "$CLIENT_ABS"; for a in "${CLIENT_ARGS[@]}"; do printf ' %s' "$a"; done; printf '\n'
	printf 'DRYRUN: env %s\n' "${ENV:-<none>}"
	printf 'DRYRUN: screen %s %sx%s, log %s, state %s\n' "$SCREEN" "$SCREEN_W" "$SCREEN_H" "$LOG" "$STATE_FILE"
	exit 0
fi

if [ -n "$STOP" ]; then
	stop_tracked
	exit 0
fi

if [ -n "$REUSE" ]; then
	if pid=$(find_tracked_same_exe); then
		CLIENT_PID=$pid
		SCREEN_DISPLAY=$(tr '\0' '\n' <"/proc/$pid/environ" 2>/dev/null | sed -n 's/^DISPLAY=//p' | head -1)
		printf 'DISPLAY=%s\nCLIENT_PID=%s\nLOG=%s\n' "${SCREEN_DISPLAY:-?}" "$CLIENT_PID" "$LOG"
		info "reusing the tracked client for $STATE_ID (pid $CLIENT_PID)"
		HARNESS_OK=1
		exit 0
	fi
	exit 6
fi

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
		            --force-windows-fullscreen)
		if [ "$SCREEN" = gamescope-headless ]; then
			args+=(--backend headless)
		else
			args+=(--backend "$backend")
		fi
		env ${ENV_PASS[@]+"${ENV_PASS[@]}"} RYZOM_HARNESS_STATE="$STATE_TAG" RYZOM_HARNESS_TOKEN="$TOKEN" \
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

# The client picks its own log path (there is no cfg key for it): it is always
# <install>/client.log, rotated to clientNNN.log. So LOG= only says what to *watch*;
# pointing it somewhere else watches a file that never appears, and two clients
# from one install dir fight over the same log. Refuse the second one, and after
# the client is up, watch the file it actually opened.
log_holders() {
	local p f target base
	# NeL renames the log it has open (client.log -> clientNNN.log / client.log.previous),
	# and a file descriptor follows the rename, so compare against the whole rotated
	# set of this install dir, not one exact path.
	base=$(dirname "$LOG")/${LOG##*/}
	base=${base%%.*}
	for p in /proc/[0-9]*; do
		[ "$p" = "/proc/$$" ] && continue
		[ -r "$p/fd" ] || continue
		for f in "$p"/fd/*; do
			target=$(readlink "$f" 2>/dev/null) || continue
			case "$target" in
			"$LOG"|"$LOG".*|"$base".log*)
				# Only a client blocks the launch. A log viewer (tail, grep, an
				# editor) holds the same file open without writing it, and watching
				# the log is a normal thing to do while testing.
				case "$(readlink "/proc/${p#/proc/}/exe" 2>/dev/null)" in
				*/ryzom_client*|ryzom_client*) echo "${p#/proc/}"; return 0 ;;
				esac ;;
			esac
		done
	done
	return 0
}

start_client() {
	local holder
	holder=$(log_holders)
	if [ -n "$holder" ]; then
		die "another live process (pid $holder) already writes '$LOG': two clients from one install dir share one log. Use another install dir, or REUSE=1 for the tracked one."
	fi
	[ -f "$LOG" ] && mv -f "$LOG" "$LOG.previous"

	case "$SCREEN" in
	gamescope|gamescope-headless) start_screen ;;
	xvfb)
		start_screen
		env ${ENV_PASS[@]+"${ENV_PASS[@]}"} RYZOM_HARNESS_STATE="$STATE_TAG" DISPLAY="$SCREEN_DISPLAY" RYZOM_HARNESS_TOKEN="$TOKEN" \
			"$CLIENT_ABS" "${CLIENT_ARGS[@]}" >>"$HARNESS_DIR/client.out" 2>&1 &
		CLIENT_PID=$!
		;;
	none)
		env ${ENV_PASS[@]+"${ENV_PASS[@]}"} RYZOM_HARNESS_STATE="$STATE_TAG" RYZOM_HARNESS_TOKEN="$TOKEN" "$CLIENT_ABS" "${CLIENT_ARGS[@]}" \
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
	adopt_log
}

# Adopt the log the client really opened. It rotates (client.log -> clientNNN.log),
# so this is also what tells us the file to tail after a rotation.
adopt_log() {
	local f target base
	# only the client's own log: the process also inherits stdout/stderr, which
	# under gamescope is gamescope.log, and that is not what we want to tail.
	base=$(dirname "$LOG")/${LOG##*/}
	base=${base%%.*}
	for f in /proc/"$CLIENT_PID"/fd/*; do
		target=$(readlink "$f" 2>/dev/null) || continue
		case "$target" in
		"$base".log*)
			if [ "$target" != "$LOG" ]; then
				info "watching the log the client opened: $target (LOG= said $LOG)"
				LOG=$target
			fi
			return 0
			;;
		esac
	done
	return 0
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
# The client rotates its own log (client.log -> clientNNN.log) while it starts up,
# and an opt-in verbose tag such as RYZOM_VEG_STATE=1 writes tens of thousands of
# lines, so the marker we wait for can land in the rotated file. Search the whole
# rotated set: they all belong to this run, the previous run went to LOG.previous.
log_files=()
collect_log_files() {
	local f
	log_files=()
	for f in "${LOG%.*}"*.log; do
		[ -f "$f" ] && log_files+=("$f")
	done
	return 0
}
log_grep() {
	[ "${#log_files[@]}" -gt 0 ] || return 1
	grep -aqiE "$1" "${log_files[@]}" 2>/dev/null
}

wait_for_client() {
	local deadline=$((SECONDS + TIMEOUT))
	local fail_re='already (logged|connected|in online state)|disconnected by the server|connection of ryzom failed|login failed'

	while [ "$SECONDS" -lt "$deadline" ]; do
		if ! kill -0 "$CLIENT_PID" 2>/dev/null; then
			return 4
		fi
		collect_log_files
		log_grep "$fail_re" && return 2
		case "$WAIT_FOR" in
		ingame)
			log_grep 'initInGame' && return 0
			;;
		login)
			if log_grep 'Connection of Ryzom success'; then
				# A busy account can disconnect right after the handshake.
				sleep 3
				log_grep "$fail_re" && return 2
				return 0
			fi
			;;
		window)
			if command -v xdotool >/dev/null 2>&1; then
				DISPLAY=$(client_display)
				[ -n "$DISPLAY" ] && [ -n "$(DISPLAY=$DISPLAY xdotool search --name Ryzom 2>/dev/null | head -1)" ] && return 0
			elif log_grep 'display created with'; then
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
		echo "$CLIENT_PID $TOKEN" >"$STATE_FILE"
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
