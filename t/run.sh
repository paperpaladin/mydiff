#!/bin/sh
# The oracle. GNU diffutils is the authority here, and the bar is not "looks
# like a diff" but "byte for byte what `diff -u` printed, headers included".
# Nothing in this file contains a hand-written expected diff.
#
# Usage: t/run.sh [path-to-mydiff]
set -u

M=${1:-./mydiff}
case $M in /*) ;; *) M="$PWD/$M" ;; esac
ROOT=$(cd "$(dirname "$0")/.." && pwd)

# Scratch lives under the repo, keyed on the binary, never in /tmp.
D=$ROOT/t/scratch/$(basename "$M")
rm -rf "$D"; mkdir -p "$D"

pass=0; fail=0
ok()  { pass=$((pass+1)); }
bad() { fail=$((fail+1)); printf 'FAIL: %s\n' "$1"
        [ $# -gt 1 ] && printf '%s\n' "$2" | sed 's/^/      | /'; }

# --- equivalence classes: `sort -u` is the authority on how many distinct
# --- lines two files have between them.
classes() {
	_want=$(cat "$1" "$2" | sort -u | wc -l)
	_got=$("$M" --stat "$1" "$2" | sed 's/.*classes //')
	if [ "$_want" = "$_got" ]; then ok
	else bad "classes $1 $2" "want $_want got $_got"; fi
}

printf 'a\nb\nc\n' > "$D/c1"; printf 'a\nb\nd\n' > "$D/c2"
printf 'x\nx\nx\nx\n' > "$D/c3"; : > "$D/c4"
seq 1 500 > "$D/c5"; seq 250 750 > "$D/c6"
classes "$D/c1" "$D/c2"
classes "$D/c3" "$D/c3"
classes "$D/c4" "$D/c1"
classes "$D/c4" "$D/c4"
classes "$D/c5" "$D/c6"
# Larger and more collision-prone: 2000 lines drawn from a 7-value alphabet,
# so a hash that ignores part of the line, or a comparator that stops short,
# shows up as a wrong class count. Watched this fail against a hash seeded
# without the line bytes before trusting it.
awk 'BEGIN{srand(7);for(i=0;i<2000;i++)print "line" int(rand()*7) "x" i%3}' \
	> "$D/c7"
awk 'BEGIN{srand(9);for(i=0;i<2000;i++)print "line" int(rand()*7) "x" i%4}' \
	> "$D/c8"
classes "$D/c7" "$D/c8"

# NOTE on what this check cannot do: `sort -u` is blind to a missing trailing
# newline, because `cat a b` welds a's unterminated tail onto b's first line
# and the distinction is gone before sort ever sees it. So nothing here proves
# that "c" and "c\n" are different classes. That claim gets its own oracle at
# the unified-output stage, where GNU diff prints "\ No newline at end of
# file" and we have to print it in the same place.

printf '\n%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" = 0 ]
