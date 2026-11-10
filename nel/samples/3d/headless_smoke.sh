#!/usr/bin/env bash
#
# Headless OpenGL3 smoke test for a NeL 3D sample (task 0.3).
#
# Runs the sample under Xvfb + the Mesa software renderer and fails if the sample
# dies early, if the driver logs an error, or if the captured frame is blank.
# SMOKE_MIN_COLOURS=<n> raises the unique-colour floor above 1, for samples that draw *some*
#   frame even when their subject failed to load (see the water sample's assets).
# The frame check is numeric (unique colour count), not a human look: a black
# window must fail the test, otherwise "it ran" would hide "it drew nothing".
#
#   usage: headless_smoke.sh <seconds> <sample-executable> [sample args...]
#   exit:  0 pass | 1 fail | 77 ctest "Not Available" (no headless GL stack here)
#
# Registered by nel/samples/3d/CMakeLists.txt as ctest name nel_gl3_smoke_<sample>.
#
# MULTI-ARCH: Unix-only by design (Xvfb, xwininfo, ImageMagick's X11 grab, Mesa's
# LIBGL_ALWAYS_SOFTWARE). Ryzom builds and runs on Windows, Mac and Linux; nothing
# in this script should be taken as a statement about those platforms, and the
# sample code it drives stays cross-platform. Off Unix the CMakeLists gate does not
# register these tests at all; the exit 77 below only covers a Unix box missing the
# X11/ImageMagick tools.

set -u

RUNSECS=${1:-8}; shift || true
BIN=${1:-}; shift || true
[ -n "$BIN" ] || { echo "usage: $0 <seconds> <executable> [args...]"; exit 2; }

for tool in Xvfb xwininfo import identify; do
  command -v "$tool" >/dev/null 2>&1 || { echo "SKIP: $tool not found"; exit 77; }
done

# Pick a display that is not in use, so parallel ctest jobs and the user's :0 are
# left alone.
for try in 1 2 3 4 5 6 7 8 9 10; do
  N=$(( 100 + RANDOM % 800 ))
  [ -e "/tmp/.X${N}-lock" ] || break
done
DISPLAY_NUM=$N
XVFB_LOG=$(mktemp /tmp/nl-smoke-xvfb.XXXXXX.log)
SAMPLE_LOG=$(mktemp /tmp/nl-smoke-sample.XXXXXX.log)
FRAME=$(mktemp /tmp/nl-smoke-frame.XXXXXX.png)

cleanup() {
  [ -n "${XV_PID:-}" ] && kill "$XV_PID" 2>/dev/null
  [ -n "${SP_PID:-}" ] && kill "$SP_PID" 2>/dev/null
  rm -f "$XVFB_LOG" "$SAMPLE_LOG" "$FRAME"
}
trap cleanup EXIT

Xvfb ":$DISPLAY_NUM" -screen 0 800x600x24 -nocursor >"$XVFB_LOG" 2>&1 &
XV_PID=$!
sleep 1
kill -0 "$XV_PID" 2>/dev/null || { echo "FAIL: Xvfb did not start"; cat "$XVFB_LOG"; exit 1; }

DISPLAY=":$DISPLAY_NUM" LIBGL_ALWAYS_SOFTWARE=1 "$BIN" "$@" >"$SAMPLE_LOG" 2>&1 &
SP_PID=$!

# Poll instead of sleeping blindly, and poll for CONTENT, not just for a window:
# the window is created before the mega programs finish linking, so capturing at
# first appearance gives an all-black frame and a *false* blank-frame failure, while
# a fixed sleep can miss the window outright on a loaded box. Each second we re-check
# liveness, find the window id once it exists, and re-capture until the frame has
# more than one unique colour (or the timeout expires).
#
# Tree children are the only lines that start with whitespace, so anchoring on
# indentation is what separates them from the root/parent header lines. (A digit-only
# regex like '0x[0-9]+ ' skips the root only by luck: its id is hex, e.g. 0x2cf, and
# would match outright as 0x64 on ~12% of runs, silently capturing the whole display.)
WIN=""
COLORS=0
# Capture only once the frame clears the sample's colour floor. Without this the
# loop stops at the first non-blank frame, which can be before a sample has
# finished loading its subject (the water sample shows its sky checker first and
# its water surface a few seconds later, so a "not blank" break is a race).
MIN_COLOURS=${SMOKE_MIN_COLOURS:-1}
for t in $(seq 1 "$RUNSECS"); do
  if ! kill -0 "$SP_PID" 2>/dev/null; then
    echo "FAIL: sample exited after ${t}s, before any frame could be captured"
    tail -25 "$SAMPLE_LOG"
    exit 1
  fi
  if [ -z "$WIN" ]; then
    WIN=$(DISPLAY=":$DISPLAY_NUM" xwininfo -root -tree 2>/dev/null \
         | grep -oE '^[[:space:]]+0x[0-9a-f]+ ' | tr -d ' ' | head -1)
  fi
  if [ -n "$WIN" ]; then
    DISPLAY=":$DISPLAY_NUM" import -window "$WIN" "$FRAME" 2>/dev/null \
      || DISPLAY=":$DISPLAY_NUM" import -window root "$FRAME" 2>/dev/null
    if [ -s "$FRAME" ]; then
      COLORS=$(identify -format "%k" "$FRAME" 2>/dev/null)
      [ "${COLORS:-0}" -gt "$MIN_COLOURS" ] && break
    fi
  fi
  sleep 1
