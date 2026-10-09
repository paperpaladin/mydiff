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
# Cap the evidence: a 2000-line disagreement scrolls the actual failure list
# off the screen, which is how a suite stops being read.
bad() { fail=$((fail+1)); printf 'FAIL: %s\n' "$1"
        [ $# -gt 1 ] && printf '%s\n' "$2" | head -20 | sed 's/^/      | /'
        return 0; }

# --- equivalence classes: `sort -u` is the authority on how many distinct
# --- lines two files have between them.
classes() {
	_want=$(cat "$1" "$2" | sort -u | wc -l)
	_got=$("$M" --stat "$1" "$2" | awk '{print $5}')
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

# --- edit distance: GNU diff's own line-oriented output is the authority on
# --- how many lines of each file a shortest edit script has to touch. The
# --- counts are invariant under the boundary normalisation that comes later,
# --- so this isolates minimality from formatting.
changed() {
	_o=$(diff --old-line-format='-' --new-line-format='+' \
	          --unchanged-line-format='' "$1" "$2")
	_w0=$(printf '%s' "$_o" | tr -cd -  | wc -c | tr -d ' ')
	_w1=$(printf '%s' "$_o" | tr -cd +  | wc -c | tr -d ' ')
	_g=$("$M" --stat "$1" "$2" | awk '{print $7" "$8}')
	if [ "$_w0 $_w1" = "$_g" ]; then ok
	else bad "changed $(basename "$1") $(basename "$2")" \
	         "GNU says $_w0 $_w1, we say $_g"; fi
}

changed "$D/c1" "$D/c2"
changed "$D/c1" "$D/c1"
changed "$D/c4" "$D/c1"
changed "$D/c1" "$D/c4"
changed "$D/c4" "$D/c4"
changed "$D/c5" "$D/c6"
changed "$D/c6" "$D/c5"
changed "$D/c7" "$D/c8"
changed "$D/c3" "$D/c5"
# 10000 lines, 50 scattered edits: D stays small, which is the whole point of
# Myers. Also the case where GNU's own heuristics could diverge from a
# minimal script and do not.
awk 'BEGIN{for(i=1;i<=10000;i++)print "l"i}' > "$D/big1"
awk 'BEGIN{for(i=1;i<=10000;i++){if(i%200==7)print "EDIT"i;else print "l"i}}' \
	> "$D/big2"
changed "$D/big1" "$D/big2"
# No line in common at all: D is N+M, the worst case for Myers.
awk 'BEGIN{for(i=1;i<=400;i++)print "aaa"i}' > "$D/dis1"
awk 'BEGIN{for(i=1;i<=400;i++)print "bbb"i}' > "$D/dis2"
changed "$D/dis1" "$D/dis2"

# --- the edit script itself: where each run of changes starts and ends, and
# --- that deletions come before insertions inside a run. GNU's line-group
# --- formats will print exactly that, with no context, no hunk merging and no
# --- line numbers, so this pins the script down before any formatting exists.
# --- %L preserves a missing trailing newline, so that case is covered here too.
runs_gnu_default() {
	diff --unchanged-line-format='' --old-line-format='-%L' \
	     --new-line-format='+%L' --unchanged-group-format='' \
	     --old-group-format='@
%<' --new-group-format='@
%>' --changed-group-format='@
%<%>' "$1" "$2"
}

runs_gnu_minimal() {
	diff --minimal --unchanged-line-format='' --old-line-format='-%L' \
	     --new-line-format='+%L' --unchanged-group-format='' \
	     --old-group-format='@
%<' --new-group-format='@
%>' --changed-group-format='@
%<%>' "$1" "$2"
}

runs() {
	_w=$(diff --unchanged-line-format='' --old-line-format='-%L' \
	          --new-line-format='+%L' --unchanged-group-format='' \
	          --old-group-format='@
%<' --new-group-format='@
%>' --changed-group-format='@
%<%>' "$1" "$2")
	_g=$("$M" --runs "$1" "$2")
	if [ "$_w" = "$_g" ]; then ok
	else bad "runs $(basename "$1") $(basename "$2")" \
	         "GNU:
$_w
ours:
$_g"; fi
}

seq 1 20 > "$D/g1"
sed -e '3c\
X' -e '10,12d' -e '17a\
Y' "$D/g1" > "$D/g2"
printf 'a\nb' > "$D/nl1"; printf 'a\nc' > "$D/nl2"
for p in "c1 c2" "c1 c1" "c4 c1" "c1 c4" "c4 c4" "c5 c6" "c6 c5" \
         "c3 c5" "g1 g2" "g2 g1" "nl1 nl2" "nl1 nl1" \
         "nl1 c1" "big1 big2" "dis1 dis2"; do
	set -- $p
	runs "$D/$1" "$D/$2"
done

# --- c7/c8 is the known divergence, asserted rather than skipped.
# ---
# --- 2000 lines over a 7-value alphabet, edit distance 2694. We agree with
# --- `diff --minimal` byte for byte and disagree with plain `diff`, and plain
# --- `diff` disagrees with its own --minimal. The cause is GNU's
# --- discard_confusing_lines, which we do not implement; both answers cost
# --- the same 1347+1347 lines, so this is a tie broken differently, not a
# --- worse script. The assertion is all three facts at once, so that if any
# --- of them changes -- including us accidentally starting to agree with the
# --- default -- the suite says so instead of quietly passing.
for p in "c7 c8" "c8 c7"; do
	set -- $p
	_gd=$(runs_gnu_default "$D/$1" "$D/$2")
	_gm=$(runs_gnu_minimal "$D/$1" "$D/$2")
	_o=$("$M" --runs "$D/$1" "$D/$2")
	_om=$("$M" --minimal --runs "$D/$1" "$D/$2")
	if [ "$_o" = "$_gm" ] && [ "$_o" = "$_om" ] && [ "$_gd" != "$_gm" ]
	then ok
	else bad "known-divergence $1 $2" \
	         "expected ours == gnu --minimal != gnu default"; fi
done

printf '\n%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" = 0 ]
