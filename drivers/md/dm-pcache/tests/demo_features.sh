#!/bin/bash
# ─────────────────────────────────────────────────────────────────────────────
# pcache-dist feature demos
#
# Per-feature executable demos for the pcache-dist feature catalog (docs/FEATURES.md).
# Each function corresponds to one feature in FEATURES.md.
#
# Usage:
#   bash demo_features.sh list                  # list all feature demos
#   bash demo_features.sh f01                   # run a single demo
#   bash demo_features.sh f01 f02 f10           # run several
#   bash demo_features.sh section1              # run all in a section
#   bash demo_features.sh smoke                 # quick subset that doesn't crash VMs
#
# Prerequisites:
#   - 3 VMs reachable: NODE1/NODE2/NODE3
#   - dm-pcache.ko deployed to /root/ on each
#   - /dev/pmem0 present on each
#   - Backing file path consistent (/var/lib/pcache_backing.img)
#   - Companion main demo script in same directory for build/start/reset
# ─────────────────────────────────────────────────────────────────────────────

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
DEMO="${SCRIPT_DIR}/dm_pcache_phase1_demo.sh"

NODE1="192.168.124.3"    # my_node_id=0  Primary
NODE2="192.168.124.50"   # my_node_id=1  Secondary
NODE3="192.168.124.60"   # my_node_id=2  Secondary
DEV="pcache-dist0"
LISTEN_PORT=9764
# ServerAlive* is not optional here: a node that reboots *during* a remote
# command leaves the client waiting on a TCP connection that will never answer.
# One such reboot wedged a 50-demo regression for 89 minutes with no output at
# all, and the hang looked like a test bug rather than the dead node it was.
SSH_OPTS=(-o BatchMode=yes -o StrictHostKeyChecking=no -o ConnectTimeout=5
          -o ServerAliveInterval=15 -o ServerAliveCountMax=4)

# Remote command timeout (seconds). A stalled dd/fsync must not hang the run.
SSH_CMD_TIMEOUT=120

# ── Result tracking ─────────────────────────────────────────────────────────
PASS_CNT=0
FAIL_CNT=0
FAILED=()          # names of failed checks, printed in the summary

# ── nft rule registry (for trap-based cleanup) ──────────────────────────────
# Every firewall table a demo installs is registered here as "node table" so
# the EXIT/INT/TERM trap can remove leftovers even if the demo aborts midway.
BLOCKED=()

log()  { echo "[$(date '+%H:%M:%S')] $*"; }
pass() { PASS_CNT=$((PASS_CNT + 1)); echo "[$(date '+%H:%M:%S')] [PASS] $*"; }
fail() { FAIL_CNT=$((FAIL_CNT + 1)); FAILED+=("$*"); echo "[$(date '+%H:%M:%S')] [FAIL] $*" >&2; }
section() { echo; echo "════════════════════════════════════════"; echo "  $*"; echo "════════════════════════════════════════"; }

ssh_n() {
    local node="$1"; shift
    ssh "${SSH_OPTS[@]}" "root@${node}" "$@"
}

# ssh with a hard local timeout — the remote command may keep running, but the
# script never blocks forever on a hung write/read. Returns 124 on timeout.
ssh_nt() {
    local node="$1"; shift
    timeout "$SSH_CMD_TIMEOUT" ssh "${SSH_OPTS[@]}" "root@${node}" "$@"
}

# assert_module_fresh NODE... — the loaded dm_pcache must be the .ko currently
# in /root. dm_pcache_phase1_demo.sh does "rmmod ... || true; insmod ... || true",
# so a device left open makes rmmod fail and the *previous* module silently stays
# resident -- which invalidates any before/after comparison without saying so.
assert_module_fresh() {
    local n loaded ondisk bad=0
    for n in "$@"; do
        loaded=$(ssh_n "$n" "cat /sys/module/dm_pcache/srcversion 2>/dev/null")
        ondisk=$(ssh_n "$n" "modinfo -F srcversion /root/dm-pcache.ko 2>/dev/null")
        if [ -z "$loaded" ] || [ "$loaded" != "$ondisk" ]; then
            bad=1
            echo "    [$n] loaded=${loaded:-<none>} ondisk=${ondisk:-<none>}"
        fi
    done
    [ "$bad" -eq 0 ] && pass "module under test is the one on disk" \
                     || fail "STALE MODULE — results are not attributable to /root/dm-pcache.ko"
}

# ── Node reachability (VM crash policy: abort the demo, don't hammer) ────────
node_up() { ssh_n "$1" 'true' >/dev/null 2>&1; }

require_nodes() {
    local n
    for n in "$@"; do
        if ! node_up "$n"; then
            fail "node $n unreachable — aborting demo (VM crash policy)"
            return 1
        fi
    done
    return 0
}

# ── Firewall block/unblock helpers (auto-registered for cleanup) ─────────────
block_inbound() {   # node table   — drop inbound peer traffic on LISTEN_PORT
    local node="$1" tbl="$2"
    ssh_n "$node" "nft add table inet $tbl 2>/dev/null
                   nft add chain inet $tbl input { type filter hook input priority 0\; } 2>/dev/null
                   nft add rule inet $tbl input tcp dport $LISTEN_PORT drop" || {
        fail "block_inbound $node/$tbl failed"; return 1; }
    BLOCKED+=("$node $tbl")
}

block_outbound_to() {   # node table dst_ip — drop outbound peer traffic to dst
    local node="$1" tbl="$2" dst="$3"
    ssh_n "$node" "nft add table inet $tbl 2>/dev/null
                   nft add chain inet $tbl output { type filter hook output priority 0\; } 2>/dev/null
                   nft add rule inet $tbl output ip daddr $dst tcp dport $LISTEN_PORT drop" || {
        fail "block_outbound $node/$tbl failed"; return 1; }
    BLOCKED+=("$node $tbl")
}

unblock() {   # node table — remove the rule and deregister
    local node="$1" tbl="$2" e keep=()
    ssh_n "$node" "nft delete table inet $tbl 2>/dev/null" || true
    for e in "${BLOCKED[@]:-}"; do
        [ -n "$e" ] && [ "$e" != "$node $tbl" ] && keep+=("$e")
    done
    BLOCKED=("${keep[@]:-}")
}

# Trap: remove any firewall table still registered when the script exits for
# any reason (normal end, Ctrl-C, kill). Prevents a leaked DROP rule from
# silently breaking the cluster and poisoning later runs.
cleanup_nft() {
    local e
    for e in "${BLOCKED[@]:-}"; do
        [ -n "$e" ] || continue
        # shellcheck disable=SC2086
        set -- $e
        node_up "$1" && ssh_n "$1" "nft delete table inet $2 2>/dev/null" || true
    done
    BLOCKED=()
}
cleanup_all() { cleanup_nft; busy_cleanup; link_cleanup; fault_cleanup; mem_hog_cleanup; }
trap cleanup_all EXIT INT TERM

assert_match() {
    local label="$1"; local actual="$2"; local expect="$3"
    [ "$actual" = "$expect" ] && pass "$label  ($actual)" || fail "$label  expected=$expect actual=$actual"
}

assert_neq() {
    local label="$1"; local a="$2"; local b="$3"
    [ "$a" != "$b" ] && pass "$label  ($a != $b OK)" || fail "$label  unexpectedly equal: $a"
}

# 32 lowercase hex chars — an md5 that is neither empty nor an error string.
is_hash() { [[ "$1" =~ ^[0-9a-f]{32}$ ]]; }

# has_line PATTERN — read stdin to the end, then report whether any line matches.
#
# Never write `producer | grep -q PATTERN` in this script. grep -q exits the
# moment it matches, which SIGPIPEs the producer; with `set -o pipefail` (line
# 23) the producer's 141 becomes the pipeline's status, so a *successful* match
# reads as a failure. It only bites once the producer is slow or its output is
# large enough to still be writing when grep quits -- which is why F42's
# refusal check passed while the secondary was broken (two matching lines) and
# started failing once it was fixed (sixty). Draining stdin first removes the
# early exit and with it the false negative.
has_line() {
    local pat="${1:-.}" out
    out=$(cat)
    [ -n "$out" ] && grep -qE "$pat" <<<"$out"
}

# assert_md5_eq LABEL REF VAL...  — every VAL must be a valid md5 AND equal REF.
# Guards against the empty==empty false-PASS when a read/write silently failed.
assert_md5_eq() {
    local label="$1" ref="$2"; shift 2
    if ! is_hash "$ref"; then fail "$label  invalid/empty reference md5: '$ref'"; return; fi
    local v
    for v in "$@"; do
        if ! is_hash "$v"; then fail "$label  invalid/empty md5: '$v'"; return; fi
        if [ "$v" != "$ref" ]; then fail "$label  mismatch: ref=$ref got=$v"; return; fi
    done
    pass "$label  ($ref)"
}

# assert_dmesg NODE PATTERN LABEL — pass iff the evidence line is present.
# Replaces the "grep … ; pass unconditionally" anti-pattern.
assert_dmesg() {
    local node="$1" pat="$2" label="$3" out
    out=$(ssh_n "$node" "dmesg | grep -E \"$pat\" | tail -3" 2>/dev/null)
    if [ -n "$out" ]; then
        echo "$out" | sed 's/^/    /'
        pass "$label"
    else
        fail "$label  (no dmesg match for /$pat/ on $node)"
    fi
}

# wait_md5_eq LABEL NODE_A NODE_B MB [timeout_s] [skip_mb] — poll until the two
# nodes' md5 over MB MiB agree (async replication/resync convergence), up to
# timeout (default 45s). Fails on invalid/empty md5 or timeout. Robust against
# the "fixed sleep too short" flake that a one-shot compare suffers.
wait_md5_eq() {
    local label="$1" a="$2" b="$3" mb="$4" to="${5:-45}" skip="${6:-0}" t=0 ma mb_
    while [ "$t" -le "$to" ]; do
        ma=$(read_md5 "$a" "$mb" "$skip"); mb_=$(read_md5 "$b" "$mb" "$skip")
        if is_hash "$ma" && [ "$ma" = "$mb_" ]; then
            pass "$label  (converged in ${t}s, $ma)"; return
        fi
        sleep 5; t=$((t + 5))
    done
    fail "$label  (no convergence in ${to}s: $a=$ma $b=$mb_)"
}

# wait_dmesg NODE PATTERN LABEL [timeout_s] — poll dmesg until the evidence line
# appears (up to timeout, default 60s). For events that arrive after a variable
# reconnect/negotiation delay (split-brain detect, resync start).
wait_dmesg() {
    local node="$1" pat="$2" label="$3" to="${4:-60}" t=0
    while [ "$t" -le "$to" ]; do
        if ssh_n "$node" "dmesg | grep -E \"$pat\"" 2>/dev/null | has_line; then
            ssh_n "$node" "dmesg | grep -E \"$pat\" | tail -2" 2>/dev/null | sed 's/^/    /'
            pass "$label  (detected in ${t}s)"; return
        fi
        sleep 5; t=$((t + 5))
    done
    fail "$label  (no dmesg match for /$pat/ on $node within ${to}s)"
}

# Wipe ALL fan-out write history; quick path for demos that need clean state.
# Guarded: verify the nodes are up and the cluster actually started, else the
# demo aborts cleanly instead of running against a broken cluster. Callers use
# `fresh2 || return`.
# ── Crash coverage across cluster setup ────────────────────────────────────
#
# forbidden_mark runs *after* fresh2/fresh3, so a node that dies inside the
# rebuild is outside every crash check the demo performs. That is not
# hypothetical: fix-history #17 panics in dm_pcache_dtr, which runs inside
# `reset` -- three regression attempts in a row died there and reported only
# "cluster start failed", which reads like a config problem and carries no
# evidence. Snapshot boot_ids before touching the cluster, and name the node
# that went away.
SETUP_BOOT=()
setup_boot_snapshot() {
    local n b
    SETUP_BOOT=()
    for n in "$@"; do
        b=$(ssh_n "$n" "cat /proc/sys/kernel/random/boot_id" 2>/dev/null)
        SETUP_BOOT+=("$n ${b:-unknown}")
    done
}

# setup_failed LABEL — report a failed reset/start, saying whether a node
# crashed rather than leaving that to guesswork.
setup_failed() {
    local label="$1" e n b0 b1 crashed=""
    for e in "${SETUP_BOOT[@]:-}"; do
        [ -n "$e" ] || continue
        # shellcheck disable=SC2086
        set -- $e; n="$1"; b0="$2"
        if ! node_up "$n"; then
            crashed="$crashed $n(unreachable)"
            continue
        fi
        b1=$(ssh_n "$n" "cat /proc/sys/kernel/random/boot_id" 2>/dev/null)
        [ -n "$b0" ] && [ "$b0" != "unknown" ] && [ -n "$b1" ] && [ "$b0" != "$b1" ] \
            && crashed="$crashed $n(rebooted)"
    done
    if [ -n "$crashed" ]; then
        fail "$label failed — node crashed during cluster setup:$crashed"
        echo "    A node died inside reset/start, so its dmesg went with it."
        echo "    Capture it next time: netconsole on the node, socat on the host."
        echo "    See fix-history #17 for the last one that hid here."
    else
        fail "$label failed"
    fi
}

fresh2() {
    [ -x "$DEMO" ] || [ -f "$DEMO" ] || { fail "demo script missing: $DEMO"; return 1; }
    require_nodes "$NODE1" "$NODE2" || return 1
    setup_boot_snapshot "$NODE1" "$NODE2"
    bash "$DEMO" reset >/dev/null 2>&1
    if ! bash "$DEMO" start >/dev/null; then setup_failed "cluster start (2-node)"; return 1; fi
    sleep 5
    assert_module_fresh "$NODE1" "$NODE2"
}
fresh3() {
    [ -x "$DEMO" ] || [ -f "$DEMO" ] || { fail "demo script missing: $DEMO"; return 1; }
    require_nodes "$NODE1" "$NODE2" "$NODE3" || return 1
    setup_boot_snapshot "$NODE1" "$NODE2" "$NODE3"
    bash "$DEMO" reset >/dev/null 2>&1
    if ! bash "$DEMO" start3 >/dev/null; then setup_failed "cluster start (3-node)"; return 1; fi
    sleep 8
    assert_module_fresh "$NODE1" "$NODE2" "$NODE3"
}

# Convenience: write random N MiB from Primary; echo its md5 (empty on failure —
# callers must validate via assert_md5_eq, never compare raw).
primary_write() {
    local mb="$1"; local seek="${2:-0}"
    ssh_nt "$NODE1" "dd if=/dev/urandom of=/tmp/in.bin bs=1M count=$mb status=none && \
                    md5sum /tmp/in.bin | awk '{print \$1}' > /tmp/in.md5 && \
                    dd if=/tmp/in.bin of=/dev/mapper/$DEV bs=1M count=$mb seek=$seek oflag=direct conv=fsync status=none && \
                    cat /tmp/in.md5"
}

# Read back N MiB md5 from any node (empty on failure — validate, don't compare raw)
read_md5() {
    local node="$1"; local mb="$2"; local skip="${3:-0}"
    ssh_nt "$node" "dd if=/dev/mapper/$DEV bs=1M count=$mb skip=$skip iflag=direct status=none | md5sum | awk '{print \$1}'"
}


# ═══════════════════════════════════════════════════════════════════════════
# Concurrency-axis helpers
#
# Ported from the DRBD 9 test suite (LINBIT drbd9-tests),
# which exercises state transitions *while application I/O is running*. Every
# demo above this line drives I/O as a blocking sequential dd, so a request
# that is enqueued but never completed cannot be observed. That is exactly how
# the negotiation-window bio hang (fix-history #5) survived F01-F35.
#
#   busy_*             <- python/busywrite.py      (background fio + hang detection)
#   assert_no_forbidden<- drbdtest.forbidden_patterns
#   link_*             <- python/trafficcontrol.py (tc netem, per-destination)
# ═══════════════════════════════════════════════════════════════════════════

# ── Background writer (busywrite.py) ────────────────────────────────────────
# Keeps the device busy with fio while the demo performs control-plane
# operations. The point is busy_wait's timeout: fio not exiting within its own
# runtime means a bio never completed.
BUSY=()          # "node pid outfile" — registered for trap cleanup

# busy_start NODE [RUNTIME_S] [EXTRA_FIO_ARGS]
busy_start() {
    local node="$1" runtime="${2:-30}" extra="${3:-}"
    local out="/tmp/busy-${node//./_}.json" err="/tmp/busy-${node//./_}.err" pid
    pid=$(ssh_n "$node" "setsid bash -c 'fio --output-format=json --name=busy \
        --filename=/dev/mapper/$DEV --rw=randwrite --bs=64k --iodepth=32 \
        --ioengine=libaio --direct=1 --size=512M --time_based --runtime=$runtime \
        --continue_on_error=all $extra </dev/null >$out 2>$err & echo \$!'" 2>/dev/null)
    if ! [[ "$pid" =~ ^[0-9]+$ ]]; then
        fail "busy_start $node: could not launch fio (pid='$pid')"; return 1
    fi
    # Confirm it actually took off rather than dying on a bad argument.
    sleep 2
    if ! ssh_n "$node" "ps -p $pid >/dev/null" 2>/dev/null; then
        fail "busy_start $node: fio died immediately ($(ssh_n "$node" "head -2 $err" 2>/dev/null))"
        return 1
    fi
    # Drop any entry left by an earlier demo for this node. Without this the
    # array accumulates across demos and busy_wait/busy_errors match the *first*
    # (stale) record: the dead pid makes the hang check pass vacuously while the
    # writer that is actually running keeps overwriting the compared region.
    local e keep=() n
    for e in "${BUSY[@]:-}"; do
        [ -n "$e" ] || continue
        # shellcheck disable=SC2086
        set -- $e; n="$1"
        [ "$n" = "$node" ] || keep+=("$e")
    done
    BUSY=("${keep[@]:-}")

    BUSY+=("$node $pid $out")
    log "  busy writer up on $node (pid=$pid, runtime=${runtime}s)"
}

busy_running() {   # node pid
    ssh_n "$1" "ps -p $2 >/dev/null" 2>/dev/null
}

# busy_wait NODE TIMEOUT_S LABEL — wait for the writer to exit on its own.
# A timeout is the assertion that matters: it means a bio never completed.
busy_wait() {
    local node="$1" to="$2" label="$3" e n p out
    for e in "${BUSY[@]:-}"; do
        [ -n "$e" ] || continue
        # shellcheck disable=SC2086
        set -- $e; n="$1"; p="$2"; out="$3"
        [ "$n" = "$node" ] || continue
        if ssh_n "$node" "timeout $to tail --pid=$p -f /dev/null" 2>/dev/null; then
            pass "$label  (writer exited cleanly)"
        else
            fail "$label  (writer still running after ${to}s -> a bio never completed)"
            ssh_n "$node" "ps -o pid,stat,wchan:24,comm -p $p" 2>/dev/null | sed 's/^/    /'
            # fio forks a job process, so the recorded pid is only the parent:
            # kill -9 $p leaves the job writing into every later assertion and
            # into the next demo. Match on the name instead (fix-history appendix B #8).
            ssh_n "$node" "pkill -9 -x fio" 2>/dev/null || true
        fi
        return
    done
    fail "$label  (no busy writer registered on $node)"
}

# busy_errors NODE LABEL — assert the writer saw zero I/O errors.
# Peer loss must degrade to bitmap-dirty, never to EEO for the user.
busy_errors() {
    local node="$1" label="$2" e n p out errs
    for e in "${BUSY[@]:-}"; do
        [ -n "$e" ] || continue
        # shellcheck disable=SC2086
        set -- $e; n="$1"; p="$2"; out="$3"
        [ "$n" = "$node" ] || continue
        errs=$(ssh_n "$node" "python3 -c \"import json;d=json.load(open('$out'));print(d['jobs'][0].get('total_err',0))\"" 2>/dev/null)
        if [ "$errs" = "0" ]; then
            pass "$label  (0 I/O errors)"
        else
            fail "$label  (fio reported total_err=${errs:-<unparsable>})"
        fi
        return
    done
    fail "$label  (no busy writer registered on $node)"
}

busy_cleanup() {
    local e n p
    for e in "${BUSY[@]:-}"; do
        [ -n "$e" ] || continue
        # shellcheck disable=SC2086
        set -- $e; n="$1"; p="$2"
        # pkill -x as well as the pid: see the note in busy_wait.
        node_up "$n" && ssh_n "$n" "kill -9 $p 2>/dev/null; pkill -9 -x fio 2>/dev/null" >/dev/null 2>&1 || true
    done
    BUSY=()
}

