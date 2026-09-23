#!/bin/bash
#
# Transparent hugepage (THP) boot-time test for VFIO DMA mapping.
#
# vfio-pci-device-dma-map exercises the same DMA mapping path a VM
# walks at boot, so its wall-clock time is a proxy for VM boot cost.
#
# The test:
#   1. Sets THP to "always" and runs vfio-pci-device-dma-map 5 times,
#      averaging the wall-clock time. This average is the baseline.
#   2. Runs a single timed pass with THP "madvise" and a single timed
#      pass with THP "never".
#   3. A test FAILS if its boot time is more than 50% bigger
#      than the "always" baseline average. Both madvise and never are
#      always run and reported, even if one of them fails.
#
# The script exits non-zero if any configuration fails.
#
# Usage: ./thp-boot-time.sh <ssss:bb:dd.f>
#
#   <ssss:bb:dd.f>  PCI address of a device bound to vfio-pci
#
# Requires root.
#

set -u

me=${0##*/}
dir=$(dirname "$0")

device=${1:-}
baseline_runs=${2:-3}

if [ -z "$device" ]; then
    echo "usage: $me <ssss:bb:dd.f>"
    exit 1
fi

if [ "$(id -u)" -ne 0 ]; then
    echo "$me: must be run as root"
    exit 1
fi

bin="$dir/vfio-pci-device-dma-map"
if [ ! -x "$bin" ]; then
    echo "$me: $bin not built, run 'make vfio-pci-device-dma-map' first"
    exit 1
fi

sysdev=/sys/bus/pci/devices/$device
if [ ! -e "$sysdev" ]; then
    echo "$me: device $device not found"
    exit 1
fi

# The DMA-map timing is only meaningful on a device with a large BAR:
# a small BAR is mapped too quickly to reveal any THP-related
# difference between always/madvise/never. Require the largest BAR to
# be at least 32 GiB. Each line of the sysfs 'resource' file is
# "start end flags" in hex; an unused BAR reads as all zeros.
min_bar=$((32 * 1024 * 1024 * 1024))
bar_bytes=$(awk '
    BEGIN { max = 0 }
    NR <= 6 {
        start = strtonum($1); end = strtonum($2)
        if (end > start) {
            size = end - start + 1
            if (size > max) max = size
        }
    }
    END { printf "%d", max }
' "$sysdev/resource")

bar_hr=$(numfmt --to=iec-i --suffix=B "$bar_bytes")
if [ "$bar_bytes" -lt "$min_bar" ]; then
    echo "$me: largest BAR is ${bar_hr}, need >= 32GiB"
    echo "$me: BAR too small to measure a THP performance difference; aborting"
    exit 77
fi
echo "Largest BAR: ${bar_hr} (>= 32GiB required)"
echo

thp_file=/sys/kernel/mm/transparent_hugepage/enabled
if [ ! -w "$thp_file" ]; then
    echo "$me: cannot write $thp_file (THP not available?)"
    exit 1
fi

# The active THP policy is the token shown in [brackets]; save it so we
# can restore the system on exit.
saved_thp=$(sed -n 's/.*\[\(.*\)\].*/\1/p' "$thp_file")

restore_thp() {
    if [ -n "$saved_thp" ]; then
        echo "$saved_thp" > "$thp_file" 2>/dev/null
    fi
}
trap restore_thp EXIT

# TIMEFORMAT='%R' makes the 'time' builtin print only the real
# (wall-clock) elapsed seconds as a plain float, easy to sum/average.
export TIMEFORMAT='%R'

set_thp() {
    echo "$1" > "$thp_file"
}

# Run vfio-pci-device-dma-map once, timing it, and echo the elapsed
# seconds on stdout. Exits the script if the binary fails.
time_one_run() {
    local t rc
    # The binary's own stdout/stderr are discarded inside the group;
    # 'time' writes its output to the group's stderr, which is then
    # redirected to stdout and captured into $t.
    t=$( { time "$bin" "$device" >/dev/null 2>&1; } 2>&1 )
    rc=$?
    if [ "$rc" -ne 0 ]; then
        echo "vfio-pci-device-dma-map failed (exit $rc)" >&2
        exit 1
    fi
    echo "$t"
}

# --- Baseline: THP "always", averaged over $baseline_runs runs -------

echo "=== baseline: THP always ($baseline_runs runs) ==="
set_thp always
echo "THP enabled: $(cat "$thp_file")"

total=0
for i in $(seq 1 "$baseline_runs"); do
    t=$(time_one_run)
    printf "run %d: %ss\n" "$i" "$t"
    total=$(awk -v a="$total" -v b="$t" 'BEGIN { printf "%.6f", a + b }')
done

avg=$(awk -v tot="$total" -v n="$baseline_runs" 'BEGIN { printf "%.6f", tot / n }')
# A configuration fails if it takes more than 50% longer than baseline.
threshold=$(awk -v a="$avg" 'BEGIN { printf "%.6f", a * 1.5 }')

echo
echo "always average:  ${avg}s"
echo "fail threshold:  ${threshold}s (average + 50%)"
echo

# --- Single-run comparisons: THP "madvise" and "never" ---------------

fail=0
fail_mode=""

check_mode() {
    local mode=$1 t verdict
    echo "=== THP $mode (1 run) ==="
    set_thp "$mode"
    echo "THP enabled: $(cat "$thp_file")"
    t=$(time_one_run)
    if awk -v v="$t" -v th="$threshold" 'BEGIN { exit !(v > th) }'; then
        verdict="FAIL"
        fail=$((fail + 1))
	fail_mode+="  - ${mode} ${t}s\n"
    else
        verdict="PASS"
    fi

    printf "%s time: %ss  (threshold %ss)  %s\n\n" "$mode" "$t" "$threshold" "$verdict"
}

# Run both regardless of individual outcomes.
check_mode madvise
check_mode never

# --- Summary ---------------------------------------------------------

echo "=== summary ==="
if [ "$fail" -eq 0 ]; then
    echo "result: PASS (madvise and never within 50% of always)"
else
    echo "result: FAIL ($fail configuration(s) exceeded threshold):"
    echo -ne "$fail_mode"
fi
echo "always baseline average: ${avg}s"
echo "fail threshold:          ${threshold}s"

exit "$fail"
