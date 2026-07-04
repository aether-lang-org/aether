#!/bin/sh
# Issue #1006: an actor (and its message types) declared in an IMPORTED module
# could not be spawned/sent to from the importing module. It failed at the call
# site with a misleading "Undefined function 'spawn_Worker'" and "Undefined
# message type 'Ping'", because the module merge cloned functions/structs/
# consts from imported modules but skipped actor and message declarations.
#
# The merge now pulls imported-module actor + message declarations into the
# program under their bare name (like structs), so spawn(Worker()) and
# `w ! Ping { ... }` resolve and run.
#
# The ${n} interpolation of the single-int message field additionally guards
# the %d-vs-intptr_t format warning fix: that field is stored in the intptr_t
# payload slot, so the handler now binds it as a real `int`.
#
# Fixture: lib/worker.ae declares `message Ping { n: int }` + `actor Worker`;
# main.ae imports it, spawns the actor, sends two Pings, waits for idle.
# Acceptance: compiles without warnings, runs, prints "ping 42", "ping 7",
# "DONE".

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
AE="$ROOT/build/ae"

out=$( cd "$SCRIPT_DIR" && AETHER_HOME="" "$AE" run main.ae 2>&1 )
rc=$?
if [ "$rc" -ne 0 ]; then
    echo "  [FAIL] cross_module_actor: program errored (rc=$rc)"
    echo "$out" | head -20 | sed 's/^/          /'
    exit 1
fi

# On a clean compile a codegen format warning would surface here.
if printf '%s\n' "$out" | grep -qi 'warning'; then
    echo "  [FAIL] cross_module_actor: compiler emitted a warning"
    echo "$out" | head -20 | sed 's/^/          /'
    exit 1
fi

got=$(printf '%s\n' "$out" | grep -v '^[[:space:]]*$' | tr '\n' ' ' | sed 's/ *$//')
if [ "$got" != "ping 42 ping 7 DONE" ]; then
    echo "  [FAIL] cross_module_actor: expected 'ping 42 ping 7 DONE', got '$got'"
    echo "$out" | head -20 | sed 's/^/          /'
    exit 1
fi

echo "  [PASS] cross_module_actor: imported-module actor + message spawn/send resolves and runs"
exit 0