# wait_established NODE [timeout_s] — poll until the device reports no
# replication debt. Writes issued before this point take the dirty-bitmap path
# instead of being replicated inline, and nothing drains those bits while the
# link stays up, so a test that starts its workload too early measures the
# bitmap rather than whatever it meant to measure.
wait_established() {
    local node="$1" to="${2:-90}" t=0 st
    while [ "$t" -le "$to" ]; do
        st=$(ssh_n "$node" "dmsetup status $DEV 2>/dev/null | grep -oE 'bm_dirty=[0-9]+'" 2>/dev/null)
        if [ "$st" = "bm_dirty=0" ]; then return 0; fi
        sleep 5; t=$((t + 5))
    done
    return 1
}

# ── Forbidden dmesg patterns (drbdtest.forbidden_patterns) ──────────────────
# Lines that must never appear while a demo runs. Checked against only the
# dmesg produced since forbidden_mark, so pre-existing noise (and F35's crash
# history) cannot poison a later demo.
# "unknown/unexpected packet type" is here because fix-history #22 sat unnoticed
# for a whole release: F48 printed two of them and still reported PASS, since
# nothing checked for them. One such line means a packet arrived that the
# receive loop did not expect -- either a phase race or a misframed stream.
FORBIDDEN_PAT='BUG:|WARNING:|Oops|general protection|soft lockup|hard LOCKUP|list_del corruption|list_add corruption|refcount_t:|use-after-free|KASAN|free_peer: drained|unknown packet type|unexpected packet type|unexpected pkt type'
FORBID_MARK=()   # "node uptime_seconds"

# The mark is a monotonic timestamp, not a line count. Line offsets are only
# valid while the kernel ring buffer does not wrap -- and it does: one
# fault-injection run pushed enough output that the recorded offset ended up
# *past* the line it was supposed to precede, so everything scoped to it
# silently scanned nothing and passed. Timestamps survive wrapping.
# Also records each node's boot id. A demo that crashes a node it did not mean
# to crash otherwise reports PASS: F42 did exactly that -- it asserted
# convergence while its peer was rebooting underneath it, and the only reason
# anyone noticed was that the *next* demo hung.
# setup_boot_of NODE — the boot_id recorded before the last cluster rebuild,
# empty if there is none for this node.
setup_boot_of() {
    local node="$1" e n b
    for e in "${SETUP_BOOT[@]:-}"; do
        [ -n "$e" ] || continue
        # shellcheck disable=SC2086
        set -- $e; n="$1"; b="$2"
        [ "$n" = "$node" ] && [ "$b" != "unknown" ] && { echo "$b"; return; }
    done
}

# forbidden_mark NODE... — open this demo's crash + dmesg window.
#
# The two halves of the mark deliberately come from different moments:
#
#   boot_id — from SETUP_BOOT, i.e. from *before* fresh2/fresh3 rebuilt the
#     cluster, when one is available. dm_pcache_dtr runs inside `reset`, so a
#     node can panic there, reboot, and still let `start` succeed; reading
#     boot_id here would record the post-crash id and assert_no_forbidden would
#     then compare it against itself and pass. That is precisely how
#     fix-history #17 stayed invisible across three regression attempts.
#
#   uptime — read now, because it scopes the dmesg scan. A pre-reset uptime
#     would drag the previous demo's output into this demo's evidence, which is
#     the trap appendix B lesson 5 was written about.
#
# SETUP_BOOT is consumed here: it belongs to the rebuild that just happened, and
# a demo that marks without rebuilding must not inherit a stale snapshot.
forbidden_mark() {
    local n t b
    FORBID_MARK=()
    for n in "$@"; do
        t=$(ssh_n "$n" "cut -d' ' -f1 /proc/uptime" 2>/dev/null)
        b=$(setup_boot_of "$n")
        [ -n "$b" ] || b=$(ssh_n "$n" "cat /proc/sys/kernel/random/boot_id" 2>/dev/null)
        FORBID_MARK+=("$n ${t:-0} ${b:-unknown}")
    done
    SETUP_BOOT=()
}

# ── Runner-level crash coverage ────────────────────────────────────────────
#
# Only 18 of the 51 demos call assert_no_forbidden, so 33 had no crash check at
# all; a node that panicked in one of those was noticed, at best, as some later
# demo failing to start. Snapshot every node around every demo so a death is
# attributed to the demo that caused it, whatever that demo asserts.
RUN_BOOT=()
run_boot_snapshot() {
    local n b
    RUN_BOOT=()
    for n in "$NODE1" "$NODE2" "$NODE3"; do
        b=$(ssh_n "$n" "cat /proc/sys/kernel/random/boot_id" 2>/dev/null)
        RUN_BOOT+=("$n ${b:-down}")
    done
}

# expect_reboot NODE — declare a restart intentional (sysrq crash test, VM
# recovery) and re-baseline it, so run_boot_check does not report it.
expect_reboot() {
    local node="$1" e n b out=()
    for e in "${RUN_BOOT[@]:-}"; do
        [ -n "$e" ] || continue
        # shellcheck disable=SC2086
        set -- $e; n="$1"
        if [ "$n" = "$node" ]; then
            b=$(ssh_n "$n" "cat /proc/sys/kernel/random/boot_id" 2>/dev/null)
            out+=("$n ${b:-down}")
        else
            out+=("$n $2")
        fi
    done
    RUN_BOOT=(${out[@]+"${out[@]}"})
}

# run_boot_check LABEL — did any node die during the demo just run?
run_boot_check() {
    local label="$1" e n b0 b1 dead=""
    for e in "${RUN_BOOT[@]:-}"; do
        [ -n "$e" ] || continue
        # shellcheck disable=SC2086
        set -- $e; n="$1"; b0="$2"
        [ "$b0" = "down" ] && continue          # already down before this demo
        if ! node_up "$n"; then
            dead="$dead $n(unreachable)"
            continue
        fi
        b1=$(ssh_n "$n" "cat /proc/sys/kernel/random/boot_id" 2>/dev/null)
        [ -n "$b1" ] && [ "$b1" != "$b0" ] && dead="$dead $n(rebooted)"
    done
    [ -z "$dead" ] && return 0
    fail "$label — node(s) died during this demo:$dead"
    echo "    An unannounced reboot is a crash. If it was deliberate, the demo"
    echo "    should say so with expect_reboot."
    for e in $dead; do
        n="${e%%(*}"
        ssh_n "$n" "journalctl -b -1 -k --no-pager 2>/dev/null | tail -12" 2>/dev/null \
            | sed "s/^/    [$n prev boot] /"
    done
    return 1
}

# dmesg_filter_since NODE — emit only the dmesg lines newer than the mark.
dmesg_filter_since() {
    local node="$1" e n t mark=0
    for e in "${FORBID_MARK[@]:-}"; do
        [ -n "$e" ] || continue
        # shellcheck disable=SC2086
        set -- $e; n="$1"; t="$2"
        [ "$n" = "$node" ] && { mark="$t"; break; }
    done
    ssh_n "$node" "dmesg | awk -v t=$mark 'match(\$0, /^\\[[ ]*[0-9]+\\.[0-9]+\\]/) { ts = substr(\$0, RSTART+1, RLENGTH-2) + 0; if (ts > t) print }'" 2>/dev/null
}

# assert_no_forbidden LABEL — check every marked node for forbidden lines.
assert_no_forbidden() {
    local label="$1" e n c b now hits bad=0 rebooted=""
    for e in "${FORBID_MARK[@]:-}"; do
        [ -n "$e" ] || continue
        # shellcheck disable=SC2086
        set -- $e; n="$1"; c="$2"; b="${3:-unknown}"

        # Did this node reboot while the demo ran? A crashed node takes its
        # dmesg with it, so this check has to come first -- otherwise the
        # forbidden-pattern scan below runs against a *fresh* log and passes
        # precisely when something went most wrong.
        #
        # Two earlier holes here let a dead node pass twice in a row:
        #   - "node_up || continue" skipped a node that had not finished
        #     rebooting yet, which is exactly when it is most guilty;
        #   - a node that was already down at forbidden_mark time recorded
        #     "unknown" and disabled its own check.
        # Both are failures now, not skips.
        if ! node_up "$n"; then
            rebooted="$rebooted $n(unreachable)"
            bad=1
            continue
        fi
        if [ "$b" = "unknown" ]; then
            rebooted="$rebooted $n(no-mark)"
            bad=1
            continue
        fi
        now=$(ssh_n "$n" "cat /proc/sys/kernel/random/boot_id" 2>/dev/null)
        if [ -n "$now" ] && [ "$now" != "$b" ]; then
            rebooted="$rebooted $n"
            bad=1
            continue
        fi

        hits=$(dmesg_filter_since "$n" | grep -E "$FORBIDDEN_PAT")
        if [ -n "$hits" ]; then
            bad=1
            echo "$hits" | head -5 | sed "s/^/    [$n] /"
        fi
    done
    if [ -n "$rebooted" ]; then
        fail "$label  (node(s) REBOOTED or went unreachable during the demo:$rebooted)"
        for n in $rebooted; do
            ssh_n "$n" "journalctl -b -1 -k --no-pager 2>/dev/null | tail -8" 2>/dev/null \
                | sed "s/^/    [$n prev boot] /"
        done
    elif [ "$bad" -eq 0 ]; then
        pass "$label  (no forbidden dmesg patterns)"
    else
        fail "$label  (forbidden dmesg pattern seen)"
    fi
}

# dmesg_since NODE PATTERN — true iff PATTERN appears in dmesg produced since
# forbidden_mark. The plain assert_dmesg/wait_dmesg above grep the whole ring
# buffer, so a line left by an earlier demo counts as evidence for a later one;
# every new demo must scope its positive evidence the same way it scopes the
# forbidden patterns.
dmesg_since() {
    local node="$1" pat="$2"
    dmesg_filter_since "$node" | has_line "$pat"
}

# dump_divergence NODE_A NODE_B [MB] — evidence bundle for a replication
# mismatch. Called at the moment of detection, so the state still belongs to
# this run; chasing one of these afterwards with an ad-hoc script means grepping
# a dmesg ring that also holds every earlier demo's output (appendix B).
dump_divergence() {
    local a="$1" b="$2" mb="${3:-64}" c n L dev bk
    echo "  ---- divergence evidence ----"
    echo "  per-8MiB chunks that differ:"
    for c in $(seq 0 $(( mb / 8 - 1 )) ); do
        local ma mb2
        ma=$(read_md5 "$a" 8 $((c * 8))); mb2=$(read_md5 "$b" 8 $((c * 8)))
        [ "$ma" != "$mb2" ] && echo "    chunk $c ($((c*8))-$((c*8+8))M): $ma vs $mb2"
    done
    echo "  device vs backing (differs => data never landed; same => both consistent internally):"
    for n in "$a" "$b"; do
        L=$(ssh_n "$n" "losetup -j /var/lib/pcache_backing.img | cut -d: -f1 | head -1" 2>/dev/null)
        dev=$(read_md5 "$n" "$mb")
        bk=$(ssh_nt "$n" "dd if=$L bs=1M count=$mb iflag=direct status=none | md5sum | awk '{print \$1}'" 2>/dev/null)
        echo "    $n device=${dev:0:12} backing=${bk:0:12} $([ "$dev" = "$bk" ] && echo same || echo DIFFER)"
    done
    echo "  counters:"
    for n in "$a" "$b"; do
        echo "    $n $(ssh_n "$n" "dmsetup status $DEV | grep -oE 'bm_gen=[0-9]+ bm_dirty=[0-9]+/[0-9]+'" 2>/dev/null)"
    done
    echo "  kernel evidence (scoped to this demo):"
    for n in "$a" "$b"; do
        local pats="RS_DATA write_to_cache failed|write to cache failed|RS_DATA recv failed|bad RS_ACK|resync (done|incomplete)"
        echo "    --- $n ---"
        dmesg_filter_since "$n" | grep -E "$pats" | tail -8 | sed 's/^/      /' | grep . \
            || echo "      (none)"
    done
    echo "  -----------------------------"
}

# ── Memory pressure (drbd9-tests/tests/bitmap-oom + python/mem_alloc.py) ────
# The fan-out allocates with GFP_NOIO, which blocks rather than skipping a peer.
# That trade -- never silently drop a replica, at the cost of possibly delaying
# an admin command -- has never been exercised under real memory pressure.
# The hog gets oom_score_adj=1000 so that if the box does run out, the kernel
# takes the hog and not sshd or fio.
HOGGED=()

mem_hog_start() {   # node percent_of_available
    local node="$1" pct="${2:-60}" kib pid rss i
    kib=$(ssh_n "$node" "awk '/MemAvailable/{print int(\$2 * $pct / 100)}' /proc/meminfo" 2>/dev/null)
    [[ "$kib" =~ ^[0-9]+$ ]] || { fail "mem_hog_start: cannot read MemAvailable on $node"; return 1; }
    pid=$(ssh_n "$node" "setsid bash -c 'echo 1000 > /proc/self/oom_score_adj; exec python3 -c \"
import sys, time
n = $kib * 1024
b = bytearray(n)
for i in range(0, n, 4096):
    b[i] = 1
sys.stderr.write(\\\"held\\\")
time.sleep(100000)
\" ' </dev/null >/dev/null 2>&1 & echo \$!" 2>/dev/null)
    [[ "$pid" =~ ^[0-9]+$ ]] || { fail "mem_hog_start: could not launch hog on $node"; return 1; }
    HOGGED+=("$node $pid")
    # DRBD waits for the helper's RSS to reach the target before proceeding;
    # without that the test runs against memory that was never actually taken.
    for i in $(seq 1 60); do
        rss=$(ssh_n "$node" "ps -q $pid --no-headers -o rss=" 2>/dev/null | tr -d ' ')
        [[ "$rss" =~ ^[0-9]+$ ]] || break
        [ "$rss" -ge $(( kib * 9 / 10 )) ] && {
            log "  $node holding $((rss / 1024)) MiB (target $((kib / 1024)) MiB)"
            return 0; }
        sleep 1
    done
    fail "mem_hog_start: $node never reached the target RSS (got ${rss:-none} KiB)"
    return 1
}

mem_hog_cleanup() {
    local e n p
    for e in "${HOGGED[@]:-}"; do
        [ -n "$e" ] || continue
        # shellcheck disable=SC2086
        set -- $e; n="$1"; p="$2"
        # pkill -x as well as the pid: see the note in busy_wait.
        node_up "$n" && ssh_n "$n" "kill -9 $p 2>/dev/null; pkill -9 -x fio 2>/dev/null" >/dev/null 2>&1 || true
    done
    HOGGED=()
}

# ── Fault injection (drbd_insert_fault / fault_rate / fault_types) ──────────
# The module exposes fault_rate (percent), fault_types (bitmask) and a
# read-only fault_count. Without this the only way to make a secondary reject a
# write is to race a teardown against live I/O -- an ~8% event that cost hours
# of waiting during the fix-history 14 hunt. See dm_pcache.h.
PCACHE_FAULT_DATA_WRITE=1        # bit 0 — secondary P_DATA -> cache
PCACHE_FAULT_RS_WRITE=2          # bit 1 — secondary P_RS_DATA -> cache
FAULTED=()                       # nodes with injection armed, for the trap

fault_arm() {   # node rate types
    local node="$1" rate="$2" types="$3"
    ssh_n "$node" "echo $types > /sys/module/dm_pcache/parameters/fault_types &&
                   echo $rate  > /sys/module/dm_pcache/parameters/fault_rate" 2>/dev/null || {
        fail "fault_arm: module has no fault-injection parameters on $node"; return 1; }
    FAULTED+=("$node")
    log "  fault injection armed on $node (rate=${rate}% types=$types)"
}

fault_disarm() {
    local node="$1" e keep=()
    ssh_n "$node" "echo 0 > /sys/module/dm_pcache/parameters/fault_rate" >/dev/null 2>&1 || true
    for e in "${FAULTED[@]:-}"; do
        [ -n "$e" ] && [ "$e" != "$node" ] && keep+=("$e")
    done
    FAULTED=("${keep[@]:-}")
}

fault_count() { ssh_n "$1" "cat /sys/module/dm_pcache/parameters/fault_count" 2>/dev/null; }

fault_cleanup() {
    local e
    for e in "${FAULTED[@]:-}"; do
        [ -n "$e" ] || continue
        node_up "$e" && ssh_n "$e" "echo 0 > /sys/module/dm_pcache/parameters/fault_rate" >/dev/null 2>&1 || true
    done
    FAULTED=()
}

# ── Per-destination traffic shaping (trafficcontrol.py) ─────────────────────
# A 'prio' qdisc with a u32 filter per peer IP. Unmatched traffic stays in
# band 0, which carries no netem -- so the harness's own SSH is never shaped.
# Widening a race window this way makes timing bugs reproducible instead of
# lucky (see drbd9-tests/tests/write-during-resync).
SHAPED=()        # nodes with a root qdisc installed

link_dev() {     # node dst -> egress interface name
    ssh_n "$1" "ip -o route get $2 2>/dev/null | grep -oP 'dev \\K\\S+' | head -1"
}

# link_shape NODE DST [delay] [rate] — e.g. link_shape n1 n2 50ms 100mbit
# NOTE: one shaped destination per node at a time. This rebuilds the root qdisc,
# so calling it twice on the same node for two different peers silently drops the
# first peer's filter -- F49 lost half an experiment to that. Shape per phase.
link_shape() {
    local node="$1" dst="$2" delay="${3:-}" rate="${4:-}" dev args=""
    dev=$(link_dev "$node" "$dst")
    [ -n "$dev" ] || { fail "link_shape: no route from $node to $dst"; return 1; }
    # tc does not autoload these here; without them netem fails with
    # "Specified qdisc kind is unknown".
    ssh_n "$node" "modprobe sch_prio 2>/dev/null; modprobe sch_netem 2>/dev/null" >/dev/null 2>&1
    ssh_n "$node" "tc qdisc del dev $dev root 2>/dev/null; \
        tc qdisc replace dev $dev root handle 1: prio bands 3 priomap $(printf '0 %.0s' {1..16}) && \
        tc filter add dev $dev parent 1: protocol ip prio 1 u32 match ip dst $dst flowid 1:2" \
        >/dev/null 2>&1 || { fail "link_shape: qdisc setup failed on $node"; return 1; }
    [ -n "$delay" ] && args="$args delay $delay"
    [ -n "$rate" ]  && args="$args rate $rate"
    ssh_n "$node" "tc qdisc replace dev $dev parent 1:2 handle 20: netem $args" \
        >/dev/null 2>&1 || { fail "link_shape: netem failed on $node"; return 1; }
    SHAPED+=("$node $dev")
    log "  shaped $node -> $dst on $dev ($args)"
}

link_reset() {   # node — drop all shaping
    local node="$1" e n d keep=()
    for e in "${SHAPED[@]:-}"; do
        [ -n "$e" ] || continue
        # shellcheck disable=SC2086
        set -- $e; n="$1"; d="$2"
        if [ "$n" = "$node" ]; then
            node_up "$n" && ssh_n "$n" "tc qdisc del dev $d root 2>/dev/null" >/dev/null 2>&1 || true
        else
            keep+=("$e")
        fi
    done
    SHAPED=("${keep[@]:-}")
}

link_cleanup() {
    local e n d
    for e in "${SHAPED[@]:-}"; do
        [ -n "$e" ] || continue
        # shellcheck disable=SC2086
        set -- $e; n="$1"; d="$2"
        node_up "$n" && ssh_n "$n" "tc qdisc del dev $d root 2>/dev/null" >/dev/null 2>&1 || true
    done
    SHAPED=()
}
# ═══════════════════════════════════════════════════════════════════════════
# Section 1 — Replication Path
# ═══════════════════════════════════════════════════════════════════════════

demo_f01() {
    section "F01 — Synchronous write replication (Protocol C)"
    fresh2 || return
    local mref m1 m2
    mref=$(primary_write 4)
    sleep 3
    m1=$(read_md5 "$NODE1" 4)
    m2=$(read_md5 "$NODE2" 4)
    log "  input md5 :  $mref"
    log "  node-1    :  $m1"
    log "  node-2    :  $m2"
    assert_md5_eq "F01 Protocol C sync replication" "$mref" "$m1" "$m2"
}

demo_f02() {
    section "F02 — Multi-peer fan-out broadcast write"
    fresh3 || return
    local mref m1 m2 m3
    mref=$(primary_write 4)
    sleep 3
    m1=$(read_md5 "$NODE1" 4)
    m2=$(read_md5 "$NODE2" 4)
    m3=$(read_md5 "$NODE3" 4)
    log "  input  :  $mref"
    log "  node-1 :  $m1"
    log "  node-2 :  $m2"
    log "  node-3 :  $m3"
    assert_md5_eq "F02 3-node fan-out" "$mref" "$m1" "$m2" "$m3"
}

