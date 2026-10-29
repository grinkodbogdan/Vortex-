#!/bin/sh
# Print what userspace costs in RAM, the number the Vortex budget is about.
#
# Uses PSS (proportional set size): shared pages are split between the
# processes that map them, so the column sums to real usage.
#   tools/memreport.sh          per-process table + total
#   tools/memreport.sh -t       total only (KiB)
set -eu

total=0
rows=$(for d in /proc/[0-9]*; do
	f=$d/smaps_rollup
	[ -r "$f" ] || continue
	pss=$(awk '/^Pss:/ {print $2; exit}' "$f" 2>/dev/null) || continue
	[ -n "$pss" ] && [ "$pss" -gt 0 ] || continue
	name=$(tr '\0' ' ' < "$d/cmdline" 2>/dev/null | cut -c1-48)
	[ -n "$name" ] || name="[$(cat "$d/comm")]"
	printf '%s\t%s\t%s\n' "$pss" "${d#/proc/}" "$name"
done | sort -rn)

total=$(printf '%s\n' "$rows" | awk -F '\t' '{s += $1} END {print s + 0}')
if [ "${1:-}" = -t ]; then
	echo "$total"
	exit 0
fi

printf '%8s %7s  %s\n' PSS_KiB PID COMMAND
printf '%s\n' "$rows" | awk -F '\t' 'NF >= 3 {printf "%8d %7d  %s\n", $1, $2, $3}'
printf '%8d          total userspace (%.1f MiB)\n' "$total" "$(echo "$total" | awk '{print $1/1024}')"
awk '/^(MemTotal|MemAvailable|Shmem|SwapTotal|SwapFree):/ {printf "%-14s %8d KiB\n", $1, $2}' /proc/meminfo
if [ -r /sys/block/zram0/mm_stat ]; then
	awk '{printf "zram: %d KiB stored in %d KiB\n", $1/1024, $3/1024}' /sys/block/zram0/mm_stat
fi