done

if [ -z "$WIN" ]; then
  echo "FAIL: no sample window appeared within ${RUNSECS}s (shader link slower than the timeout?)"
  tail -25 "$SAMPLE_LOG"
  exit 1
fi

fail=0
# A frame proves it drew something at some point. It does not prove it survived:
# without this, a sample that segfaults one frame in still passes as long as it
# leaves no ERR/crash marker in the log. Only kill it if it is still alive; if it
# already exited on its own, that exit code is evidence and a non-zero one fails.
SP_RC=0
if kill -0 "$SP_PID" 2>/dev/null; then
  kill "$SP_PID" 2>/dev/null
else
  wait "$SP_PID" 2>/dev/null; SP_RC=$?
  [ "$SP_RC" -ne 0 ] && echo "FAIL: sample exited on its own (rc=$SP_RC) after the first frame" && fail=1
fi
sleep 0.3

# 0) the frame must have been drawn by the driver this gate is about. Without it the
#    test is a rubber stamp: a sample that silently fell back to the legacy OpenGL
#    driver would pass while covering zero GL3 code, and the "only GL3-selecting
#    samples are registered" list in CMakeLists.txt is a manual list, not a check.
#    "3D: Light table size:" is logged by driver_opengl3.cpp:455 and by no other driver,
#    so it proves the GL3 driver was created and got through init.
if ! grep -aq "3D: Light table size:" "$SAMPLE_LOG"; then
  echo "FAIL: no OpenGL3 driver banner in the log - the sample did not render under GL3"
  fail=1
fi

# 1) driver / NeL errors and crash markers. NeL's tags are ERR WRN INF DBG STT AST UKN
#    (displayer.cpp:59) - there is no FATAL tag, and a failed nlAssert logs AST, so
#    AST is included and FATAL was dead weight. Mesa's own GLSL errors go to stderr
#    as "ERROR: ..." with no NeL tag, so they need their own pattern.
if grep -aqE '(ERR|AST) [0-9a-f]{6,} ' "$SAMPLE_LOG"; then
  echo "FAIL: driver logged errors or assertions:"
  grep -aE '(ERR|AST) [0-9a-f]{6,} ' "$SAMPLE_LOG" | head -10
  fail=1
fi
if grep -aqE '^ERROR(:| \[)' "$SAMPLE_LOG"; then
  echo "FAIL: raw GL/GLSL errors on stderr:"
  grep -aE '^ERROR(:| \[)' "$SAMPLE_LOG" | head -10
  fail=1
fi
if grep -aqE 'User Crash Callback|Dumping call stack|Segmentation fault|Aborted' "$SAMPLE_LOG"; then
  echo "FAIL: crash marker in the log"
  fail=1
fi

# 2) the frame must actually contain something. %k = unique colour count;
#     a blank window gives 1.
if [ -s "$FRAME" ]; then
  COLORS=$(identify -format "%k" "$FRAME" 2>/dev/null)
  SIZE=$(identify -format "%wx%h" "$FRAME" 2>/dev/null)
  echo "frame: $SIZE, unique colours: $COLORS"
  # SMOKE_MIN_COLOURS raises the bar above "not blank" for samples whose point is a specific
  # surface: nl_sample_water draws its sky checker and cube even when the water shape failed to
  # load (32 unique colours) and draws the wave-distorted surface when it did (849), so a floor
  # of ~100 is what makes it a water test rather than a "did it crash" test. Default 1 keeps
  # every other sample exactly as strict as it was. The poll above uses the same floor, so
  # the timeout expires while the subject is missing rather than capturing too early.
  if [ -z "${COLORS:-}" ] || [ "${COLORS:-0}" -le "$MIN_COLOURS" ]; then
    if [ "$MIN_COLOURS" -le 1 ]; then
      echo "FAIL: captured frame is blank ($COLORS unique colours)"
    else
      echo "FAIL: captured frame has $COLORS unique colours, need more than $MIN_COLOURS"
    fi
    fail=1
  fi
else
  echo "FAIL: no frame captured"
  fail=1
fi

if [ "$fail" -ne 0 ]; then
  echo "--- last 25 log lines ---"; tail -25 "$SAMPLE_LOG"
  exit 1
fi

echo "PASS: $(basename "$BIN") rendered under Xvfb for ${RUNSECS}s with no driver error"
exit 0