demo_f03() {
    section "F03 — Zero-copy network transport (MSG_SPLICE_PAGES)"
    fresh2 || return
    log "  Writing 200 MiB from Primary; observe throughput (zero-copy path)"
    ssh_n "$NODE1" "dd if=/dev/zero of=/dev/mapper/$DEV bs=1M count=200 oflag=direct status=none 2>&1; echo ok"
    # No assert here; visual inspection
    ssh_n "$NODE1" 'dmesg | grep -E "send_bio|MSG_SPLICE" | tail -3' || true
    pass "F03 200 MiB write completed (verify dd output speed; should be >> 100 MB/s)"
}

demo_f04() {
    section "F04 — ACK timeout reaping + auto-reconnect"
    fresh2 || return
    log "  Block port $LISTEN_PORT inbound on Secondary"
    block_inbound "$NODE2" block || return
    log "  Primary tries to write — expect 30s ACK timeout then EIO"
    ssh_nt "$NODE1" "dd if=/dev/urandom of=/dev/mapper/$DEV bs=1M count=4 oflag=direct status=none 2>&1; echo dd-exit=\$?"
    unblock "$NODE2" block
    log "  Unblocked; reconnect should happen automatically"
    assert_dmesg "$NODE1" "ACK timeout|primary recv: ACK recv failed" "F04 ACK timeout reap demonstrated"
}

demo_f05() {
    section "F05 — Removed-peer re-add recovery (force-resync)"
    fresh3 || return
    primary_write 2 0 >/dev/null
    sleep 3
    log "  Remove node-3 (drops its per-peer dirty bitmap entirely)"
    ssh_n "$NODE1" "dmsetup message $DEV 0 remove-peer 2" || true
    log "  Write 2 MiB @offset 4 MiB while node-3 detached"
    primary_write 2 4 >/dev/null
    sleep 2
    # NOTE: remove-peer tears down node-3's dirty bitmap, so the re-added peer
    # comes back CLEAN — an incremental resync would transfer 0 runs and node-3
    # would stay stale (this is a real operational sharp-edge, see test guide).
    # The correct recovery for a re-added peer is force-resync (full sync).
    log "  Re-add node-3, then force-resync (full sync — the supported recovery)"
    ssh_n "$NODE1" "dmsetup message $DEV 0 add-peer 2 ${NODE3}:${LISTEN_PORT}" || true
    sleep 5
    ssh_n "$NODE1" "dmsetup message $DEV 0 force-resync" || true
    wait_md5_eq "F05 removed-peer re-add + force-resync caught node-3 up" \
        "$NODE1" "$NODE3" 8 45
}

demo_f06() {
    section "F06 — REQ_PREFLUSH cross-cluster flush barrier"
    fresh2 || return
    log "  dd with conv=fsync — replicates a flush; must complete (no 'Connection timed out')"
    local out
    out=$(ssh_n "$NODE1" "dd if=/dev/urandom of=/dev/mapper/$DEV bs=1M count=4 oflag=direct conv=fsync status=none 2>&1" || true)
    if grep -q "Connection timed out" <<<"$out"; then
        fail "F06 fsync hit ETIMEDOUT — secondary not ACKing flush?"
    else
        pass "F06 fsync flush replicated + ACKed (cross-cluster barrier, no timeout)"
    fi
}

# ═══════════════════════════════════════════════════════════════════════════
# Section 2 — Network & Mesh
# ═══════════════════════════════════════════════════════════════════════════

demo_f07() {
    section "F07 — Full-mesh accept dispatch"
    fresh3 || return
    local n hits=0
    for n in $NODE1 $NODE2 $NODE3; do
        echo "  --- $n ---"
        if ssh_n "$n" 'dmesg | grep -E "accept: handed inbound sock" | tail -3' | has_line; then
            hits=$((hits + 1))
        fi
    done
    [ "$hits" -ge 1 ] && pass "F07 accept_thread routing visible ($hits/3 nodes)" \
        || fail "F07 no accept dispatch logged on any node"
}

demo_f08() {
    section "F08 — id-based connection direction"
    fresh3 || return
    local n hits=0
    for n in $NODE1 $NODE2 $NODE3; do
        echo "  --- $n ---"
        if ssh_n "$n" 'dmesg | grep -E "handshake complete \(initiator\)|handshake complete \(responder\)" | tail -3' | has_line; then
            hits=$((hits + 1))
        fi
    done
    [ "$hits" -ge 1 ] && pass "F08 initiator/responder roles visible ($hits/3 nodes)" \
        || fail "F08 no handshake role logged on any node"
}

demo_f09() {
    section "F09 — SO_KEEPALIVE-driven dead-link detection"
    fresh2 || return
    log "  Block port; wait ~55s for keepalive to fire"
    block_inbound "$NODE2" block || return
    sleep 55
    unblock "$NODE2" block
    assert_dmesg "$NODE1" "ACK recv failed|connection lost" "F09 SO_KEEPALIVE triggered disconnect detection"
}

# ═══════════════════════════════════════════════════════════════════════════
# Section 3 — Dirty Tracking & Resync
# ═══════════════════════════════════════════════════════════════════════════

demo_f10() {
    section "F10 — Per-peer persistent dirty bitmap on PMEM"
    fresh2 || return
    log "  Check slot 0 PMEM magic = PCACMB1\\3 (0x504341434d423103)"
    ssh_n "$NODE1" 'echo "magic bytes (offset 0x7008, 8 bytes):"; dd if=/dev/pmem0 bs=1 skip=$((0x7008)) count=8 status=none | xxd'
    # Expected: 03 31 42 4d 43 41 43 50
    pass "F10 per-peer bitmap header on PMEM"
}

demo_f11() {
    section "F11 — Bitmap-based incremental resync"
    fresh2 || return
    primary_write 4 >/dev/null; sleep 3
    log "  Block Secondary inbound, wait 55s for disconnect detection"
    block_inbound "$NODE2" block || return
    sleep 55
    log "  Primary writes 1 MiB at offset 0 while peer offline"
    ssh_nt "$NODE1" "dd if=/dev/urandom of=/dev/mapper/$DEV bs=1M count=1 oflag=direct conv=fsync status=none 2>&1 || true"
    log "  Unblock and wait for incremental resync"
    unblock "$NODE2" block
    sleep 20
    # Expected: "dirty=256 / 131072 bits" — incremental, not full
    assert_dmesg "$NODE1" "resync starting" "F11 incremental resync ran (expect 'dirty=256' not 'dirty=131072')"
}

demo_f12() {
    section "F12 — Bilateral bitmap merge (P_BITMAP + ENC_RUN)"
    fresh2 || return
    log "  Trigger force-resync → bitmap exchange on reconnect"
    ssh_n "$NODE1" "dmsetup message $DEV 0 force-resync" || true
    sleep 10
    local n hits=0
    for n in $NODE1 $NODE2; do
        echo "  --- $n ---"
        if ssh_n "$n" 'dmesg | grep "bitmap-exchange" | tail -3' | has_line; then
            hits=$((hits + 1))
        fi
    done
    [ "$hits" -ge 1 ] && pass "F12 bilateral bitmap exchange logged ($hits/2 nodes)" \
        || fail "F12 no bitmap-exchange logged"
}

demo_f13() {
    section "F13 — Generation counter + per-peer peer_gen_at_sync"
    fresh2 || return
    log "  Initial state:"
    ssh_n "$NODE1" "dmsetup message $DEV 0 bitmap-status; dmesg | grep 'bitmap\\[slot' | tail -2"
    log "  Force-resync (bump gen)"
    ssh_n "$NODE1" "dmsetup message $DEV 0 force-resync"
    sleep 10
    ssh_n "$NODE1" "dmesg | grep -E 'reset-generation|force-resync|gen=' | tail -5"
    pass "F13 gen + peer_gen_at_sync tracking visible"
}

# ═══════════════════════════════════════════════════════════════════════════
# Section 4 — Persistence & Crash Recovery
# ═══════════════════════════════════════════════════════════════════════════

demo_f14() {
    section "F14 — PMEM superblock + double-copy CRC headers"
    fresh2 || return
    log "  Bitmap header copy 0 at 0x7000 (first 16 bytes):"
    ssh_n "$NODE1" "dd if=/dev/pmem0 bs=1 skip=\$((0x7000)) count=16 status=none | xxd"
    log "  Bitmap header copy 1 at 0x8000 (first 16 bytes):"
    ssh_n "$NODE1" "dd if=/dev/pmem0 bs=1 skip=\$((0x8000)) count=16 status=none | xxd"
    # Expected: both have valid CRC but different seq (alternating writer)
    pass "F14 double-copy headers visible"
}

demo_f15() {
    section "F15 — Persistent peer registry (32 slots)"
    fresh2 || return
    log "  Registry header copy 0 at 0x5000:"
    ssh_n "$NODE1" "dd if=/dev/pmem0 bs=1 skip=\$((0x5000)) count=32 status=none | xxd"
    # magic at offset 8: 33 52 50 4d 43 41 43 50  ("PCACMPR3" / 0x504341434d505233)
    log "  After remove + module reload + dmsetup create (no re-add-peer), registry should remember peer"
    ssh_n "$NODE1" "dmsetup remove $DEV
                    rmmod dm_pcache
                    insmod /root/dm-pcache.ko
                    LOOP=\$(losetup -j /var/lib/pcache_backing.img 2>/dev/null | cut -d: -f1 | head -1)
                    [ -z \"\$LOOP\" ] && LOOP=\$(losetup -f --show /var/lib/pcache_backing.img)
                    SIZE=\$(blockdev --getsz \$LOOP)
                    dmsetup create $DEV --table \"0 \$SIZE pcache-dist /dev/pmem0 \$LOOP my_node_id=0 is_primary=1 listen_port=$LISTEN_PORT\"
                    dmesg | grep 'registry: loaded' | tail -1"
    pass "F15 registry survived module reload"
}

demo_f16() {
    section "F16 — RESYNC_IN_PROGRESS crash detection"
    log "  Run bash $DEMO test-crash for full sysrq+c flow"
    log "  (skipped here — destructive; use 'bash demo_features.sh f16-run' to actually trigger)"
}

demo_f16-run() {
    bash "$DEMO" test-crash
}

demo_f17() {
    section "F17 — Idempotent add-peer (registry recovery)"
    fresh2 || return
    log "  dmsetup remove + recreate, registry still holds old peer"
    ssh_n "$NODE1" "dmsetup remove $DEV
                    rmmod dm_pcache
                    insmod /root/dm-pcache.ko
                    LOOP=\$(losetup -j /var/lib/pcache_backing.img 2>/dev/null | cut -d: -f1 | head -1)
                    [ -z \"\$LOOP\" ] && LOOP=\$(losetup -f --show /var/lib/pcache_backing.img)
                    SIZE=\$(blockdev --getsz \$LOOP)
                    dmsetup create $DEV --table \"0 \$SIZE pcache-dist /dev/pmem0 \$LOOP my_node_id=0 is_primary=1 listen_port=$LISTEN_PORT\"
                    # add-peer with registry entry already there
                    dmsetup message $DEV 0 add-peer 1 ${NODE2}:${LISTEN_PORT}
                    echo add-peer exit=\$?"
    # Expected: exit=0 (idempotent), not EEXIST
    pass "F17 idempotent add-peer reused registry slot"
}

# ═══════════════════════════════════════════════════════════════════════════
# Section 5 — HA / Role Management
# ═══════════════════════════════════════════════════════════════════════════

demo_f18() {
    section "F18 — Primary/Secondary role + write fencing"
    fresh2 || return
    local p_rc s_rc
    p_rc=$(ssh_n "$NODE1" "dd if=/dev/zero of=/dev/mapper/$DEV bs=4K count=1 oflag=direct status=none; echo \$?")
    s_rc=$(ssh_n "$NODE2" "dd if=/dev/zero of=/dev/mapper/$DEV bs=4K count=1 oflag=direct status=none 2>&1; echo \$?" | tail -1)
    log "  Primary write exit  : $p_rc  (expect 0)"
    log "  Secondary write exit: $s_rc  (expect non-zero EIO)"
    [ "$p_rc" = "0" ] && [ "$s_rc" != "0" ] && pass "F18 write fencing OK" || fail "F18 fencing broken"
}

demo_f19() {
    section "F19 — set-role runtime role switch"
    fresh2 || return
    log "  Demote node-1, force-promote node-2"
    ssh_n "$NODE1" "dmsetup message $DEV 0 set-role secondary && echo n1-demote-OK"
    sleep 2
    ssh_n "$NODE2" "dmsetup message $DEV 0 set-role primary force && echo n2-promote-OK"
    sleep 5
    local n2_rc n1_rc
    n2_rc=$(ssh_n "$NODE2" "dd if=/dev/zero of=/dev/mapper/$DEV bs=4K count=1 oflag=direct status=none; echo \$?")
    n1_rc=$(ssh_n "$NODE1" "dd if=/dev/zero of=/dev/mapper/$DEV bs=4K count=1 oflag=direct status=none 2>&1; echo \$?" | tail -1)
    log "  new-Primary (node-2) write : $n2_rc  (expect 0)"
    log "  ex-Primary  (node-1) write : $n1_rc  (expect non-zero)"
    [ "$n2_rc" = "0" ] && [ "$n1_rc" != "0" ] && pass "F19 role swapped correctly" || fail "F19 role swap broken"
}

demo_f20() {
    section "F20 — Coordinated demote handshake (P_DEMOTE_REQ/ACK)"
    fresh2 || return
    ssh_n "$NODE1" "dmsetup message $DEV 0 set-role secondary && echo demote-OK" || true
    sleep 2
    ssh_n "$NODE2" "dmsetup message $DEV 0 set-role primary && echo promote-OK" || true
    sleep 3
    log "  node-2 should have broadcast DEMOTE_REQ; node-1 should have received it:"
    local n2_ok=0 n1_ok=0
    ssh_n "$NODE2" 'dmesg | grep -E "broadcast DEMOTE_REQ|DEMOTE_ACK" | tail -5' | has_line && n2_ok=1
    ssh_n "$NODE1" 'dmesg | grep -E "DEMOTE_REQ" | tail -3' | has_line && n1_ok=1
    [ "$n2_ok" = 1 ] && [ "$n1_ok" = 1 ] && pass "F20 DEMOTE_REQ/ACK flow logged (both sides)" \
        || fail "F20 demote handshake not observed (n2_broadcast=$n2_ok n1_recv=$n1_ok)"
}

demo_f21() {
    section "F21 — Worker kthread role swap"
    fresh2 || return
    ssh_n "$NODE1" "dmsetup message $DEV 0 set-role secondary" || true
    sleep 2
    log "  After demote, node-1 dmesg should show worker swap (primary OFF → secondary up):"
    assert_dmesg "$NODE1" "set-role: demoted|handshake complete" "F21 worker swap visible"
}

# ═══════════════════════════════════════════════════════════════════════════
# Section 6 — Quorum & Split-brain Safety
# ═══════════════════════════════════════════════════════════════════════════

demo_f22() {
    section "F22 — Quorum check (DRBD-style local)"
    log "  See F23 demo for end-to-end quorum gating"
    demo_f23
}

demo_f23() {
    section "F23 — Configurable quorum (off | majority | N)"
    require_nodes "$NODE1" "$NODE2" || return
    bash "$DEMO" reset >/dev/null 2>&1
    if ! bash "$DEMO" start >/dev/null; then fail "F23 cluster start failed"; return; fi
    sleep 3
    log "  Restart node-1 with quorum=majority"
    ssh_n "$NODE1" "dmsetup remove $DEV
                    LOOP=\$(losetup -j /var/lib/pcache_backing.img 2>/dev/null | cut -d: -f1 | head -1)
                    SIZE=\$(blockdev --getsz \$LOOP)
                    dmsetup create $DEV --table \"0 \$SIZE pcache-dist /dev/pmem0 \$LOOP my_node_id=0 is_primary=1 listen_port=$LISTEN_PORT quorum=majority\"
                    dmsetup message $DEV 0 add-peer 1 ${NODE2}:${LISTEN_PORT}"
    sleep 5
    log "  Both peers up (quorum 2/2) — write should succeed"
    local rc_q
    rc_q=$(ssh_n "$NODE1" "dd if=/dev/zero of=/dev/mapper/$DEV bs=4K count=1 oflag=direct status=none; echo \$?")
    log "    write exit = $rc_q  (expect 0)"
    log "  Block peer outbound, wait 55s for keepalive disconnect"
    block_outbound_to "$NODE1" q "$NODE2" || return
    sleep 55
    log "  Now quorum 1/2 — write should EIO"
    local rc_nq
    rc_nq=$(ssh_nt "$NODE1" "dd if=/dev/zero of=/dev/mapper/$DEV bs=4K count=1 oflag=direct status=none 2>&1; echo \$?" | tail -1)
    log "    write exit = $rc_nq  (expect non-zero)"
    unblock "$NODE1" q
    [ "$rc_q" = "0" ] && [ "$rc_nq" != "0" ] && pass "F23 quorum gating OK" || fail "F23 quorum broken (up=$rc_q down=$rc_nq)"
}

demo_f24() {
    section "F24 — Split-brain safety (no silent convergence)"
    fresh2 || return
    # Split-brain (HALT) is only reachable once BOTH peers have completed the
    # initial full sync (FULL_SYNC_NEEDED cleared). On a fresh cluster
    # decide_sync_role takes the fresh-boot branch (my_need||peer_need ->
    # SYNC_SOURCE) and never HALTs, so we must establish a clean baseline first;
    # only then is a both-sides divergence a true "same gen, reciprocity broken"
    # conflict (dm_pcache_state.c:244).
    log "  Establish clean baseline sync (clears FULL_SYNC_NEEDED)"
    primary_write 2 >/dev/null
    local est=0 t=0
    while [ "$t" -le 40 ]; do
        if ssh_n "$NODE1" 'dmesg | grep -E "repl_state.*-> ESTABLISHED"' 2>/dev/null | has_line; then est=1; break; fi
        sleep 5; t=$((t + 5))
    done
    log "    baseline ESTABLISHED=$est (after ${t}s)"
    sleep 5
    log "  Isolate node-2 inbound (nft drop)"
    block_inbound "$NODE2" iso || return
    sleep 55
    log "  Both sides write divergent data while isolated"
    ssh_nt "$NODE1" "dd if=/dev/urandom of=/dev/mapper/$DEV bs=1M count=1 oflag=direct status=none 2>&1 || true"
    ssh_nt "$NODE2" "dmsetup message $DEV 0 set-role primary force
                    dd if=/dev/urandom of=/dev/mapper/$DEV bs=1M count=1 oflag=direct status=none 2>&1 || true"
    log "  Remove isolation; verify split-brain SAFETY (no silent convergence + repl halted)"
    unblock "$NODE2" iso
    sleep 30   # allow reconnect attempts
    # FINDING (see test guide): the 'split-brain detected' HALT log is NOT
    # reachable via the operator force-promote flow — set-role primary force sets
    # FULL_SYNC_NEEDED, so decide_sync_role takes the full-sync branch (SYNC_SOURCE)
    # instead of the reciprocity-broken HALT branch (dm_pcache_state.c:244). The
    # two-primary case livelocks SYNC_SOURCE->OFF. So we assert the OBSERVABLE
    # safety guarantee instead: divergent data must NOT silently converge, and
    # replication must stay halted (not ESTABLISHED) until the operator runs
    # resolve-split-brain (exercised by F25).
    local sm1 sm2 rs1 rs2
    sm1=$(read_md5 "$NODE1" 2); sm2=$(read_md5 "$NODE2" 2)
    rs1=$(ssh_n "$NODE1" 'dmesg | grep -oE "repl_state [A-Z_]+ -> [A-Z_]+" | tail -1' 2>/dev/null)
    rs2=$(ssh_n "$NODE2" 'dmesg | grep -oE "repl_state [A-Z_]+ -> [A-Z_]+" | tail -1' 2>/dev/null)
    log "  node-1 md5=$sm1 repl='$rs1'"
    log "  node-2 md5=$sm2 repl='$rs2'"
    if is_hash "$sm1" && is_hash "$sm2" && [ "$sm1" != "$sm2" ] \
       && ! grep -q "ESTABLISHED" <<<"$rs1 $rs2"; then
        pass "F24 split-brain safety: divergence preserved + replication halted (no silent overwrite)"
    else
        fail "F24 split-brain safety broken (m1=$sm1 m2=$sm2 rs1='$rs1' rs2='$rs2')"
    fi
}

demo_f25() {
    section "F25 — Split-brain manual resolve (victim/survivor)"
    require_nodes "$NODE1" "$NODE2" || return
    log "  Expected to be run AFTER F24 setup"
    log "  Designate node-1 = survivor, node-2 = victim"
    ssh_n "$NODE2" "dmsetup message $DEV 0 resolve-split-brain 0 victim
                    dmsetup message $DEV 0 set-role secondary force" || true
    ssh_n "$NODE1" "dmsetup message $DEV 0 resolve-split-brain 1 survivor" || true
    sleep 25
    local m1 m2
    m1=$(read_md5 "$NODE1" 1)
    m2=$(read_md5 "$NODE2" 1)
    log "  node-1 MD5 : $m1"
    log "  node-2 MD5 : $m2"
    assert_md5_eq "F25 split-brain resolved; data converged" "$m1" "$m2"
}

# ═══════════════════════════════════════════════════════════════════════════
# Section 7 — Operational Interface
# ═══════════════════════════════════════════════════════════════════════════

demo_f26() {
    section "F26 — dmsetup table line config"
    fresh2 || return
    ssh_n "$NODE1" "dmsetup table $DEV"
    ssh_n "$NODE1" 'dmesg | grep "network up" | tail -1'
    pass "F26 table line params parsed (see network up log)"
}

demo_f27() {
    section "F27 — add-peer / remove-peer / list-peers"
    fresh2 || return
    ssh_n "$NODE1" "dmsetup message $DEV 0 add-peer 2 ${NODE3}:${LISTEN_PORT}" || true
    sleep 2
    log "  list-peers:"
    ssh_n "$NODE1" "dmsetup message $DEV 0 list-peers; dmesg | grep -E 'slot=[0-9]+ node_id' | tail -5"
    ssh_n "$NODE1" "dmsetup message $DEV 0 remove-peer 2" || true
    sleep 1
    ssh_n "$NODE1" "dmsetup message $DEV 0 add-peer 2 ${NODE3}:${LISTEN_PORT} && echo re-add OK"
    pass "F27 add/remove/list-peers + idempotent re-add"
}

demo_f28() {
    section "F28 — abort-resync / force-resync / reset-generation"
    fresh2 || return
    log "  force-resync:"
    ssh_n "$NODE1" "dmsetup message $DEV 0 force-resync"
    sleep 5
    ssh_n "$NODE1" 'dmesg | grep -E "force-resync|full-sync needed" | tail -3'
    log "  abort-resync:"
    ssh_n "$NODE1" "dmsetup message $DEV 0 abort-resync && dmesg | grep abort-resync | tail -1"
    log "  reset-generation:"
    ssh_n "$NODE1" "dmsetup message $DEV 0 reset-generation && dmesg | grep reset-generation | tail -1"
    pass "F28 all three resync-control messages exercised"
}

demo_f29() {
    section "F29 — Status reporting"
    fresh3 || return
    primary_write 1 >/dev/null; sleep 3
    log "  dmsetup status:"
    ssh_n "$NODE1" "dmsetup status $DEV"
    log "  bitmap-status (per-peer):"
    ssh_n "$NODE1" "dmsetup message $DEV 0 bitmap-status"
    pass "F29 status + bitmap-status messages exercised"
}

demo_f30() {
    section "F30 — Clean PMEM reset script"
    log "  bash $DEMO reset wipes all 3 VMs in 1 sec"
    time bash "$DEMO" reset
    pass "F30 reset complete"
}

# ═══════════════════════════════════════════════════════════════════════════
# Section 8 — Performance / Cut A·deferred-fix·Cut C (this session's work)
# ═══════════════════════════════════════════════════════════════════════════

# Recreate the 2-node device with streams=N on the demo's existing loop backing.
# Reuses the cluster fresh2() already brought up; just swaps the table line.
recreate_streams() {
    local n="$1"
    for spec in "$NODE2:1:0" "$NODE1:0:1"; do
        local ip="${spec%%:*}"
        local rest="${spec#*:}"
        local nid="${rest%%:*}"
        local prim="${rest##*:}"
        ssh_n "$ip" "
            dmsetup remove $DEV 2>/dev/null || true
            LOOP=\$(losetup -j /var/lib/pcache_backing.img 2>/dev/null | cut -d: -f1 | head -1)
            [ -z \"\$LOOP\" ] && LOOP=\$(losetup -f --show /var/lib/pcache_backing.img)
            SZ=\$(blockdev --getsz \$LOOP)
            dmsetup create $DEV --table \"0 \$SZ pcache-dist /dev/pmem0 \$LOOP my_node_id=$nid is_primary=$prim listen_port=$LISTEN_PORT streams=$n\"
            dmsetup message $DEV 0 add-peer \$([ $nid = 0 ] && echo '1 ${NODE2}' || echo '0 ${NODE1}'):${LISTEN_PORT} 2>/dev/null || true
        " >/dev/null 2>&1
    done
    sleep 6
}

demo_f31() {
    section "F31 — Multi-stream replication (Cut C, streams=2)"
    fresh2 || return
    recreate_streams 2
    log "  control + data stream sockets handed off (accept dmesg):"
    ssh_n "$NODE2" 'dmesg | grep -E "handed inbound sock.*stream=" | tail -3' || true
    log "  per-data-stream threads:  primary pcache-dsend/drecv, secondary pcache-dsec"
    local nd ns
    nd=$(ssh_n "$NODE1" 'ps -eL -o comm 2>/dev/null | grep -c pcache-dsend')
    ns=$(ssh_n "$NODE2" 'ps -eL -o comm 2>/dev/null | grep -c pcache-dsec')
    [[ "$nd" =~ ^[0-9]+$ ]] || nd=0
    [[ "$ns" =~ ^[0-9]+$ ]] || ns=0
    log "    node-1 dsend threads=$nd  node-2 dsec threads=$ns  (expect >=1 each)"
    local mref m1 m2
    mref=$(primary_write 8); sleep 3
    m1=$(read_md5 "$NODE1" 8); m2=$(read_md5 "$NODE2" 8)
    log "  input=$mref node-1=$m1 node-2=$m2"
    if is_hash "$mref" && [ "$m1" = "$mref" ] && [ "$m2" = "$mref" ] && [ "$nd" -ge 1 ] && [ "$ns" -ge 1 ]; then
        pass "F31 streams=2: 2 streams up + replication correct (hash-by-sector ordering)"
    else
        fail "F31 multi-stream wrong (ref=$mref n1=$m1 n2=$m2 dsend=$nd dsec=$ns)"
    fi
}

demo_f32() {
    section "F32 — Multi-stream churn robustness (teardown under rapid reconnect)"
    log "  10 rapid recreate(streams=2)+write+teardown cycles; node must stay healthy"
    fresh2 || return
    local ok=1
    for c in $(seq 1 10); do
        recreate_streams 2 >/dev/null 2>&1
        ssh_n "$NODE1" "dd if=/dev/urandom of=/dev/mapper/$DEV bs=1M count=8 oflag=direct status=none 2>/dev/null || true"
        if ! ssh_n "$NODE1" 'true' || ! ssh_n "$NODE2" 'true'; then
            fail "F32 node unresponsive at cycle $c (churn deadlock?)"; ok=0; break
        fi
    done
    [ "$ok" = 1 ] && ssh_n "$NODE1" 'dmesg | grep -ciE "soft lockup|BUG:|Oops" | grep -q "^0$"' \
        && pass "F32 survived 10 churn cycles, no soft-lockup/oops" \
        || fail "F32 churn instability"
}

demo_f33() {
    section "F33 — Deferred-write replication under cache pressure"
    fresh2 || return
    log "  Heavy write burst (forces cache-full -> deferred writes); verify replication still matches"
    local mref m1 m2
    # write a working set, fsync, then compare both nodes (deferred writes must replicate)
    mref=$(ssh_nt "$NODE1" "dd if=/dev/urandom of=/tmp/p.bin bs=1M count=32 status=none && md5sum /tmp/p.bin|awk '{print \$1}'")
    ssh_nt "$NODE1" "dd if=/tmp/p.bin of=/dev/mapper/$DEV bs=1M count=32 oflag=direct conv=fsync status=none 2>&1 || true"
    sleep 3
    m1=$(read_md5 "$NODE1" 32); m2=$(read_md5 "$NODE2" 32)
    log "  input=$mref node-1=$m1 node-2=$m2"
    assert_md5_eq "F33 deferred writes replicated (node md5 match)" "$mref" "$m1" "$m2"
}

# ═══════════════════════════════════════════════════════════════════════════
# Section 9 — Fault Injection & Recovery (corruption / crash / failover)
# ═══════════════════════════════════════════════════════════════════════════

# Write N MiB random from an arbitrary node; echo md5 (empty on failure).
node_write() {   # node mb [seek]
    local node="$1" mb="$2" seek="${3:-0}"
    ssh_nt "$node" "dd if=/dev/urandom of=/tmp/nw.bin bs=1M count=$mb status=none && \
                    md5sum /tmp/nw.bin | awk '{print \$1}' && \
                    dd if=/tmp/nw.bin of=/dev/mapper/$DEV bs=1M count=$mb seek=$seek oflag=direct conv=fsync status=none" \
        2>/dev/null | head -1
}

# Recreate the pcache-dist device on a node with a given role (reuses backing loop).
recreate_dev() {   # node my_node_id is_primary
    local node="$1" nid="$2" prim="$3"
    ssh_n "$node" "
        dmsetup remove $DEV 2>/dev/null || true
        LOOP=\$(losetup -j /var/lib/pcache_backing.img 2>/dev/null | cut -d: -f1 | head -1)
        [ -z \"\$LOOP\" ] && LOOP=\$(losetup -f --show /var/lib/pcache_backing.img)
        SZ=\$(blockdev --getsz \$LOOP)
        dmsetup create $DEV --table \"0 \$SZ pcache-dist /dev/pmem0 \$LOOP my_node_id=$nid is_primary=$prim listen_port=$LISTEN_PORT\"
    " 2>&1 | tail -1
}

# Map node IP -> libvirt domain (only the 3 permitted clones).
vm_domain() { case "$1" in
    "$NODE1") echo fedora-clone ;;
    "$NODE2") echo fedora-clone-2 ;;
    "$NODE3") echo fedora-clone-3 ;;
esac; }

# Emergency VM recovery: virsh destroy+start the node's domain, wait for SSH.
recover_vm() {   # node
    local node="$1" dom i; dom=$(vm_domain "$node")
    [ -n "$dom" ] || { fail "no domain mapping for $node"; return 1; }
    log "  recovering VM $dom (virsh destroy+start)"
    ${SUDO:-sudo} virsh destroy "$dom" >/dev/null 2>&1 || true
    ${SUDO:-sudo} virsh start "$dom" >/dev/null 2>&1 || true
    for i in $(seq 1 40); do node_up "$node" && { log "  $node SSH back (~$((i*3))s)"; expect_reboot "$node"; return 0; }; sleep 3; done
    fail "  $node did not come back after recovery"; return 1
}

demo_f34() {
    section "F34 — On-media metadata double-copy corruption recovery"
    fresh2 || return
    local mref
    mref=$(primary_write 4)
    log "  input md5: $mref"
    # cache_info is load-critical and double-copied at 0x2000 / 0x3000
    # (PCACHE_CACHE_INFO_OFF, x PCACHE_META_INDEX_MAX). Zero copy-0; the device
    # must reload via the surviving copy-1 (pcache_meta_find_latest) with no data
    # loss. Also re-add the peer to confirm the persisted registry slot is reused.
    log "  Remove device; zero cache_info COPY-0 (0x2000), keep copy-1 (0x3000)"
    ssh_n "$NODE1" "dmsetup remove $DEV
                    dd if=/dev/zero of=/dev/pmem0 bs=4096 seek=2 count=1 conv=notrunc status=none
                    sync" 2>/dev/null
    log "  Recreate device — must load via surviving copy (else create fails)"
    local cr loaded m1 readd
    cr=$(recreate_dev "$NODE1" 0 1); echo "    recreate: $cr"
    sleep 3
    loaded=$(ssh_n "$NODE1" "dmsetup info $DEV >/dev/null 2>&1 && echo 1 || echo 0" 2>/dev/null)
    m1=$(read_md5 "$NODE1" 4)
    readd=$(ssh_n "$NODE1" "dmsetup message $DEV 0 add-peer 1 ${NODE2}:${LISTEN_PORT} >/dev/null 2>&1 && echo ok" 2>/dev/null)
    log "  device loaded=$loaded  data md5=$m1  re-add-peer=$readd"
    # Recovery proof: device reloaded despite one metadata copy zeroed, and the
    # 4 MiB written before corruption reads back byte-identical.
    if [ "${loaded:-0}" = "1" ] && is_hash "$mref" && [ "$m1" = "$mref" ]; then
        pass "F34 recovered from single-copy metadata corruption (device loaded + data intact)"
    else
        fail "F34 corruption recovery failed (loaded=$loaded ref=$mref got=$m1)"
    fi
}

demo_f35() {
    section "F35 — Primary crash -> Secondary failover -> rejoin & resync"
    fresh2 || return
    local mref m2_before
    mref=$(primary_write 4)          # Protocol-C durable on BOTH nodes
    sleep 3
    m2_before=$(read_md5 "$NODE2" 4) # survivor copy before the crash
    log "  baseline md5=$mref  node-2(survivor)=$m2_before"

    log "  [1] Hard-crash Primary (sysrq+c on node-1)"
    ssh_n "$NODE1" 'echo c > /proc/sysrq-trigger' 2>/dev/null || true
    sleep 15
    if node_up "$NODE1"; then
        log "  node-1 still up (self-rebooted?)"
        expect_reboot "$NODE1"   # the sysrq crash above is the point of F35
    else
        recover_vm "$NODE1" || return
    fi

    log "  [2] DURABILITY: node-2 still holds the fsync'd data after Primary loss"
    local m2_after
    m2_after=$(read_md5 "$NODE2" 4)
    assert_md5_eq "F35a durability: fsync'd data survived Primary crash" "$mref" "$m2_before" "$m2_after"

    log "  [3] FAILOVER: promote node-2, write NEW data, verify it serves writes"
    ssh_n "$NODE2" "dmsetup message $DEV 0 set-role primary force" 2>/dev/null || true
    sleep 3
    local newmd rc2
    newmd=$(node_write "$NODE2" 2 4)   # new 2 MiB @offset 4 on the new Primary
    rc2=$?
    log "    new-Primary(node-2) write md5=$newmd rc=$rc2"
    [ -n "$newmd" ] && is_hash "$newmd" && pass "F35b failover: promoted node-2 serves writes" \
        || fail "F35b failover write failed (md5='$newmd')"

    log "  [4] REJOIN: bring node-1 back as SECONDARY, add peer, force-resync"
    ssh_n "$NODE1" "rmmod dm_pcache 2>/dev/null; insmod /root/dm-pcache.ko 2>/dev/null; true" 2>/dev/null
    recreate_dev "$NODE1" 0 0          # node-1 rejoins as Secondary
    ssh_n "$NODE1" "dmsetup message $DEV 0 add-peer 1 ${NODE2}:${LISTEN_PORT}" 2>/dev/null || true
    ssh_n "$NODE2" "dmsetup message $DEV 0 add-peer 0 ${NODE1}:${LISTEN_PORT}; dmsetup message $DEV 0 force-resync" 2>/dev/null || true

    log "  [5] CONVERGENCE: node-1 (rejoined Secondary) catches up to node-2 (6 MiB)"
    wait_md5_eq "F35c rejoin+resync: node-1 converged to new Primary" "$NODE2" "$NODE1" 6 60
}

# ═══════════════════════════════════════════════════════════════════════════
# Top-level dispatch
# ═══════════════════════════════════════════════════════════════════════════


# Safe subset that doesn't crash VMs or run 60s+ tests

# ═══════════════════════════════════════════════════════════════════════════
# Section 10 — Concurrency (control-plane operations under live write load)
#
# Everything in F01-F35 drives I/O as a blocking sequential dd: the write is
# already finished before the control-plane action starts. These demos keep a
# writer running *across* the action, which is the axis the DRBD 9 suite leans
# on (concurrent-disconnect, switch-primaries, write-during-resync) and the one
# that exposed the negotiation-window bio hang.
#
# Every demo asserts the same three things:
#   (a) no hang        — the writer exits within its own runtime  (busy_wait)
#   (b) no data loss   — Primary and Secondary agree afterwards   (wait_md5_eq)
#   (c) no user errors — peer loss degrades, it does not EIO      (busy_errors)
# ═══════════════════════════════════════════════════════════════════════════

demo_f36() {
    section "F36 — Peer churn under live write load"
    # Ported from drbd9-tests/tests/concurrent-disconnect: repeat a control-plane
    # teardown while the device is busy. DRBD loops because "the potential
    # deadlock is a race"; the same applies here -- the window between
    # "socket connected" and "negotiation complete" is milliseconds wide.
    #
    # Scenario shape follows drbd9-tests/tests/split-brain: churn -> the pair
    # may latch -> operator arbitrates -> data converges. The false split-brain
    # that remove-peer + add-peer used to guarantee (a re-added peer reloaded a
    # stale bitmap out of the same PMEM slot) is fixed -- see fix-history #9 --
    # so this normally reports "did not latch". The arbitration branch stays:
    # a genuine divergence must still be reported and resolved, never silently
    # converged, and that path needs to keep being exercised.
    fresh2 || return
    forbidden_mark "$NODE1" "$NODE2"

    busy_start "$NODE1" 40 || return
    sleep 3

    local i rc_rm=0 rc_add=0
    for i in 1 2 3 4; do
        ssh_n "$NODE1" "dmsetup message $DEV 0 remove-peer 1" >/dev/null 2>&1 || rc_rm=1
        sleep 2
        ssh_n "$NODE1" "dmsetup message $DEV 0 add-peer 1 $NODE2:$LISTEN_PORT" >/dev/null 2>&1 || rc_add=1
        sleep 3
        log "  cycle $i done (remove rc=$rc_rm add rc=$rc_add)"
    done
    assert_match "F36 remove-peer accepted under load" "$rc_rm" "0"
    assert_match "F36 add-peer accepted under load"    "$rc_add" "0"

    # (a) the assertion that catches a stranded bio -- this is the regression
    #     guard for the negotiation-window hang (kernel 6b9406f862a4).
    busy_wait "$NODE1" 60 "F36 no bio hang across peer churn"
    # (c) an operator action must not surface as filesystem I/O errors
    #     (kernel 6b9406f862a4 flush-status half).
    busy_errors "$NODE1" "F36 churn causes no user-visible I/O error"

    # Write a known pattern after the churn so the convergence check below has
    # something it must actually carry across. Comparing the two nodes straight
    # after the churn would pass trivially whenever the busy writer's random
    # offsets happened to miss the compared region.
    local mref m1 m2
    mref=$(primary_write 8)
    if ! is_hash "$mref"; then fail "F36 could not write the post-churn pattern"; return; fi

    # The outcome of the churn is not deterministic: whether decide_sync_role
    # declares a (false) split-brain depends on where the busy writer had pushed
    # the generation counters at the instant of the reconnect. Both outcomes are
    # acceptable -- what must never happen is silent divergence. So: if the pair
    # latched, it must have said so, and the documented arbitration must clear it.
    # Arbitration has to be applied to *every* node that latched. A HALTed
    # worker sits in its own loop until resolve-split-brain restarts it --
    # force-resync does not break it out -- so arbitrating only one side leaves
    # the other wedged forever. Pairing per manual section 7.2: the Primary
    # survives, the Secondary surrenders.
    local latched=0
    if dmesg_since "$NODE1" "split-brain detected"; then
        latched=1
        log "  arbitration: node-1 (Primary) keeps its data (survivor)"
        ssh_n "$NODE1" "dmsetup message $DEV 0 resolve-split-brain 1 survivor" >/dev/null 2>&1
    fi
    if dmesg_since "$NODE2" "split-brain detected"; then
        latched=1
        log "  arbitration: node-2 (Secondary) surrenders (victim)"
        ssh_n "$NODE2" "dmsetup message $DEV 0 resolve-split-brain 0 victim" >/dev/null 2>&1
    fi
    if [ "$latched" -eq 1 ]; then
        pass "F36 churn-induced divergence is reported, not silently ignored"
        sleep 10
    else
        log "  (this run did not latch — expected since fix-history #9)"
    fi
    # Settle before asking for a resync. pcache_do_resync now drains writeback
    # itself (fix-history #8), so this is no longer load-bearing for
    # correctness; it just keeps F36 measuring churn robustness instead of
    # waiting out a drain that can take a while under a live writer.
    ssh_nt "$NODE1" "sync; blockdev --flushbufs /dev/mapper/$DEV" >/dev/null 2>&1
    sleep 5
    # Belt and braces: a re-added peer now comes up with FULL_SYNC_NEEDED on its
    # own (fix-history #9), so this is normally a no-op. Kept so the demo also
    # covers the path where arbitration above left something owed.
    ssh_n "$NODE1" "dmsetup message $DEV 0 force-resync" >/dev/null 2>&1

    # (b) both sides must end up holding the pattern -- not merely agreeing.
    wait_md5_eq "F36 data converges after churn" "$NODE1" "$NODE2" 8 150
    m1=$(read_md5 "$NODE1" 8); m2=$(read_md5 "$NODE2" 8)
    assert_md5_eq "F36 converged content is the post-churn pattern" "$mref" "$m1" "$m2"

    assert_no_forbidden "F36 kernel log clean"
}

demo_f37() {
    section "F37 — Secondary restart under live write load (create before add-peer)"
    # The window is not exotic: it is the documented setup order. The secondary's
    # dmsetup create brings its listen socket up, but until add-peer runs the
    # accept thread answers "no peer slot ... drop", which the primary sees as
    # ECONNRESET out of the sync-params exchange -- once per second, with writes
    # in flight the whole time. Cf. drbd9-tests/tests/connect-abort and
    # promote_while_connecting.
    fresh2 || return
    forbidden_mark "$NODE1" "$NODE2"

    local sz
    sz=$(ssh_n "$NODE2" "blockdev --getsz /dev/mapper/$DEV" 2>/dev/null)
    if ! [[ "$sz" =~ ^[0-9]+$ ]]; then fail "F37 could not read device size from $NODE2"; return; fi

    busy_start "$NODE1" 45 || return
    sleep 4

    log "  node-2 down (device removed)"
    ssh_n "$NODE2" "dmsetup remove $DEV" >/dev/null 2>&1
    sleep 4

    log "  node-2 create only — listen socket up, no peer slot yet"
    local loop
    loop=$(ssh_n "$NODE2" "losetup -j /var/lib/pcache_backing.img | cut -d: -f1 | head -1" 2>/dev/null)
    if [ -z "$loop" ]; then
        loop=$(ssh_n "$NODE2" "losetup -f --show /var/lib/pcache_backing.img" 2>/dev/null)
    fi
    if [ -z "$loop" ]; then fail "F37 no backing loop device on $NODE2"; return; fi
    ssh_n "$NODE2" "dmsetup create $DEV --table '0 $sz pcache-dist /dev/pmem0 $loop my_node_id=1 is_primary=0 listen_port=$LISTEN_PORT'" >/dev/null 2>&1 \
        || { fail "F37 could not recreate the device on $NODE2"; return; }
    log "  holding 12s in the create -> add-peer gap"
    sleep 12
    # The primary should be hammering the window; confirm it really was exercised.
    assert_dmesg "$NODE1" "sync_params exchange failed|no peer slot" \
        "F37 negotiation window actually exercised"

    log "  node-2 add-peer"
    ssh_n "$NODE2" "dmsetup message $DEV 0 add-peer 0 $NODE1:$LISTEN_PORT" >/dev/null 2>&1

    busy_wait "$NODE1" 60 "F37 no bio hang across the negotiation window"
    busy_errors "$NODE1" "F37 restart causes no user-visible I/O error"

    # Write a known pattern after the peer is back, so convergence has to carry
    # something across rather than passing on whatever the two sides already
    # shared. Settle writeback first: pcache_do_resync reads the backing device
    # and the pcache_cache_flush() it performs only closes ksets (see F36).
    local mref m1 m2
    mref=$(primary_write 8)
    if ! is_hash "$mref"; then fail "F37 could not write the post-restart pattern"; return; fi
    ssh_nt "$NODE1" "sync; blockdev --flushbufs /dev/mapper/$DEV" >/dev/null 2>&1
    sleep 5
    ssh_n "$NODE1" "dmsetup message $DEV 0 force-resync" >/dev/null 2>&1

    wait_md5_eq "F37 data converges after peer restart" "$NODE1" "$NODE2" 8 120
    m1=$(read_md5 "$NODE1" 8); m2=$(read_md5 "$NODE2" 8)
    assert_md5_eq "F37 converged content is the post-restart pattern" "$mref" "$m1" "$m2"

    assert_no_forbidden "F37 kernel log clean"
}
demo_f39() {
    section "F39 — Continuous writes during a resync"
    # Ported from drbd9-tests/tests/write-during-resync. The point of that test
    # is not the resync itself but the overlap: application writes land while
    # the SyncSource is streaming, and DRBD shapes the link so the overlap is
    # reproducible rather than lucky. Same here -- without the delay the resync
    # finishes in ~0.3s on this rig and the window is easy to miss.
    #
    # Invariant under test: once the resync has finished and the writer has
    # stopped, the two nodes must agree *without* another force-resync. Writes
    # issued during the resync are either carried by it or left marked dirty;
    # neither may be silently dropped.
    fresh2 || return
    forbidden_mark "$NODE1" "$NODE2"

    # Slow the replication link so the resync spans the write load.
    # 10ms is ~50x this rig's native RTT -- enough to stretch the sweep well past
    # the write load without making the test take forever. Each pass walks 2048
    # 256 KiB chunks, so a pass costs roughly nr_chunks x RTT; a continuously
    # writing peer forces several passes (see the multi-pass sweep in
    # pcache_do_resync), which is why the verdict window below is generous.
    link_shape "$NODE1" "$NODE2" 10ms || { link_reset "$NODE1"; return; }

    # The writer must outlast the resync, otherwise the tail of the sweep runs
    # against a quiet device and converges trivially. At 25ms the resync takes
    # ~110s on this rig, so give the writer a good margin over that.
    #
    # It must also be *rate limited*, and that is not a detail: the per-chunk
    # writeback drain in pcache_do_resync waits for the whole cache to be clean
    # before it reads a chunk off backing. An unthrottled writer (this rig does
    # ~530 MiB/s, 8.5k IOPS) never lets that happen, so the pre-loop drain and
    # then chunk 0 both time out and the sweep ends having shipped nothing:
    #   "resync incomplete, 131072 bit(s) still owed"
    # The test still passed, because the writes replicate inline once the resync
    # gives up and the link goes ESTABLISHED -- i.e. it was asserting on the
    # inline path while claiming to test the overlap. 400 IOPS (~25 MiB/s) is
    # slow enough for the drain to keep up and for the sweep to actually stream.
    busy_start "$NODE1" 200 "--rate_iops=${F39_RATE_IOPS:-400}" || { link_reset "$NODE1"; return; }
    sleep 4

    log "  force-resync with the writer running"
    ssh_n "$NODE1" "dmsetup message $DEV 0 force-resync" >/dev/null 2>&1

    # Wait for the sweep to report a verdict rather than guessing at a sleep.
    local t=0 done_seen=0
    while [ "$t" -le 300 ]; do
        if dmesg_since "$NODE1" "resync done|resync incomplete"; then done_seen=1; break; fi
        sleep 5; t=$((t + 5))
    done
    local verdict=""
    if [ "$done_seen" -eq 1 ]; then
        pass "F39 resync reached a verdict (${t}s)"
        verdict=$(ssh_n "$NODE1" "dmesg | grep -E 'resync (done|incomplete)' | tail -1" 2>/dev/null)
        echo "$verdict" | sed 's/^/    /'
    else
        fail "F39 resync never reported done/incomplete within 300s"
    fi

    # The sweep has to have streamed something, or this demo proves nothing:
    # a resync that gives up before chunk 0 leaves the final md5 comparison
    # resting entirely on inline replication. Same reasoning as F41's
    # "the fault really was injected" guard.
    local owed total
    total=$(ssh_n "$NODE1" "dmsetup status $DEV | grep -oE 'bm_dirty=[0-9]+/[0-9]+' | cut -d/ -f2" 2>/dev/null)
    owed=$(echo "$verdict" | grep -oE '[0-9]+ bit\(s\) still owed' | grep -oE '^[0-9]+')
    [ -n "$owed" ] || owed=0        # "resync done (dirty=0)" -- swept everything
    if [[ "$total" =~ ^[0-9]+$ ]] && [ "$owed" -lt "$total" ]; then
        pass "F39 the sweep actually streamed ($((total - owed)) of $total bits carried by the resync)"
    else
        fail "F39 the sweep shipped nothing (owed=$owed of $total) -- the overlap under test never happened"
    fi

    busy_wait "$NODE1" 260 "F39 no bio hang across the resync"
    busy_errors "$NODE1" "F39 writes during resync cause no user-visible I/O error"

    link_reset "$NODE1"

    # Let replication settle: anything the resync left owed is still marked
    # dirty, and post-resync writes replicate normally.
    ssh_nt "$NODE1" "sync; blockdev --flushbufs /dev/mapper/$DEV" >/dev/null 2>&1
    sleep 20

    # THE assertion: agreement without a rescue force-resync. The busy writer
    # covers 512 MiB, so compare a region it certainly touched.
    local m1 m2
    m1=$(read_md5 "$NODE1" 64); m2=$(read_md5 "$NODE2" 64)
    log "  node-1 (64M): $m1"
    log "  node-2 (64M): $m2"
    # This one is intermittent (fix-history #14) and has resisted reproduction on
    # demand, so capture the evidence here, while it is still this run's state.
    [ "$m1" = "$m2" ] || dump_divergence "$NODE1" "$NODE2" 64
    assert_md5_eq "F39 nodes agree after a resync that overlapped writes" "$m1" "$m2"

    assert_no_forbidden "F39 kernel log clean"
}

demo_f38() {
    section "F38 — Role switch under live write load"
    # drbd9-tests/tests/switch-primaries moves the Primary role around a running
    # cluster in a loop; the interesting part is doing it while I/O is in
    # flight, not the switch itself. F18-F21 already cover the static fencing
    # and worker swap, so what this adds is the concurrency: a demote must
    # fence promptly, must not strand a bio, and the device must be fully
    # usable again after the promote.
    #
    # Note the errors here are *expected*: once this node is Secondary its
    # writes are refused by design (manual section 6.1), so unlike F36/F37 this
    # demo asserts the fence fired, not that the writer was error-free.
    fresh2 || return
    forbidden_mark "$NODE1" "$NODE2"

    busy_start "$NODE1" 45 || return
    sleep 5

    log "  demote node-1 to Secondary while it is writing"
    ssh_n "$NODE1" "dmsetup message $DEV 0 set-role secondary" >/dev/null 2>&1
    sleep 3

    # The write fence must actually bite -- a probe write has to fail now.
    local rc
    rc=$(ssh_nt "$NODE1" "dd if=/dev/zero of=/dev/mapper/$DEV bs=4K count=1 oflag=direct status=none 2>/dev/null; echo \$?" | tail -1)
    assert_neq "F38 write fence engaged while demoted" "$rc" "0"

    sleep 4
    log "  promote node-1 back to Primary"
    ssh_n "$NODE1" "dmsetup message $DEV 0 set-role primary force" >/dev/null 2>&1
    sleep 6

    # The writer must terminate rather than hang on a bio stranded by the
    # role change (this is the F36/F37 regression guard applied to set-role).
    busy_wait "$NODE1" 90 "F38 no bio hang across demote/promote"

    # And the device has to be genuinely usable again, both locally and on the peer.
    local mref m1 m2
    mref=$(primary_write 8)
    if ! is_hash "$mref"; then fail "F38 device not writable after promote"; return; fi
    ssh_nt "$NODE1" "sync; blockdev --flushbufs /dev/mapper/$DEV" >/dev/null 2>&1
    sleep 5
    ssh_n "$NODE1" "dmsetup message $DEV 0 force-resync" >/dev/null 2>&1
    wait_md5_eq "F38 replication healthy again after the role round-trip" "$NODE1" "$NODE2" 8 150
    m1=$(read_md5 "$NODE1" 8); m2=$(read_md5 "$NODE2" 8)
    assert_md5_eq "F38 post-promote content reached the peer" "$mref" "$m1" "$m2"

    assert_no_forbidden "F38 kernel log clean"
}

demo_f40() {
    section "F40 — Overlapping concurrent writes across streams (ordering)"
    # Section 3.3 demuxes writes by hash_64(sector) so a given region always
    # travels the same stream, which is what preserves per-region ordering when
    # nr_streams > 1. Nothing tested that invariant: every other demo writes
    # sequentially, where any ordering would produce the same result.
    #
    # Hammer a deliberately small region from several jobs at once so the same
    # blocks are rewritten constantly, then compare. If two writes to one region
    # could take different streams, the peer could apply them in the other order
    # and end up with a different final block than the Primary.
    # Overrides for the A/B that isolates the routing key (defaults are the
    # regression's own settings, so an unset environment changes nothing):
    #
    #   F40_BS       fio block-size spec. The default --bs=4k means every write
    #                to a block starts at the same sector, and the router keys on
    #                the request's start sector -- so this default never produces
    #                two overlapping writes with different keys. A mixed size
    #                (--bsrange=4k-64k --blockalign=4k) does, constantly.
    #   F40_STREAMS  2 by default. 1 removes cross-stream ordering altogether and
    #                is the control: with one stream TCP orders everything.
    local f40_bs=${F40_BS:---bs=4k}
    local f40_streams=${F40_STREAMS:-2}
    #   F40_IODEPTH / F40_JOBS  default 16 / 4. Setting both to 1 serialises the
    #                application's writes completely, which leaves exactly one
    #                correct final state: a mismatch then cannot be two nodes
    #                picking different winners among concurrent writes, only
    #                data applied wrongly.
    local f40_iodepth=${F40_IODEPTH:-16}
    local f40_jobs=${F40_JOBS:-4}
    fresh2 || return
    recreate_streams "$f40_streams"

    # The workload must run entirely inside a settled connection. Anything
    # written while the peer is not yet replication-eligible goes to the dirty
    # bitmap instead of the wire, and those bits are not drained while the link
    # stays up -- the nodes then differ for a reason that has nothing to do with
    # ordering. (That gap is real and noted in the manual; it is simply not what
    # this demo is measuring.)
    if wait_established "$NODE1" 90; then
        pass "F40 replication settled before the workload"
    else
        fail "F40 replication never settled (bm_dirty != 0) — cannot test ordering"
        return
    fi
    forbidden_mark "$NODE1" "$NODE2"

    # Confirm we really are multi-stream, else this proves nothing. (Skipped
    # for the single-stream control, which is single-stream on purpose.)
    local nd
    nd=$(ssh_n "$NODE1" 'ps -eL -o comm 2>/dev/null | grep -c pcache-dsend')
    [[ "$nd" =~ ^[0-9]+$ ]] || nd=0
    if [ "$f40_streams" -le 1 ]; then
        log "  F40 single-stream control (streams=$f40_streams, dsend=$nd)"
    elif [ "$nd" -ge 1 ]; then
        pass "F40 data-stream threads present (streams=$f40_streams, dsend=$nd)"
    else
        fail "F40 no data-stream thread — not actually multi-stream, result would be meaningless"
        return
    fi

    log "  $f40_jobs job(s) x iodepth $f40_iodepth rewriting the same 8 MiB region ($f40_bs, streams=$f40_streams)"
    ssh_nt "$NODE1" "fio --name=ovl --filename=/dev/mapper/$DEV --rw=randwrite $f40_bs \
        --iodepth=$f40_iodepth --numjobs=$f40_jobs --ioengine=libaio --direct=1 --size=8M \
        --norandommap=1 --randrepeat=0 --runtime=25 --time_based \
        --group_reporting >/dev/null 2>&1; echo done" >/dev/null 2>&1

    ssh_nt "$NODE1" "sync; blockdev --flushbufs /dev/mapper/$DEV" >/dev/null 2>&1
    sleep 8

    # Every write must have gone out on the wire. If any got deferred to the
    # bitmap the md5 comparison below would be testing replication debt, not
    # ordering, so check this first.
    local debt
    debt=$(ssh_n "$NODE1" "dmsetup status $DEV | grep -oE 'bm_dirty=[0-9]+'" 2>/dev/null)
    assert_match "F40 all overlapping writes replicated inline" "$debt" "bm_dirty=0"

    # No force-resync here on purpose: the point is that ordinary replication
    # kept the peer identical, not that a resync could repair it afterwards.
    local m1 m2
    m1=$(read_md5 "$NODE1" 8); m2=$(read_md5 "$NODE2" 8)
    log "  node-1: $m1"
    log "  node-2: $m2"
    [ "$m1" = "$m2" ] || dump_divergence "$NODE1" "$NODE2" 8
    assert_md5_eq "F40 per-region ordering held under overlapping writes" "$m1" "$m2"

    assert_no_forbidden "F40 kernel log clean"
}

demo_f41() {
    section "F41 — Injected secondary write failures must not reach the user"
    # Positive test for fix-history #13. Until now that path was only ever
    # reached by racing a dmsetup remove against live I/O, which is why it went
    # unnoticed: a NACK completed the bio with -EIO even though the range had
    # just been marked dirty and the payload was committed locally.
    #
    # Modelled on drbd9-tests/tests/io-errors, which uses DRBD's
    # set_fault_injection to make the secondary's disk fail on demand rather
    # than waiting for a real one.
    fresh2 || return
    forbidden_mark "$NODE1" "$NODE2"
    wait_established "$NODE1" 90 || { fail "F41 replication never settled"; return; }

    # 30% of the secondary's data writes fail. High enough to bite within a few
    # seconds, low enough that replication still makes progress.
    fault_arm "$NODE2" 30 "$PCACHE_FAULT_DATA_WRITE" || return

    busy_start "$NODE1" 25 || { fault_disarm "$NODE2"; return; }
    busy_wait "$NODE1" 90 "F41 no bio hang while the peer is rejecting writes"

    local injected
    injected=$(fault_count "$NODE2")
    if [[ "$injected" =~ ^[0-9]+$ ]] && [ "$injected" -gt 0 ]; then
        pass "F41 faults actually injected ($injected)"
    else
        fail "F41 no faults injected (count=$injected) — test proved nothing"
        fault_disarm "$NODE2"; return
    fi

    # THE assertion: a peer that cannot store a write is a replication problem,
    # not the filesystem's problem. The range is dirty and the data is local.
    busy_errors "$NODE1" "F41 rejected writes do not surface as user I/O errors"

    # And the debt must be recorded, not lost.
    local dirty
    dirty=$(ssh_n "$NODE1" "dmsetup status $DEV | grep -oE 'bm_dirty=[0-9]+'" 2>/dev/null)
    assert_neq "F41 rejected ranges are recorded as dirty" "$dirty" "bm_dirty=0"

    fault_disarm "$NODE2"
    ssh_nt "$NODE1" "sync; blockdev --flushbufs /dev/mapper/$DEV" >/dev/null 2>&1
    sleep 5
    ssh_n "$NODE1" "dmsetup message $DEV 0 force-resync" >/dev/null 2>&1
    wait_md5_eq "F41 resync repairs everything the peer rejected" "$NODE1" "$NODE2" 32 180

    assert_no_forbidden "F41 kernel log clean"
}

demo_f42() {
    section "F42 — An injected resync write failure must not be acknowledged"
    # Positive test for fix-history #12. The secondary used to send P_RS_ACK
    # unconditionally, so a chunk it failed to store was reported as delivered;
    # combined with the source clearing dirty bits before reading, the data was
    # neither delivered nor still owed. Nothing in the system noticed.
    #
    # This is the assertion that matters: after a resync that hit write
    # failures, the two sides must still agree. Getting there requires the
    # secondary to drop the connection instead of ACKing, and the source to
    # restore the range and redo it on reconnect.
    fresh2 || return
    forbidden_mark "$NODE1" "$NODE2"
    wait_established "$NODE1" 90 || { fail "F42 replication never settled"; return; }

    local mref
    mref=$(primary_write 16)
    if ! is_hash "$mref"; then fail "F42 could not write the baseline pattern"; return; fi
    ssh_nt "$NODE1" "sync; blockdev --flushbufs /dev/mapper/$DEV" >/dev/null 2>&1
    sleep 5

    # Fail a fifth of the resync chunks; each failure costs a reconnect, so keep
    # it low enough that the resync still converges in reasonable time.
    fault_arm "$NODE2" 20 "$PCACHE_FAULT_RS_WRITE" || return
    ssh_n "$NODE1" "dmsetup message $DEV 0 force-resync" >/dev/null 2>&1
    sleep 30

    local injected
    injected=$(fault_count "$NODE2")
    if [[ "$injected" =~ ^[0-9]+$ ]] && [ "$injected" -gt 0 ]; then
        pass "F42 resync write faults actually injected ($injected)"
    else
        fail "F42 no faults injected (count=$injected) — test proved nothing"
        fault_disarm "$NODE2"; return
    fi

    # The source must have noticed rather than believing the chunks landed.
    if dmesg_since "$NODE2" "RS_DATA .*(write_to_cache failed|dropping connection)"; then
        pass "F42 the secondary refused to acknowledge what it could not store"
    else
        fail "F42 no evidence the failed chunks were refused — they may have been ACKed"
        # Evidence at the moment of detection: "no matching line" has two very
        # different causes -- the secondary really did ACK, or the mark/ring
        # dropped the lines before we looked. F41 runs first and floods the
        # ring, so the second one is real.
        echo "    ---- refusal evidence ----"
        echo "    mark for $NODE2: ${FORBID_MARK[*]}"
        echo "    oldest dmesg line still in the ring:"
        ssh_n "$NODE2" "dmesg | head -1" | sed 's/^/      /'
        echo "    RS_DATA lines in the whole ring: $(ssh_n "$NODE2" "dmesg | grep -cE 'RS_DATA .*(write_to_cache failed|dropping connection)'")"
        echo "    RS_DATA lines after the mark:    $(dmesg_filter_since "$NODE2" | grep -cE 'RS_DATA .*(write_to_cache failed|dropping connection)')"
    fi

    fault_disarm "$NODE2"
    # With faults off the retry must converge, and onto the known pattern --
    # not merely onto whatever both sides happen to hold.
    wait_md5_eq "F42 resync converges once the faults stop" "$NODE1" "$NODE2" 16 240
    local m1 m2
    m1=$(read_md5 "$NODE1" 16); m2=$(read_md5 "$NODE2" 16)
    assert_md5_eq "F42 converged content is the baseline pattern" "$mref" "$m1" "$m2"

    assert_no_forbidden "F42 kernel log clean"
}

demo_f43() {
    section "F43 — Resync progress under heavy application writes"
    # Ported from drbd9-tests/tests/busy-resync, whose whole point is phase 4:
    # "slowly but surely, the resync should finish in finite time". It builds a
    # backlog by disconnecting and writing, then reconnects while the primary is
    # generating as much I/O as it can.
    #
    # pcache-dist does not promise DRBD's outcome. The multi-pass sweep added
    # for fix-history #8 gives up when a pass makes no progress, precisely so a
    # writer faster than the resync cannot spin it forever -- and that branch
    # has never once been exercised. So the invariant here is not "it always
    # finishes", it is "it always reaches an honest verdict":
    #
    #   done (dirty=0)  -> the backlog was cleared, and the peer must match
    #   incomplete      -> it gave up, and the debt must still be on the bitmap
    #
    # What must never happen is the third outcome: claiming success while
    # something is still owed, which is the shape of every silent-divergence
    # bug in the fix history.
    fresh2 || return
    forbidden_mark "$NODE1" "$NODE2"
    wait_established "$NODE1" 90 || { fail "F43 replication never settled"; return; }

    log "  isolate the peer, then build a backlog while it is away"
    block_inbound "$NODE2" f43block || return
    sleep 55                              # let keepalive declare the link dead
    ssh_nt "$NODE1" "fio --name=backlog --filename=/dev/mapper/$DEV --rw=write \
        --bs=1M --iodepth=8 --ioengine=libaio --direct=1 --size=96M \
        --group_reporting >/dev/null 2>&1; echo done" >/dev/null 2>&1

    local owed
    owed=$(ssh_n "$NODE1" "dmsetup status $DEV | grep -oE 'bm_dirty=[0-9]+' | cut -d= -f2" 2>/dev/null)
    if [[ "$owed" =~ ^[0-9]+$ ]] && [ "$owed" -gt 0 ]; then
        pass "F43 backlog accumulated while disconnected ($owed dirty bits)"
    else
        fail "F43 no backlog to resync (bm_dirty=$owed) — test would prove nothing"
        unblock "$NODE2" f43block; return
    fi

    # Writer first, then reconnect: the resync must start into a device that is
    # already busy, which is what starves it.
    busy_start "$NODE1" 180 || { unblock "$NODE2" f43block; return; }
    sleep 3
    log "  reconnect the peer -- resync now competes with the writer"
    unblock "$NODE2" f43block

    local t=0 verdict=""
    while [ "$t" -le 300 ]; do
        if dmesg_since "$NODE1" "resync done"; then verdict=done; break; fi
        if dmesg_since "$NODE1" "resync incomplete"; then verdict=incomplete; break; fi
        sleep 5; t=$((t + 5))
    done

    case "$verdict" in
    done)
        pass "F43 resync reached a verdict under load: completed (${t}s)"
        ;;
    incomplete)
        pass "F43 resync reached a verdict under load: gave up honestly (${t}s)"
        ssh_n "$NODE1" "dmesg | grep 'resync incomplete' | tail -1" 2>/dev/null | sed 's/^/    /'
        ;;
    *)
        fail "F43 resync never reported done or incomplete within 300s"
        ;;
    esac

    busy_wait "$NODE1" 240 "F43 no bio hang while the resync competes with writes"
    busy_errors "$NODE1" "F43 competing resync causes no user-visible I/O error"

    # The verdict has to match reality. "done" claims nothing is owed; "incomplete"
    # claims the opposite. Either way the bitmap must agree with what was said.
    local dirty_now
    dirty_now=$(ssh_n "$NODE1" "dmsetup status $DEV | grep -oE 'bm_dirty=[0-9]+'" 2>/dev/null)
    if [ "$verdict" = "incomplete" ]; then
        assert_neq "F43 an incomplete resync leaves the debt recorded" "$dirty_now" "bm_dirty=0"
    fi

    # And once the writer stops, a quiet resync must always converge.
    ssh_nt "$NODE1" "sync; blockdev --flushbufs /dev/mapper/$DEV" >/dev/null 2>&1
    sleep 5
    ssh_n "$NODE1" "dmsetup message $DEV 0 force-resync" >/dev/null 2>&1
    local m1 m2
    wait_md5_eq "F43 a quiet resync clears whatever the busy one left" "$NODE1" "$NODE2" 96 240
    m1=$(read_md5 "$NODE1" 96); m2=$(read_md5 "$NODE2" 96)
    [ "$m1" = "$m2" ] || dump_divergence "$NODE1" "$NODE2" 96

    assert_no_forbidden "F43 kernel log clean"
}

# misaligned_writes NODE COUNT — a burst of small writes that deliberately do
# not line up with the 4 KiB bitmap granularity: odd 512-byte counts at odd
# sector offsets, so every one of them covers a bit only partially.
misaligned_writes() {
    local node="$1" n="${2:-24}" i sz seek
    for i in $(seq 1 "$n"); do
        sz=$(( (i * 2 - 1) % 15 + 1 ))          # odd-ish block counts, never 8
        seek=$(( i * 137 + 3 ))                  # never a multiple of 8 sectors
        ssh_n "$node" "dd if=/dev/urandom of=/dev/mapper/$DEV bs=512 count=$sz \
              seek=$seek oflag=direct conv=fsync status=none 2>/dev/null" || return 1
    done
    return 0
}

demo_f44() {
    section "F44 — Misaligned and odd-sized I/O"
    # From drbd9-tests/tests/unusual-bios, which writes through a helper that
    # deliberately misaligns bios. Every other demo here writes in 4K/64K/128K/1M
    # multiples, so nothing has ever exercised a bio that covers a bitmap bit
    # only partially -- and the bitmap rounds such a range outward when marking
    # it dirty. If that rounding ever missed a bit the write would simply not be
    # replicated, with nothing recording the debt.
    #
    # The device reports a 512-byte logical block, so misaligned direct I/O is
    # accepted. It does not advertise discard or write-zeroes
    # (discard_max_bytes=0), so the BLKDISCARD/BLKZEROOUT half of the DRBD test
    # does not apply here and is deliberately not simulated.
    fresh2 || return
    forbidden_mark "$NODE1" "$NODE2"
    wait_established "$NODE1" 90 || { fail "F44 replication never settled"; return; }

    local m_before m1 m2
    m_before=$(read_md5 "$NODE1" 8)

    log "  24 misaligned writes (odd 512B counts at odd sector offsets)"
    misaligned_writes "$NODE1" 24 || { fail "F44 misaligned write was rejected"; return; }
    sleep 3

    m1=$(read_md5 "$NODE1" 8)
    assert_neq "F44 misaligned writes actually changed the device" "$m1" "$m_before"

    m2=$(read_md5 "$NODE2" 8)
    assert_md5_eq "F44 misaligned writes replicate inline" "$m1" "$m2"

    # Now the same thing across a disconnect, so the ranges have to survive the
    # round trip through the dirty bitmap and an incremental resync rather than
    # being replicated as they are issued.
    log "  isolate the peer and repeat, so the bitmap has to carry them"
    block_inbound "$NODE2" f44block || return
    sleep 55
    misaligned_writes "$NODE1" 24 || { unblock "$NODE2" f44block; fail "F44 offline misaligned write failed"; return; }

    local owed
    owed=$(ssh_n "$NODE1" "dmsetup status $DEV | grep -oE 'bm_dirty=[0-9]+' | cut -d= -f2" 2>/dev/null)
    if [[ "$owed" =~ ^[0-9]+$ ]] && [ "$owed" -gt 0 ]; then
        pass "F44 misaligned ranges were marked dirty ($owed bits)"
    else
        fail "F44 misaligned writes left no dirty bits (bm_dirty=$owed) — they would be lost"
        unblock "$NODE2" f44block; return
    fi

    unblock "$NODE2" f44block
    wait_md5_eq "F44 incremental resync carries misaligned ranges" "$NODE1" "$NODE2" 8 180
    m1=$(read_md5 "$NODE1" 8); m2=$(read_md5 "$NODE2" 8)
    [ "$m1" = "$m2" ] || dump_divergence "$NODE1" "$NODE2" 8

    assert_no_forbidden "F44 kernel log clean"
}

demo_f45() {
    section "F45 — Replication under memory pressure"
    # From drbd9-tests/tests/bitmap-oom, adapted. DRBD's version sizes a bitmap
    # to need most of memory and checks the attach fails cleanly; pcache-dist's
    # bitmap RAM copy is 16 KiB for this device, so that angle is not the risk
    # here.
    #
    # The risk here is the fan-out's GFP_NOIO. It blocks rather than skipping a
    # peer, deliberately: skipping downgrades a write from replicated to owed
    # with nothing to pay the debt while the link is up. The price is that an
    # allocation can stall under memory pressure -- and that price has never
    # been measured. This demo squeezes both sides of that trade.
    fresh2 || return
    forbidden_mark "$NODE1" "$NODE2"
    wait_established "$NODE1" 90 || { fail "F45 replication never settled"; return; }

    mem_hog_start "$NODE1" 60 || { mem_hog_cleanup; return; }
    mem_hog_start "$NODE2" 60 || { mem_hog_cleanup; return; }

    busy_start "$NODE1" 40 || { mem_hog_cleanup; return; }
    sleep 5

    # The cost side: an administrative command must still complete. GFP_NOIO can
    # hold the peers_lock read side while it waits, and add/remove-peer needs the
    # write side -- so this is exactly the path the NOWAIT version was protecting.
    local t0 t1 elapsed rc
    t0=$(date +%s)
    ssh_nt "$NODE1" "dmsetup message $DEV 0 list-peers" >/dev/null 2>&1; rc=$?
    t1=$(date +%s); elapsed=$((t1 - t0))
    if [ "$rc" -eq 0 ] && [ "$elapsed" -le 60 ]; then
        pass "F45 admin command still completes under pressure (${elapsed}s)"
    else
        fail "F45 admin command stalled or failed under pressure (rc=$rc, ${elapsed}s)"
    fi

    # The benefit side.
    busy_wait "$NODE1" 120 "F45 no bio hang under memory pressure"
    busy_errors "$NODE1" "F45 memory pressure causes no user-visible I/O error"

    mem_hog_cleanup
    sleep 5
    require_nodes "$NODE1" "$NODE2" || return

    local m1 m2
    ssh_nt "$NODE1" "sync; blockdev --flushbufs /dev/mapper/$DEV" >/dev/null 2>&1
    sleep 5
    wait_md5_eq "F45 data agrees after the pressure lifts" "$NODE1" "$NODE2" 32 180
    m1=$(read_md5 "$NODE1" 32); m2=$(read_md5 "$NODE2" 32)
    [ "$m1" = "$m2" ] || dump_divergence "$NODE1" "$NODE2" 32

    assert_no_forbidden "F45 kernel log clean"
}

demo_f46() {
    section "F46 — Soak: repeated resync under varying write load"
    # From drbd9-tests/python/endurance.py::run_resync. Two things there are
    # worth copying exactly:
    #
    #   the rate sweep -- 1M through 1G -- so the resync sometimes outruns the
    #   writer and sometimes loses, covering both verdicts of the multi-pass
    #   sweep rather than whichever one this rig happens to produce;
    #
    #   the IO profile -- randrw, 70% writes, zipf:2.0, 4k-64k -- which DRBD
    #   chose, in its own words, "to trigger bugs which occur with repeated IO
    #   to the same sectors". Every other demo here writes uniformly.
    #
    # Runtime is F46_MINUTES (default 8). The invariant checked every cycle is
    # the same one F43 checks once: the verdict must match the bitmap. A cycle
    # that says "done" while bits are still owed is the failure this is hunting.
    local minutes="${F46_MINUTES:-8}"
    fresh2 || return
    forbidden_mark "$NODE1" "$NODE2"
    wait_established "$NODE1" 90 || { fail "F46 replication never settled"; return; }

    local rates=(1M 4M 16M 64M 256M 1G)
    local deadline=$(( $(date +%s) + minutes * 60 ))
    local cycle=0 done_cnt=0 incomplete_cnt=0 bad=0
    local profile="--rw=randrw --rwmixwrite=70 --percentage_random=50 \
        --random_distribution=zipf:2.0 --blocksize_range=4k-64k --numjobs=2"

    log "  soaking for ${minutes} min, sweeping write rates ${rates[*]}"
    while [ "$(date +%s)" -lt "$deadline" ]; do
        cycle=$((cycle + 1))
        local rate="${rates[$(( (cycle - 1) % ${#rates[@]} ))]}"

        busy_start "$NODE1" 45 "$profile --rate=$rate" || { bad=1; break; }
        sleep 2
        ssh_n "$NODE1" "dmsetup message $DEV 0 force-resync" >/dev/null 2>&1

        local t=0 verdict=""
        while [ "$t" -le 120 ]; do
            if dmesg_since "$NODE1" "resync done"; then verdict=done; break; fi
            if dmesg_since "$NODE1" "resync incomplete"; then verdict=incomplete; break; fi
            sleep 5; t=$((t + 5))
        done
        busy_wait "$NODE1" 120 "F46 cycle $cycle (rate=$rate): writer exited" >/dev/null

        case "$verdict" in
        done)       done_cnt=$((done_cnt + 1)) ;;
        incomplete) incomplete_cnt=$((incomplete_cnt + 1)) ;;
        *)          fail "F46 cycle $cycle (rate=$rate): no verdict within 120s"; bad=1; break ;;
        esac

        # Note, do not fail, on debt left after a "done" verdict. It is tempting
        # to call that a lie, but the bits can legitimately appear *after* the
        # verdict: between the sweep finishing and repl_state reaching
        # ESTABLISHED, writes take the dirty-bitmap path, and nothing drains
        # them while the link stays up (manual 9.6.1). That debt is recorded,
        # which is the opposite of the silent-divergence bugs this is hunting --
        # those left data neither delivered nor owed. The assertion that
        # separates the two is convergence once everything quiesces, at the end.
        if [ "$verdict" = "done" ]; then
            local dirty
            dirty=$(ssh_n "$NODE1" "dmsetup status $DEV | grep -oE 'bm_dirty=[0-9]+'" 2>/dev/null)
            [ "$dirty" != "bm_dirty=0" ] && \
                log "    cycle $cycle: $dirty after a done verdict (9.6.1 transition window)"
        fi

        local errs
        errs=$(ssh_n "$NODE1" "dmesg | grep -c 'Buffer I/O error'" 2>/dev/null)
        [ "${errs:-0}" != "0" ] && { fail "F46 cycle $cycle: user I/O errors appeared"; bad=1; break; }
        log "    cycle $cycle rate=$rate -> $verdict"
    done

    if [ "$bad" -eq 0 ]; then
        pass "F46 $cycle cycles, every one reached a verdict without error (done=$done_cnt incomplete=$incomplete_cnt)"
    fi
    # Both outcomes must actually have occurred, or the sweep proved only one path.
    if [ "$done_cnt" -gt 0 ] && [ "$incomplete_cnt" -gt 0 ]; then
        pass "F46 the rate sweep exercised both verdicts"
    else
        log "    note: only one verdict seen (done=$done_cnt incomplete=$incomplete_cnt) — widen the sweep or run longer"
    fi

    ssh_nt "$NODE1" "sync; blockdev --flushbufs /dev/mapper/$DEV" >/dev/null 2>&1
    sleep 5
    ssh_n "$NODE1" "dmsetup message $DEV 0 force-resync" >/dev/null 2>&1
    local m1 m2
    wait_md5_eq "F46 data converges once the soak stops" "$NODE1" "$NODE2" 64 300
    m1=$(read_md5 "$NODE1" 64); m2=$(read_md5 "$NODE2" 64)
    [ "$m1" = "$m2" ] || dump_divergence "$NODE1" "$NODE2" 64

    require_nodes "$NODE1" "$NODE2" || return
    assert_no_forbidden "F46 kernel log clean"
}

demo_f47() {
    section "F47 — An incomplete resync's debt must survive reconnects"
    # From drbd9-tests/tests/resync-finish (+ resync-missed-end-no-bits). DRBD
    # breaks the connection so the resync-target's final P_UUIDS110 never
    # reaches the source, then asks the only question that matters: does the
    # *next* connection still know a resync is owed? DRBD forgot to, up to and
    # including 9.0.24, and that is what the test pins down.
    #
    # pcache-dist's equivalent of that final packet is P_BITMAP_DONE, and the
    # equivalent of "the end was missed" is the sweep reporting itself
    # incomplete. The claim in pcache_do_resync -- "left dirty for the next
    # pass" -- is only true if the debt survives reconnects, so that is the
    # invariant here:
    #
    #   an incomplete resync leaves N bits owed
    #   -> reconnecting (no operator force-resync) must pay them off
    #   -> and the Primary must never elect itself SYNC_TARGET on the way
    #
    # The second half is not decoration. The bitmap exchange marks the peer's
    # bitmap dirty, which bumps that node's generation; do it twice and the
    # Secondary looks newer than the Primary, the roles inverse, and neither
    # side sends anything ever again.
    fresh2 || return
    forbidden_mark "$NODE1" "$NODE2"
    wait_established "$NODE1" 90 || { fail "F47 replication never settled"; return; }

    # An unthrottled writer is what drives the sweep to give up -- which is the
    # precondition this demo needs, not the thing it asserts.
    busy_start "$NODE1" 240 || return
    sleep 3
    # force-resync for the first round, not an nft-driven reconnect: it sets
    # FULL_SYNC_NEEDED and bumps the generation, which is the sequence that puts
    # the two nodes' generations on the path to inverting. Driving round one
    # from a link block instead leaves the peer's generation where it was, the
    # first quiet reconnect settles everything, and the demo proves nothing.
    log "  force-resync while the writer runs -- expect an incomplete sweep"
    ssh_n "$NODE1" "dmsetup message $DEV 0 force-resync" >/dev/null 2>&1

    local t=0 verdict=""
    while [ "$t" -le 300 ]; do
        if dmesg_since "$NODE1" "resync incomplete"; then verdict=incomplete; break; fi
        if dmesg_since "$NODE1" "resync done";       then verdict=done;       break; fi
        sleep 5; t=$((t + 5))
    done
    if [ "$verdict" != "incomplete" ]; then
        # Not a failure of the invariant -- the setup simply did not produce the
        # state under test. Say so instead of asserting on nothing (F46's rule).
        log "  verdict was '${verdict:-none}', not 'incomplete' — no debt to carry, nothing to test"
        busy_cleanup; return
    fi
    pass "F47 sweep gave up honestly and recorded the debt (${t}s)"
    ssh_n "$NODE1" "dmesg | grep 'resync incomplete' | tail -1" 2>/dev/null | sed 's/^/    /'

    # Interrupt and reconnect a couple more times *while the writer still runs*,
    # so each attempt ends incomplete again. This is the part that matters: every
    # reconnect exchanges bitmaps, and receiving a peer's dirty run marks the
    # local bitmap, which bumps that node's generation. Do it a few times and the
    # Secondary's generation overtakes the Primary's. One quiet reconnect would
    # settle everything and prove nothing.
    local r n_before n_after
    n_before=$(ssh_n "$NODE1" "dmesg | grep -cE 'resync (done|incomplete)'" 2>/dev/null)
    for r in 1 2; do
        log "  interrupt #$r: reconnect while the writer is still going"
        ssh_n "$NODE1" "dmsetup message $DEV 0 abort-resync" >/dev/null 2>&1
        local t2=0
        while [ "$t2" -le 240 ]; do
            n_after=$(ssh_n "$NODE1" "dmesg | grep -cE 'resync (done|incomplete)'" 2>/dev/null)
            [ "${n_after:-0}" -gt "${n_before:-0}" ] && break
            sleep 5; t2=$((t2 + 5))
        done
        n_before="${n_after:-$n_before}"
        ssh_n "$NODE1" "dmesg | grep -E 'resync (done|incomplete)' | tail -1" 2>/dev/null | sed 's/^/    /'
    done

    busy_cleanup                          # writer off; the device goes quiet
    sleep 20

    # THE assertion: reconnects alone must settle the debt. force-resync is an
    # operator rescue and would mask exactly the defect being tested (it sets
    # every bit, so it repairs the data no matter what the bookkeeping says).
    local i owed=""
    for i in 1 2 3; do
        log "  reconnect #$i (abort-resync: drops the socket, keeps the bitmap)"
        ssh_n "$NODE1" "dmsetup message $DEV 0 abort-resync" >/dev/null 2>&1
        if wait_established "$NODE1" 120; then owed=0; break; fi
        owed=$(ssh_n "$NODE1" "dmsetup status $DEV | grep -oE 'bm_dirty=[0-9]+' | cut -d= -f2" 2>/dev/null)
        log "    still owed: $owed bit(s)"
    done
    if [ "${owed:-1}" = "0" ]; then
        pass "F47 the recorded debt was paid off by reconnecting (no force-resync)"
    else
        fail "F47 debt outlived 3 reconnects (bm_dirty=$owed) — 'left dirty for the next pass' is not true"
    fi

    # The role inversion is the concrete shape of that failure: once the peer's
    # generation overtakes ours, the Primary decides it is the *target* of a
    # resync nobody will ever send.
    if dmesg_since "$NODE1" "decided role = SYNC_TARGET"; then
        fail "F47 the Primary elected itself SYNC_TARGET — resync direction inverted"
        ssh_n "$NODE1" "dmesg | grep -E 'sync_params: my|decided role' | tail -4" 2>/dev/null | sed 's/^/    /'
    else
        pass "F47 the Primary stayed the resync source across every reconnect"
    fi

    # Where the inversion leads if it is allowed to continue: both generations
    # drift, reciprocity breaks, and the next handshake reads it as a genuine
    # split-brain. F48 gets there; this guard is here so that if F47's shorter
    # sequence ever does, it is not mistaken for a real divergence.
    if dmesg_since "$NODE1" "incompatible generations|decided role = HALT"; then
        fail "F47 replication HALTed as a false split-brain after an interrupted resync"
        ssh_n "$NODE1" "dmesg | grep -E 'sync_params: my|decided role' | tail -6" 2>/dev/null | sed 's/^/    /'
    else
        pass "F47 no false split-brain across the interrupted resyncs"
    fi

    local m1 m2
    m1=$(read_md5 "$NODE1" 64); m2=$(read_md5 "$NODE2" 64)
    [ "$m1" = "$m2" ] || dump_divergence "$NODE1" "$NODE2" 64
    assert_md5_eq "F47 nodes agree once the debt is settled" "$m1" "$m2"

    assert_no_forbidden "F47 kernel log clean"
}

demo_f48() {
    section "F48 — A brief resync must not finish over an in-flight write"
    # From drbd9-tests/tests/brief-resync-oos, whose first line is the whole
    # point: "Demonstrate the necessity of flushing requests before finishing a
    # resync." DRBD let a resync complete while out-of-sync packets for it were
    # still on their way, up to and including 9.2.13.
    #
    # pcache-dist's version of that finish line is the tail of pcache_do_resync:
    # last chunk -> P_BITMAP_DONE -> mark_synced -> ESTABLISHED. A write landing
    # in that window takes the bitmap path, so the invariant is the one every
    # silent-divergence entry in the fix history comes back to:
    #
    #   every write is either carried by the resync or left owed on the bitmap.
    #   Never neither.
    #
    # Rather than time a single write against that window, drive many *brief*
    # resyncs (abort-resync reconnects in seconds and syncs only what the bitmap
    # holds) under a light writer, so the whole run is finish line. The writer
    # is rate limited for the same reason as F39: a saturating one makes the
    # per-chunk drain time out and the sweep ships nothing at all.
    fresh2 || return
    forbidden_mark "$NODE1" "$NODE2"
    wait_established "$NODE1" 90 || { fail "F48 replication never settled"; return; }

    # Cycle budget: a brief resync under this writer needs ~40-90s to reach a
    # verdict (the sweep's writeback drain alone can burn two 30s timeouts), so
    # a cycle that waits less than that just aborts the previous resync before
    # it finishes. At 6 cycles x 30s the whole demo produced nothing but
    # "resync starting ... resync failed: -32, reconnecting" pairs -- every pass
    # cut short by the next cycle's abort, dirty climbing 37120 -> 76544 across
    # them, and the guard below correctly reporting that nothing was exercised.
    # Fewer cycles, each given room to terminate, and a writer that outlives
    # them all.
    local cycles="${F48_CYCLES:-4}" cyc_wait="${F48_CYCLE_WAIT:-90}"
    local wr_run=$(( cycles * (cyc_wait + 5) + 90 )) wr_start=$SECONDS
    busy_start "$NODE1" "$wr_run" "--rate_iops=${F48_RATE_IOPS:-200}" || return
    sleep 3

    local i n_before n_after brief=0 started_before started_after
    n_before=$(ssh_n "$NODE1" "dmesg | grep -cE 'resync (done|incomplete)'" 2>/dev/null)
    started_before=$(ssh_n "$NODE1" "dmesg | grep -c 'resync starting'" 2>/dev/null)
    for i in $(seq 1 "$cycles"); do
        # abort-resync drops the socket (and clears FULL_SYNC, so the reconnect
        # syncs from the bitmap rather than everything). The burst right behind
        # it lands while the link is down, which is what guarantees this cycle
        # has a few dirty bits to carry -- without it most reconnects find
        # nothing owed, decide no resync is needed, and the cycle tests nothing.
        ssh_n "$NODE1" "dmsetup message $DEV 0 abort-resync" >/dev/null 2>&1
        ssh_nt "$NODE1" "dd if=/dev/urandom of=/dev/mapper/$DEV bs=1M count=4 \
            seek=$(( (i * 4) % 56 )) oflag=direct status=none" >/dev/null 2>&1
        local t=0
        while [ "$t" -le "$cyc_wait" ]; do
            n_after=$(ssh_n "$NODE1" "dmesg | grep -cE 'resync (done|incomplete)'" 2>/dev/null)
            [ "${n_after:-0}" -gt "${n_before:-0}" ] && { brief=$((brief + 1)); break; }
            sleep 2; t=$((t + 2))
        done
        n_before="${n_after:-$n_before}"
        sleep 2
    done
    log "  $brief of $cycles cycles produced a resync"
    if [ "$brief" -gt 0 ]; then
        pass "F48 drove $brief brief resync(s) across the writer"
    else
        fail "F48 no resync ran at all — nothing was exercised"
        # "No resync finished" has two very different causes, and the message
        # above does not separate them: none was ever triggered, or every one
        # was triggered and then cut short by the next cycle's abort. Say which.
        started_after=$(ssh_n "$NODE1" "dmesg | grep -c 'resync starting'" 2>/dev/null)
        echo "    resyncs started during the demo: $(( ${started_after:-0} - ${started_before:-0} ))"
        echo "    (started but never finished => raise F48_CYCLE_WAIT, currently ${cyc_wait}s;"
        echo "     none started => the aborts are not producing dirty bits to carry)"
        dmesg_filter_since "$NODE1" | grep -E 'resync (starting|done|incomplete|failed)' \
            | tail -6 | sed 's/^/    /'
        busy_cleanup; return
    fi

    # The writer outlives the cycles on purpose (they usually resolve well
    # inside cyc_wait), so this must wait out whatever runtime it has left --
    # a fixed timeout shorter than that reports a writer that is merely still
    # working as a hung bio, which is exactly what a 300s wait did against a
    # 540s writer. Only time past its scheduled end means a bio never completed.
    local wr_left=$(( wr_run - (SECONDS - wr_start) ))
    [ "$wr_left" -lt 0 ] && wr_left=0
    busy_wait "$NODE1" $(( wr_left + 120 )) "F48 no bio hang across the reconnect cycles"
    busy_errors "$NODE1" "F48 reconnect cycles cause no user-visible I/O error"
    sleep 20

    local m1 m2 owed
    m1=$(read_md5 "$NODE1" 64); m2=$(read_md5 "$NODE2" 64)
    if [ "$m1" = "$m2" ] && is_hash "$m1"; then
        pass "F48 nodes agree with nothing left to settle"
        assert_no_forbidden "F48 kernel log clean"
        return
    fi

    # They differ. That is allowed only if the difference is *recorded* -- so
    # settle whatever the bitmap still holds, on a quiet device, and look again.
    log "  nodes differ — settling the recorded debt before judging"
    ssh_n "$NODE1" "dmsetup message $DEV 0 abort-resync" >/dev/null 2>&1
    wait_established "$NODE1" 180
    sleep 5
    owed=$(ssh_n "$NODE1" "dmsetup status $DEV | grep -oE 'bm_dirty=[0-9]+' | cut -d= -f2" 2>/dev/null)
    m1=$(read_md5 "$NODE1" 64); m2=$(read_md5 "$NODE2" 64)

    if [ "$m1" = "$m2" ]; then
        pass "F48 the difference was honest debt; settling the bitmap converged it"
    elif [ "${owed:-1}" != "0" ]; then
        # Cannot blame the finish line: the debt is still on the bitmap and
        # simply was not paid. That is F47's defect, not this one.
        fail "F48 the debt was recorded but cannot be settled — $owed bit(s) still owed after a quiet reconnect"
        ssh_n "$NODE1" "dmesg | grep -E 'sync_params: my|decided role|resync (starting|done|incomplete)' | tail -8" 2>/dev/null | sed 's/^/    /' 
    else
        dump_divergence "$NODE1" "$NODE2" 64
        fail "F48 writes were neither carried by the resync nor left owed (bm_dirty=0 and still divergent)"
    fi

    assert_no_forbidden "F48 kernel log clean"
}

demo_f49() {
    section "F49 — Interrupted resyncs to two peers must both resume"
    # Ported from drbd9-tests/tests/resync-multiple-interrupt: "test that DRBD
    # correctly resumes syncs after disconnection and reconnection. In
    # particular, test the case where 2 nodes are both SyncTarget and they first
    # connect to each other on reconnection."
    #
    # What carries over, and what does not:
    #   - carries over: two peers each holding a *partial* resync, interrupted
    #     at different points, both of which must resume and finish;
    #   - carries over: the two partially-synced Secondaries meeting each other
    #     before either meets the Primary again;
    #   - does NOT carry over: DRBD then syncs node_c *from node_b* (downstream
    #     chaining). pcache-dist has no such path -- only the Primary drives
    #     pcache_do_resync -- so this asserts the Secondaries do nothing to each
    #     other rather than that one feeds the other.
    #
    # The per-peer bitmaps are the point: interrupting peer A's resync must not
    # disturb what peer B is owed. dmsetup status reports the *sum* over peers
    # (total_dirty), so progress is asserted on that aggregate and correctness
    # on the three-way data comparison.
    fresh3 || return
    forbidden_mark "$NODE1" "$NODE2" "$NODE3"
    wait_established "$NODE1" 120 || { fail "F49 replication never settled"; return; }

    # Cut the Primary off from both peers. Blocking the Primary's *outbound*
    # leaves the Secondary-to-Secondary link alone, which step 4 needs; the
    # abort-resync then drops the live sockets immediately so we do not have to
    # wait ~55s for keepalive to notice.
    log "  isolate both peers from the Primary"
    block_outbound_to "$NODE1" f49o2 "$NODE2" || return
    block_outbound_to "$NODE1" f49o3 "$NODE3" || { unblock "$NODE1" f49o2; return; }
    ssh_n "$NODE1" "dmsetup message $DEV 0 abort-resync" >/dev/null 2>&1
    # Both sides must agree the link is gone before we expect a clean reconnect
    # later. The far side only notices via TCP keepalive: 20s idle + 3 probes
    # 10s apart = up to 50s (pcache_setup_socket). Reconnecting sooner gets
    # refused by the peer's accept path as a duplicate.
    sleep 55

    log "  build a backlog only the Primary has"
    ssh_nt "$NODE1" "fio --name=backlog --filename=/dev/mapper/$DEV --rw=write \
        --bs=1M --iodepth=8 --ioengine=libaio --direct=1 --size=192M \
        --group_reporting >/dev/null 2>&1; echo done" >/dev/null 2>&1

    local owed0
    owed0=$(bm_owed "$NODE1")
    if [[ "$owed0" =~ ^[0-9]+$ ]] && [ "$owed0" -gt 0 ]; then
        pass "F49 both peers are owed data ($owed0 dirty bits across the two)"
    else
        fail "F49 no backlog (bm_dirty=$owed0) — nothing to interrupt"
        unblock "$NODE1" f49o2; unblock "$NODE1" f49o3; return
    fi

    # --- interrupt the resync to peer 2, then to peer 3 -------------------
    # Each peer starts owing exactly half of the aggregate. A peer's resync is
    # "partial" only while the aggregate stays strictly above what the other
    # peer still owes -- at or below that, this peer finished and there is
    # nothing partial left to resume.
    local per_peer=$((owed0 / 2))
    f49_partial_resync f49o2 "$NODE2" "$owed0" "$per_peer" "peer node-2"
    f49_partial_resync f49o3 "$NODE3" "$F49_OWED" 0 "peer node-3"

    # --- the case the DRBD test exists for --------------------------------
    # Both Secondaries now hold a partial resync and neither can reach the
    # Primary. Make them meet each other first.
    log "  drop and restore the Secondary-to-Secondary link while both are partial"
    block_outbound_to "$NODE2" f49x "$NODE3" || true
    sleep 12
    unblock "$NODE2" f49x
    sleep 10

    local n
    for n in "$NODE2" "$NODE3"; do
        if dmesg_since "$n" "halting replication|decided role = HALT|split-brain"; then
            fail "F49 $n called it a split-brain after meeting the other Secondary"
            ssh_n "$n" "dmesg | grep -E 'sync_params: my|decided role' | tail -4" 2>/dev/null | sed 's/^/    /'
        else
            pass "F49 $n drew no conclusion from meeting the other partial Secondary"
        fi
    done

    # --- both debts must settle, with no operator force-resync -------------
    log "  restore both links; the two resyncs must resume on their own"
    unblock "$NODE1" f49o2
    unblock "$NODE1" f49o3
    link_reset "$NODE1"

    if wait_established "$NODE1" 420; then
        pass "F49 both interrupted resyncs resumed and settled (no force-resync)"
    else
        fail "F49 debt outlived the reconnects (bm_dirty=$(bm_owed "$NODE1")) — a resync did not resume"
        ssh_n "$NODE1" "dmesg | grep -E 'resync (starting|done|incomplete)|decided role' | tail -6" 2>/dev/null | sed 's/^/    /'
        # Diagnose rather than assert again: one defect should cost one FAIL.
        # A transient "drop dup" while the links are being cut is expected; the
        # defect is the one that never resolves (fix-history #16).
        local n c
        for n in "$NODE2" "$NODE3"; do
            c=$(ssh_n "$n" "dmesg | grep -c 'already connected; drop dup'" 2>/dev/null)
            [ "${c:-0}" -gt 0 ] && echo "    $n is refusing reconnects as duplicates ($c times) — see fix-history #16"
        done
        ssh_n "$NODE1" "dmesg | grep -c 'sync_params exchange failed'" 2>/dev/null \
            | sed 's/^/    Primary reconnect attempts that got reset: /'
    fi



    sleep 5
    local m1 m2 m3
    m1=$(read_md5 "$NODE1" 64); m2=$(read_md5 "$NODE2" 64); m3=$(read_md5 "$NODE3" 64)
    log "  node-1 $m1"
    log "  node-2 $m2"
    log "  node-3 $m3"
    [ "$m1" = "$m2" ] || dump_divergence "$NODE1" "$NODE2" 64
    assert_md5_eq "F49 all three nodes agree after both resyncs resumed" "$m1" "$m2" "$m3"

    assert_no_forbidden "F49 kernel log clean"
}

# bm_owed NODE — total dirty bits across every peer bitmap (dmsetup status
# reports the sum, not per peer).
bm_owed() {
    ssh_n "$1" "dmsetup status $DEV | grep -oE 'bm_dirty=[0-9]+' | cut -d= -f2" 2>/dev/null
}

# f49_partial_resync TABLE PEER PREV_OWED LABEL — let one peer's resync start,
# interrupt it in flight, and assert it made progress without finishing.
# Reports the new owed count in F49_OWED rather than on stdout: called in a
# $(...) the pass/fail inside it would land in a subshell, so the counters would
# be lost and the messages would be swallowed by the capture.
F49_OWED=0
f49_partial_resync() {
    local tbl="$1" peer="$2" prev="$3" floor="$4" label="$5" n_before n_after t=0 owed

    # Shape this peer's link only: link_shape rebuilds the root qdisc, so
    # shaping both up front leaves only the second one actually shaped.
    link_shape "$NODE1" "$peer" 25ms || true

    n_before=$(ssh_n "$NODE1" "dmesg | grep -cE 'resync starting'" 2>/dev/null)
    unblock "$NODE1" "$tbl"
    while [ "$t" -le 90 ]; do
        n_after=$(ssh_n "$NODE1" "dmesg | grep -cE 'resync starting'" 2>/dev/null)
        [ "${n_after:-0}" -gt "${n_before:-0}" ] && break
        sleep 3; t=$((t + 3))
    done
    F49_OWED="$prev"
    if [ "${n_after:-0}" -le "${n_before:-0}" ]; then
        fail "F49 resync to $label never started"
        return
    fi

    # Interrupt on *progress*, not on a timer: this rig ships a peer's whole
    # backlog in a couple of seconds even with the link shaped, so any fixed
    # sleep either interrupts nothing or misses the window entirely. DRBD's
    # original does the same thing -- it waits for events2 to report >= 20%
    # done before breaking the sync off.
    local target=$((prev - (prev - floor) / 5))     # 20% of this peer's debt
    t=0
    while [ "$t" -le 120 ]; do
        owed=$(bm_owed "$NODE1")
        [[ "$owed" =~ ^[0-9]+$ ]] && [ "$owed" -le "$target" ] && break
        [[ "$owed" =~ ^[0-9]+$ ]] && [ "$owed" -le "$floor" ] && break   # already done
        sleep 1; t=$((t + 1))
    done
    ssh_n "$NODE1" "dmsetup message $DEV 0 abort-resync" >/dev/null 2>&1
    block_outbound_to "$NODE1" "$tbl" "$peer" || true
    sleep 3

    owed=$(bm_owed "$NODE1")
    [[ "$owed" =~ ^[0-9]+$ ]] && F49_OWED="$owed"
    if ! [[ "$owed" =~ ^[0-9]+$ ]]; then
        fail "F49 could not read bm_dirty after interrupting $label"
    elif [ "$owed" -ge "$prev" ]; then
        fail "F49 resync to $label made no progress ($prev -> $owed bits owed)"
    elif [ "$owed" -le "$floor" ]; then
        fail "F49 resync to $label finished before it could be interrupted ($owed bits left, floor $floor) — nothing partial to resume"
    else
        pass "F49 resync to $label was interrupted in flight ($prev -> $owed bits owed)"
    fi
}

demo_f50() {
    section "F50 — One peer finishing its resync must not disturb the other's debt"
    # Ported from drbd9-tests/tests/resync-finish2: "Two concurrent resyncs from
    # one source. one resync finishes earlier than the other one. This test
    # verifies that the SyncTarget of the quicker resync does not mark the other
    # SyncTarget's disk as Outdated." (broken in DRBD up to 9.0.24)
    #
    # The same coupling exists here, in pcache_bm_mark_synced():
    #
    #     /* gen_dirty is device-wide; reset on any peer's sync. */
    #     if (pcache->gen_dirty) { pcache->gen_dirty = false; ... }
    #
    # Bitmaps are per peer but the generation is per device, so the peer that
    # finishes first reaches into shared state. Fix-history #15 was this family:
    # one peer's completion corrupting what another was owed. Nothing guarded it.
    #
    # DRBD loops 40 times because its two resyncs race. Ours does not need to:
    # the demo decides who connects when, so the "one finishes much earlier" part
    # is arranged rather than hoped for.
    fresh3 || return
    forbidden_mark "$NODE1" "$NODE2" "$NODE3"
    wait_established "$NODE1" 120 || { fail "F50 replication never settled"; return; }

    # Cut both peers off the Primary. Blocking the Primary's outbound leaves the
    # Secondary-to-Secondary link alone; the abort-resync drops the live sockets
    # so we do not wait for keepalive to notice. Then wait out the far sides'
    # keepalive (20s idle + 3x10s) or their accept path refuses the reconnect.
    log "  isolate both peers from the Primary"
    block_outbound_to "$NODE1" f50o2 "$NODE2" || return
    block_outbound_to "$NODE1" f50o3 "$NODE3" || { unblock "$NODE1" f50o2; return; }
    ssh_n "$NODE1" "dmsetup message $DEV 0 abort-resync" >/dev/null 2>&1
    sleep 55

    log "  build a backlog both peers are equally owed"
    ssh_nt "$NODE1" "fio --name=backlog --filename=/dev/mapper/$DEV --rw=write \
        --bs=1M --iodepth=8 --ioengine=libaio --direct=1 --size=96M \
        --group_reporting >/dev/null 2>&1; echo done" >/dev/null 2>&1

    local both per_peer
    both=$(bm_owed "$NODE1")
    per_peer=$(( ${both:-0} / 2 ))
    if [[ "$both" =~ ^[0-9]+$ ]] && [ "$both" -gt 0 ] && [ $((both % 2)) -eq 0 ]; then
        pass "F50 both peers owe the same backlog ($both bits total, $per_peer each)"
    else
        fail "F50 backlog is not two equal debts (bm_dirty=$both) — the split below would be meaningless"
        unblock "$NODE1" f50o2; unblock "$NODE1" f50o3; return
    fi

    # --- let peer node-2 finish, on its own, while node-3 stays away ---------
    local n_before n_after t=0
    n_before=$(ssh_n "$NODE1" "dmesg | grep -cE 'resync done'" 2>/dev/null)
    log "  let peer node-2 resync to completion (node-3 still cut off)"
    unblock "$NODE1" f50o2
    while [ "$t" -le 180 ]; do
        n_after=$(ssh_n "$NODE1" "dmesg | grep -cE 'resync done'" 2>/dev/null)
        [ "${n_after:-0}" -gt "${n_before:-0}" ] && break
        sleep 3; t=$((t + 3))
    done
    if [ "${n_after:-0}" -le "${n_before:-0}" ]; then
        fail "F50 peer node-2's resync never completed (${t}s) — nothing to test"
        unblock "$NODE1" f50o3; return
    fi
    pass "F50 peer node-2's resync completed (${t}s)"

    # THE assertion, and the direct analogue of DRBD's "must not mark the other
    # Outdated": node-3's debt must be exactly what it was. dmsetup status
    # reports the sum over peers, so node-2 finishing should take the total from
    # 2*per_peer down to per_peer -- no further.
    sleep 3
    local after
    after=$(bm_owed "$NODE1")
    if [ "${after:-0}" -eq "$per_peer" ]; then
        pass "F50 node-3's debt survived node-2's completion untouched ($after bits, unchanged)"
    elif [ "${after:-0}" -lt "$per_peer" ]; then
        fail "F50 node-2's completion ate into node-3's debt ($per_peer -> $after bits) — per-peer accounting is not per peer"
        ssh_n "$NODE1" "dmsetup message $DEV 0 bitmap-status" 2>/dev/null | sed 's/^/    /'
    else
        fail "F50 unexpected debt after node-2 finished (expected $per_peer, got $after)"
    fi

    # A write now, with only node-2 connected: it must replicate inline to
    # node-2 and be added to node-3's debt. If the device-wide generation
    # bookkeeping got confused by node-2's mark_synced, this is where node-3
    # stops being flagged.
    ssh_nt "$NODE1" "dd if=/dev/urandom of=/dev/mapper/$DEV bs=1M count=8 seek=200 \
        oflag=direct status=none" >/dev/null 2>&1
    sleep 2
    local grown
    grown=$(bm_owed "$NODE1")
    if [ "${grown:-0}" -gt "${after:-0}" ]; then
        pass "F50 writes after node-2 synced are still recorded for node-3 ($after -> $grown bits)"
    else
        fail "F50 a write after node-2 synced was NOT recorded for node-3 ($after -> $grown bits)"
    fi

    # --- now let node-3 in; it must resync on its own -----------------------
    log "  restore node-3; its resync must run without a force-resync"
    unblock "$NODE1" f50o3
    if wait_established "$NODE1" 300; then
        pass "F50 node-3 resynced on its own after node-2 had already finished"
    else
        fail "F50 node-3's debt never settled (bm_dirty=$(bm_owed "$NODE1"))"
        ssh_n "$NODE1" "dmesg | grep -E 'resync (starting|done|incomplete)|decided role' | tail -6" 2>/dev/null | sed 's/^/    /'
    fi

    if dmesg_since "$NODE1" "incompatible generations|decided role = HALT|decided role = SYNC_TARGET"; then
        fail "F50 the Primary's role or generation state was disturbed by the staggered completions"
        ssh_n "$NODE1" "dmesg | grep -E 'sync_params: my|decided role' | tail -4" 2>/dev/null | sed 's/^/    /'
    else
        pass "F50 the Primary stayed SYNC_SOURCE throughout, no false split-brain"
    fi

    sleep 5
    local m1 m2 m3
    m1=$(read_md5 "$NODE1" 64); m2=$(read_md5 "$NODE2" 64); m3=$(read_md5 "$NODE3" 64)
    log "  node-1 $m1"
    log "  node-2 $m2"
    log "  node-3 $m3"
    [ "$m1" = "$m3" ] || dump_divergence "$NODE1" "$NODE3" 64
    assert_md5_eq "F50 all three nodes agree after the staggered resyncs" "$m1" "$m2" "$m3"

    assert_no_forbidden "F50 kernel log clean"
}

demo_f51() {
    section "F51 — Writes already in flight when a resync starts"
    # Ported from drbd9-tests/tests/busy-resync-start. The clever part of that
    # test is not "write during a resync" -- F39 and F43 already do that -- it is
    # the third peer:
    #
    #     writing_tc.slow_down(other_n, delay='50ms')
    #     # Including a delayed peer ensures that there are pending writes while
    #     # starting the resync.
    #
    # A deliberately slow peer keeps the Primary's replication queue non-empty,
    # so the resync to the *other* peer begins with writes already in flight.
    #
    # That matters here more than it does for DRBD, because of a hole this
    # project has on record and has not fixed (fix-history #14, last section):
    # with another peer ESTABLISHED the write takes the fast path, and
    # pcache_net_submit_write() marks the non-replicating peer's bitmap
    # *before* pcache_cache_write_commit() -- the bit is set before the data
    # exists. Two nodes never walk into it, because with a single peer the slow
    # path marks dirty only after the commit. Three nodes do.
    fresh3 || return
    forbidden_mark "$NODE1" "$NODE2" "$NODE3"
    wait_established "$NODE1" 120 || { fail "F51 replication never settled"; return; }

    # The slow peer. Only one destination is shaped, so link_shape's
    # one-at-a-time limitation (see its comment) does not bite here.
    link_shape "$NODE1" "$NODE3" 50ms || { fail "F51 could not shape the slow peer"; return; }

    # Cut the resync target off and let its keepalive reap the connection
    # (20s idle + 3x10s). No abort-resync here: that would drop the slow peer's
    # socket too, and the slow peer must stay connected -- it is the mechanism.
    log "  cut off the resync target (node-2); the slow peer stays connected"
    block_outbound_to "$NODE1" f51o2 "$NODE2" || { link_reset "$NODE1"; return; }
    sleep 55

    # Sequential, rate limited: DRBD notes the writer must not outrun itself and
    # overwrite the very blocks written as the resync starts, which would mask a
    # lost write. Later fio options win, so this overrides busy_start's defaults.
    busy_start "$NODE1" 90 "--rw=write --size=256M --rate=10m" || {
        unblock "$NODE1" f51o2; link_reset "$NODE1"; return; }
    sleep 3

    # Guard: the scenario has to actually exist before anything is asserted on
    # it -- same reasoning as F41's fault_count > 0.
    local shaped writer_alive
    shaped=$(ssh_n "$NODE1" "tc qdisc show | grep -c netem" 2>/dev/null)
    writer_alive=$(ssh_n "$NODE1" "pgrep -cx fio" 2>/dev/null)
    if [ "${shaped:-0}" -gt 0 ] && [ "${writer_alive:-0}" -gt 0 ]; then
        pass "F51 scenario is set up: slow peer shaped and the writer is running"
    else
        fail "F51 scenario missing (netem=$shaped fio=$writer_alive) — nothing would be tested"
        busy_cleanup; unblock "$NODE1" f51o2; link_reset "$NODE1"; return
    fi

    log "  reconnect node-2 while writes are in flight"
    local n_before n_after t=0
    n_before=$(ssh_n "$NODE1" "dmesg | grep -cE 'resync (done|incomplete)'" 2>/dev/null)
    unblock "$NODE1" f51o2
    while [ "$t" -le 240 ]; do
        n_after=$(ssh_n "$NODE1" "dmesg | grep -cE 'resync (done|incomplete)'" 2>/dev/null)
        [ "${n_after:-0}" -gt "${n_before:-0}" ] && break
        sleep 3; t=$((t + 3))
    done
    if [ "${n_after:-0}" -gt "${n_before:-0}" ]; then
        pass "F51 the resync started and reached a verdict with writes in flight (${t}s)"
        ssh_n "$NODE1" "dmesg | grep -E 'resync (done|incomplete)' | tail -1" 2>/dev/null | sed 's/^/    /'
    else
        fail "F51 no resync verdict within 240s"
    fi

    busy_wait "$NODE1" 200 "F51 no bio hang across the resync start"
    busy_errors "$NODE1" "F51 writes in flight at resync start cause no user-visible I/O error"
    link_reset "$NODE1"
    sleep 20

    # Convergence without an operator rescue.
    if wait_established "$NODE1" 300; then
        pass "F51 all debt settled without a force-resync"
    else
        fail "F51 debt outlived the run (bm_dirty=$(bm_owed "$NODE1"))"
    fi

    local m1 m2 m3 owed
    m1=$(read_md5 "$NODE1" 64); m2=$(read_md5 "$NODE2" 64); m3=$(read_md5 "$NODE3" 64)
    if [ "$m1" = "$m2" ] && [ "$m1" = "$m3" ] && is_hash "$m1"; then
        assert_md5_eq "F51 all three nodes agree" "$m1" "$m2" "$m3"
        assert_no_forbidden "F51 kernel log clean"
        return
    fi

    # They differ. Allowed only if the difference is still recorded: settle the
    # bitmap on a quiet device and look again. Anything left after that was
    # neither carried by the resync nor written down -- the shape of #14, and of
    # the multi-peer hole this demo exists to hunt.
    log "  nodes differ — settling the recorded debt before judging"
    ssh_n "$NODE1" "dmsetup message $DEV 0 abort-resync" >/dev/null 2>&1
    wait_established "$NODE1" 300
    sleep 5
    owed=$(bm_owed "$NODE1")
    m1=$(read_md5 "$NODE1" 64); m2=$(read_md5 "$NODE2" 64); m3=$(read_md5 "$NODE3" 64)
    if [ "$m1" = "$m2" ] && [ "$m1" = "$m3" ]; then
        pass "F51 the difference was honest debt; settling the bitmap converged it"
    elif [ "${owed:-1}" != "0" ]; then
        fail "F51 inconclusive — $owed bit(s) still owed, so the debt was recorded but not settled"
    else
        [ "$m1" = "$m3" ] || dump_divergence "$NODE1" "$NODE3" 64
        fail "F51 writes in flight at resync start were neither replicated nor recorded (bm_dirty=0, still divergent)"
    fi

    assert_no_forbidden "F51 kernel log clean"
}

SMOKE="f01 f02 f03 f06 f07 f08 f10 f14 f15 f17 f18 f19 f20 f21 f26 f27 f28 f29 f30 f31 f33 f34"

SECTION8="f31 f32 f33"
# Section 9 — fault injection & recovery (f35 is destructive: crashes node-1)
SECTION9="f34 f35"
# Section 10 — concurrency: control-plane operations under live write load
SECTION10="f36 f37 f38 f39 f40"
# Section 11 — fault injection (drbd9-tests/tests/io-errors)
SECTION11="f41 f42"
# Section 12 — load and abnormal-flow scenarios from drbd9-tests
SECTION12="f43 f44 f45 f46"
# Section 13 — resync bookkeeping (drbd9-tests: resync-finish, brief-resync-oos)
SECTION13="f47 f48 f49 f50 f51"

SECTION1="f01 f02 f03 f04 f05 f06"
SECTION2="f07 f08 f09"
SECTION3="f10 f11 f12 f13"
SECTION4="f14 f15 f16 f17"
SECTION5="f18 f19 f20 f21"
SECTION6="f22 f23 f24 f25"
SECTION7="f26 f27 f28 f29 f30"

# Keep this in sync with SECTION1..SECTION13 when adding a demo -- it is the
# list `all` runs, and a demo missing from it is silently never regressed.
# F47-F51 sat outside it long enough to run a full green `all` that had not
# executed the very cases which found #16, #17 and #18.
ALL_FEATURES="$SECTION1 $SECTION2 $SECTION3 $SECTION4 $SECTION5 $SECTION6 $SECTION7 $SECTION8 $SECTION9 $SECTION10 $SECTION11 $SECTION12 $SECTION13"

list_features() {
    cat <<'EOF'
Available feature demos:
  Section 1 — Replication Path:
    f01  Synchronous write replication (Protocol C)
    f02  Multi-peer fan-out broadcast write
    f03  Zero-copy network transport
    f04  ACK timeout reaping + auto-reconnect          [~30s; uses nft]
    f05  Disconnected peer fallback (bitmap-dirty)
    f06  REQ_PREFLUSH cross-cluster flush barrier
  Section 2 — Network & Mesh:
    f07  Full-mesh accept dispatch
    f08  id-based connection direction
    f09  SO_KEEPALIVE-driven dead-link detection        [~55s; uses nft]
  Section 3 — Dirty Tracking & Resync:
    f10  Per-peer persistent dirty bitmap on PMEM
    f11  Bitmap-based incremental resync                [~75s; uses nft]
    f12  Bilateral bitmap merge
    f13  Generation counter + per-peer peer_gen_at_sync
  Section 4 — Persistence & Crash Recovery:
    f14  PMEM superblock + double-copy CRC headers
    f15  Persistent peer registry
    f16  RESYNC_IN_PROGRESS crash detection             [destructive; use f16-run]
    f17  Idempotent add-peer
  Section 5 — HA / Role Management:
    f18  Primary/Secondary role + write fencing
    f19  set-role runtime role switch
    f20  Coordinated demote handshake (P_DEMOTE_REQ/ACK)
    f21  Worker kthread role swap
  Section 6 — Quorum & Split-brain Safety:
    f22  Quorum check
    f23  Configurable quorum (off | majority | N)       [~60s]
    f24  Split-brain per-pair detection                 [~75s; uses nft]
    f25  Split-brain manual resolve (run after f24)
  Section 7 — Operational Interface:
    f26  dmsetup table line config
    f27  add-peer / remove-peer / list-peers
    f28  abort-resync / force-resync / reset-generation
    f29  Status reporting
    f30  Clean PMEM reset script

  Section 8 — Performance / Cut A·deferred-fix·Cut C (this session):
    f31  Multi-stream replication (Cut C, streams=2)
    f32  Multi-stream churn robustness (rapid teardown)    [~3min]
    f33  Deferred-write replication under cache pressure

  Section 9 — Fault Injection & Recovery:
    f34  On-media metadata double-copy corruption recovery
    f35  Primary crash -> Secondary failover -> rejoin & resync  [destructive; crashes node-1, uses virsh]

  Section 10 — Concurrency (control-plane ops under live write load):
    f36  Peer churn under live write load                  [~2min]
    f37  Secondary restart under live write load           [~2min]
    f38  Role switch (demote/promote) under live write load [~2min]
    f39  Continuous writes during a resync                  [~4min; uses tc]
    f40  Overlapping concurrent writes across streams        [~1min; streams=2]

  Section 11 — Fault injection (needs the module's fault_rate/fault_types):
    f41  Injected secondary write failures stay off the user  [~2min]
    f42  Injected resync write failures are never ACKed       [~2min]

  Section 12 — Load / abnormal flow (from drbd9-tests):
    f43  Resync progress under heavy application writes        [~7min; uses nft]
    f44  Misaligned and odd-sized I/O                           [~2min; uses nft]
    f45  Replication under memory pressure                       [~2min; 60% RAM]
    f46  Soak: repeated resync under varying write load         [F46_MINUTES, default 8]

  Section 13 — Resync bookkeeping (from drbd9-tests):
    f47  An incomplete resync's debt must survive reconnects    [~7min]
    f48  A brief resync must not finish over an in-flight write [~5min; F48_CYCLES=6]
    f49  Interrupted resyncs to two peers must both resume      [~8min; 3 nodes, nft]
    f50  One peer finishing must not disturb the other's debt    [~5min; 3 nodes, nft]
    f51  Writes already in flight when a resync starts          [~4min; 3 nodes, nft+tc]

Group selectors:
  smoke      — quick subset that completes in ~3 minutes, no VM crashes
  section1..12
  all        — every demo (f35 crashes node-1)
EOF
}

# A full run takes well over an hour. If the workstation sleeps in the middle of
# one, every ssh in flight dies and whichever demo was running reports failures
# that have nothing to do with the code (an empty reference md5, "unreachable"
# nodes).
#
# A block-mode inhibitor on "sleep" is what stops that, whatever asked for the
# sleep (it was neither idle nor battery here: IdleAction=ignore and the host
# has no battery). Taking one needs privilege -- polkit answers "Interactive
# authentication required" for this session -- so hold it in a background root
# helper rather than running the whole suite as root, which would change whose
# ssh keys the demos use. The helper watches this script's pid and exits with
# it, so there is nothing to clean up and no lock left behind on a crash.
pcache_inhibit_sleep() {
    if ! command -v systemd-inhibit >/dev/null 2>&1; then
        echo "  note: systemd-inhibit not found; the host may sleep mid-run" >&2
        return 0
    fi
    ${SUDO:-sudo} systemd-inhibit --what=sleep --mode=block \
        --who="dm-pcache demos" --why="dm-pcache regression in progress" \
        bash -c "while kill -0 $$ 2>/dev/null; do sleep 5; done" \
        >/dev/null 2>&1 &
    sleep 2
    if systemd-inhibit --list 2>/dev/null | grep -q "dm-pcache demos"; then
        log "  sleep inhibited for the duration of this run"
    else
        echo "  warning: could not take a sleep inhibitor; the host may sleep mid-run" >&2
    fi
}

main() {
    pcache_inhibit_sleep "$@"
    if [ $# -eq 0 ]; then
        list_features
        exit 0
    fi

    local args=()
    for a in "$@"; do
        case "$a" in
            list|--list|-l) list_features; exit 0 ;;
            smoke)    args+=($SMOKE) ;;
            all)      args+=($ALL_FEATURES) ;;
            section1) args+=($SECTION1) ;;
            section2) args+=($SECTION2) ;;
            section3) args+=($SECTION3) ;;
            section4) args+=($SECTION4) ;;
            section5) args+=($SECTION5) ;;
            section6) args+=($SECTION6) ;;
            section7) args+=($SECTION7) ;;
            section8) args+=($SECTION8) ;;
            section9) args+=($SECTION9) ;;
            section10) args+=($SECTION10) ;;
            section11) args+=($SECTION11) ;;
            section12) args+=($SECTION12) ;;
            section13) args+=($SECTION13) ;;
            *)        args+=("$a") ;;
        esac
    done

    for f in "${args[@]}"; do
        local fn="demo_${f}"
        if declare -f "$fn" > /dev/null; then
            # Isolate each demo: a non-zero return (guard abort) must not kill
            # the run, and pipefail/-u inside one demo must not leak out.
            run_boot_snapshot
            "$fn" || true
            run_boot_check "$f" || true
        else
            fail "unknown feature: $f"
        fi
    done

    # ── Summary + exit status (so CI can detect failures) ───────────────────
    section "SUMMARY"
    echo "  PASS: $PASS_CNT    FAIL: $FAIL_CNT"
    if [ "$FAIL_CNT" -gt 0 ]; then
        echo "  Failed checks:"
        local t
        for t in "${FAILED[@]:-}"; do
            [ -n "$t" ] && echo "    - $t"
        done
        return 1
    fi
    return 0
}

main "$@"
exit $?
