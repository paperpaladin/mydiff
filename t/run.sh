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

# Every invocation below goes through a wrapper that bounds it in time, by
# pointing $M at the wrapper rather than by editing thirty-two call sites.
#
# Termination is a property a diff algorithm has to have, and the way it
# fails is not a wrong answer but a process that never comes back -- at which
# point this suite stops too and the number it was about to print never
# appears. That happened: an earlier build recursed on an inverted range and
# spun, the run was killed from outside, and the orphaned child held a core
# at 99% for fifteen hours before anyone noticed. A bound turns all of that
# into one FAIL line. 30s is about a hundred times the slowest case here.
M_TIMEOUT=${M_TIMEOUT:-30}
_REAL_M=$(cd "$(dirname "$M")" && pwd)/$(basename "$M")
_WRAPDIR=$(mktemp -d)      # not under $D: the suite wipes that
trap 'rm -rf "$_WRAPDIR"' EXIT
cat > "$_WRAPDIR/mydiff" <<WRAP
#!/bin/sh
exec timeout $M_TIMEOUT "$_REAL_M" "\$@"
WRAP
chmod +x "$_WRAPDIR/mydiff"
M=$_WRAPDIR/mydiff
rm -rf "$D"; mkdir -p "$D"

# --- benchmark mode: t/run.sh --bench [binary]. Separate from the suite
# --- because it measures rather than asserts.
if [ "${1:-}" = "--bench" ]; then
	shift
	M=${1:-./mydiff}
	case $M in /*) ;; *) M="$PWD/$M" ;; esac
	ROOT=$(cd "$(dirname "$0")/.." && pwd)
	BD=$ROOT/t/scratch/bench
	python3 "$ROOT/t/gen.py" "$BD" --bench > /dev/null

	# Five runs each, best-of reported, output to /dev/null so neither
	# program is charged for terminal I/O. Both are asked for the same
	# thing: a unified diff with three lines of context.
	timeit() {
		_best=999999999
		for _i in 1 2 3 4 5; do
			_t0=$(date +%s%N)
			"$@" > /dev/null 2>&1
			_t1=$(date +%s%N)
			_d=$(( (_t1 - _t0) / 1000000 ))
			[ "$_d" -lt "$_best" ] && _best=$_d
		done
		printf '%s' "$_best"
	}

	# How many times slower we are than GNU diff. Above 1.00 we lose.
	ratio() { awk "BEGIN{printf \"%.2fx\", ($2>0)?($1/$2):0}"; }

	echo "edit distance at fixed size (20000 lines each side)"
	printf '%10s %12s %12s %8s\n' D mydiff_ms diff_ms ratio
	for f in "$BD"/d-*.a; do
		d=$(basename "$f" .a); d=${d#d-}
		mt=$(timeit "$M" "$f" "${f%.a}.b")
		gt=$(timeit diff -u "$f" "${f%.a}.b")
		printf '%10s %12s %12s %8s\n' \
		    "$(echo "$d" | sed 's/^0*//')" "$mt" "$gt" "$(ratio "$mt" "$gt")"
	done
	echo
	echo "file size at fixed edit distance (100 edits)"
	printf '%10s %12s %12s %8s\n' lines mydiff_ms diff_ms ratio
	for f in "$BD"/n-*.a; do
		n=$(basename "$f" .a); n=${n#n-}
		mt=$(timeit "$M" "$f" "${f%.a}.b")
		gt=$(timeit diff -u "$f" "${f%.a}.b")
		printf '%10s %12s %12s %8s\n' \
		    "$(echo "$n" | sed 's/^0*//')" "$mt" "$gt" "$(ratio "$mt" "$gt")"
	done
	echo
	echo "like for like at large D: both sides forced minimal, so the"
	echo "comparison is of the same algorithm and not of GNU's heuristics"
	printf '%10s %12s %12s %8s\n' D mydiff_ms diff_ms ratio
	for f in "$BD"/d-010000.a "$BD"/d-020000.a; do
		d=$(basename "$f" .a); d=${d#d-}
		mt=$(timeit "$M" --minimal "$f" "${f%.a}.b")
		gt=$(timeit diff --minimal -u "$f" "${f%.a}.b")
		printf '%10s %12s %12s %8s\n' \
		    "$(echo "$d" | sed 's/^0*//')" "$mt" "$gt" "$(ratio "$mt" "$gt")"
	done
	exit 0
fi

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
	diff --unchanged-line-format='' --old-line-format='-%dn %L' \
	     --new-line-format='+%dn %L' --unchanged-group-format='' \
	     --old-group-format='@
%<' --new-group-format='@
%>' --changed-group-format='@
%<%>' "$1" "$2"
}

runs_gnu_minimal() {
	diff --minimal --unchanged-line-format='' --old-line-format='-%dn %L' \
	     --new-line-format='+%dn %L' --unchanged-group-format='' \
	     --old-group-format='@
%<' --new-group-format='@
%>' --changed-group-format='@
%<%>' "$1" "$2"
}

# Every comparison below goes through files and `cmp`, never through
# "$(cmd)" = "$(cmd)". Command substitution strips trailing newlines from both
# sides, so a missing final newline -- the exact bug this suite exists to
# catch -- would compare equal. Found that in this harness, not in the code.
runs() {
	runs_gnu_default "$1" "$2" > "$D/o.gnu"
	"$M" --runs "$1" "$2" > "$D/o.our"
	if cmp -s "$D/o.gnu" "$D/o.our"; then ok
	else bad "runs $(basename "$1") $(basename "$2")" \
	         "$(diff "$D/o.gnu" "$D/o.our")"; fi
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
	runs_gnu_default "$D/$1" "$D/$2" > "$D/k.gd"
	runs_gnu_minimal "$D/$1" "$D/$2" > "$D/k.gm"
	"$M" --runs "$D/$1" "$D/$2" > "$D/k.o"
	"$M" --minimal --runs "$D/$1" "$D/$2" > "$D/k.om"
	if cmp -s "$D/k.o" "$D/k.gm" && cmp -s "$D/k.o" "$D/k.om" \
	   && ! cmp -s "$D/k.gd" "$D/k.gm"
	then ok
	else bad "known-divergence $1 $2" \
	         "expected ours == gnu --minimal != gnu default"; fi
done

# --- unified output, the real bar: byte for byte against `diff -u`, headers
# --- and mtime timestamps included. We format the timestamp the way GNU's
# --- time_format does ("%Y-%m-%d %H:%M:%S.%N %z"), so there is nothing to
# --- normalise away and the whole stream is compared.
uni() {
	diff -u "$1" "$2" > "$D/u.gnu"; _ws=$?
	"$M" "$1" "$2" > "$D/u.our"; _gs=$?
	if cmp -s "$D/u.gnu" "$D/u.our" && [ "$_ws" = "$_gs" ]; then ok
	else bad "unified $(basename "$1") $(basename "$2")" \
	         "$(diff "$D/u.gnu" "$D/u.our")
exit status: GNU $_ws, ours $_gs"; fi
}

printf 'a\n' > "$D/s1"; printf 'b\n' > "$D/s2"
# Every combination of "ends with a newline" that matters, on purpose. nn1 is
# a one-line file with no newline, nn2 the same text with one; c1/c7 differ
# only in the final newline of the last line; nl1/nl2 both lack it and differ
# in the last line, which is the case where the marker has to appear twice.
printf 'a' > "$D/nn1"; printf 'a\n' > "$D/nn2"
printf 'a\nb\nc' > "$D/nn3"
for p in "c1 c2" "c1 c1" "c4 c1" "c1 c4" "c4 c4" "c5 c6" "c6 c5" \
         "c3 c5" "g1 g2" "g2 g1" "s1 s2" "s1 s1" "s1 c1" "big1 big2" \
         "dis1 dis2" "nn1 nn2" "nn2 nn1" "nn1 nn1" "nl1 nl2" "nl2 nl1" \
         "nn3 c1" "c1 nn3" "nn3 nn3" "c4 nn1" "nn1 c4" "nn3 c5"; do
	set -- $p
	uni "$D/$1" "$D/$2"
done

# --- context width: `diff -U N` for several N, because the hunk-merge
# --- threshold is 2*N+1 and so the grouping changes with it. -U 0 is the
# --- interesting one: no context at all, so no two runs ever merge.
uniU() {
	diff -U "$1" "$2" "$3" > "$D/u.gnu"; _ws=$?
	"$M" -U "$1" "$2" "$3" > "$D/u.our"; _gs=$?
	if cmp -s "$D/u.gnu" "$D/u.our" && [ "$_ws" = "$_gs" ]; then ok
	else bad "unified -U$1 $(basename "$2") $(basename "$3")" \
	         "$(diff "$D/u.gnu" "$D/u.our")
exit status: GNU $_ws, ours $_gs"; fi
}

for n in 0 1 2 5 10; do
	for p in "g1 g2" "c5 c6" "big1 big2" "nn3 c1" "nl1 nl2" "c4 c1" \
	         "dis1 dis2" "c1 c1"; do
		set -- $p
		uniU "$n" "$D/$1" "$D/$2"
	done
done

# The three spellings GNU accepts must all mean the same thing. "-U0" used to
# be an unknown option here while "-U 0" worked, which is the kind of gap that
# makes a harness quietly test something other than what it meant to.
diff -U2 "$D/g1" "$D/g2" > "$D/s.want"
for form in "-U2" "-U 2" "--unified=2"; do
	# shellcheck disable=SC2086
	"$M" $form "$D/g1" "$D/g2" > "$D/s.got"
	if cmp -s "$D/s.want" "$D/s.got"; then ok
	else bad "spelling [$form]" "$(diff "$D/s.want" "$D/s.got")"; fi
done

# --- patch parsing: re-printing every @@ header of a real `diff -U N` has to
# --- reproduce those lines byte for byte. That catches a parser that drops a
# --- field or silently defaults one, which is the failure mode that would
# --- then apply a hunk at the wrong line. GNU omits a count of 1, so this
# --- also pins the "@@ -1 +1 @@" spelling.
hunks() {
	diff -U "$1" "$2" "$3" > "$D/h.diff"
	grep '^@@' "$D/h.diff" > "$D/h.want"
	"$M" --hunks "$D/h.diff" > "$D/h.got"
	if cmp -s "$D/h.want" "$D/h.got"; then ok
	else bad "hunks -U$1 $(basename "$2") $(basename "$3")" \
	         "$(diff "$D/h.want" "$D/h.got")"; fi
}

for n in 0 1 3 10; do
	for p in "g1 g2" "c5 c6" "big1 big2" "s1 s2" "nn1 nn2" "c4 c1" \
	         "dis1 dis2" "c7 c8"; do
		set -- $p
		hunks "$n" "$D/$1" "$D/$2"
	done
done

# --- the closed loop, in both directions and against two independent tools.
# ---   1. GNU patch applied to OUR diff must reproduce B from A.
# ---   2. OUR -p applied to GNU's diff must reproduce B from A.
# ---   3. OUR -p applied to our own diff must too -- the weakest of the
# ---      three, since both halves are ours and a shared misreading passes.
loop() {
	_n="$(basename "$1") $(basename "$2")"

	# 1. GNU patch reads what we write.
	"$M" "$1" "$2" > "$D/l.our"
	if [ -s "$D/l.our" ]; then
		cp "$1" "$D/l.work"
		if patch -s -F0 --no-backup-if-mismatch "$D/l.work" \
		         < "$D/l.our" 2> "$D/l.err"; then
			# patch removes a file it has emptied, which is not a
			# failure when B is in fact empty.
			[ -f "$D/l.work" ] || : > "$D/l.work"
			if cmp -s "$D/l.work" "$2"; then ok
			else bad "patch(ours) $_n" "result != B"; fi
		else
			bad "patch(ours) $_n" "$(cat "$D/l.err")"
		fi
	else
		if cmp -s "$1" "$2"; then ok
		else bad "patch(ours) $_n" "we printed nothing but A != B"; fi
	fi

	# 2. We read what GNU writes.
	diff -u "$1" "$2" > "$D/l.gnu"
	if "$M" -p "$D/l.gnu" "$1" > "$D/l.out" 2> "$D/l.err"; then
		if cmp -s "$D/l.out" "$2"; then ok
		else bad "ours-p(gnu) $_n" "$(diff "$D/l.out" "$2" | head -6)"; fi
	else
		bad "ours-p(gnu) $_n" "$(cat "$D/l.err")"
	fi

	# 3. Self round-trip.
	if "$M" -p "$D/l.our" "$1" > "$D/l.out" 2> "$D/l.err"; then
		if cmp -s "$D/l.out" "$2"; then ok
		else bad "ours-p(ours) $_n" "$(diff "$D/l.out" "$2" | head -6)"; fi
	else
		bad "ours-p(ours) $_n" "$(cat "$D/l.err")"
	fi
}

for p in "c1 c2" "c1 c1" "c4 c1" "c1 c4" "c4 c4" "c5 c6" "c6 c5" "c3 c5" \
         "g1 g2" "g2 g1" "s1 s2" "s1 c1" "big1 big2" "dis1 dis2" \
         "c7 c8" "nn1 nn2" "nn2 nn1" "nl1 nl2" "nl2 nl1" "nn3 c1" \
         "c1 nn3" "c4 nn1" "nn1 c4" "nn3 c5"; do
	set -- $p
	loop "$D/$1" "$D/$2"
done

# --- the corpus. 93 generated pairs, every one of them put to GNU diff three
# --- ways: is our unified output the same bytes, does GNU patch accept it,
# --- and do we accept GNU's. The agree/differ tally is printed separately
# --- from the pass count, because "differs from GNU" is a fact worth a
# --- number of its own.
echo
echo "corpus: $(python3 "$ROOT/t/gen.py" "$D/corpus") pairs"
agree=0; differ=0
for a in "$D"/corpus/*.a; do
	b=${a%.a}.b
	nm=$(basename "$a" .a)
	diff -u "$a" "$b" > "$D/c.gnu"; gs=$?
	"$M" "$a" "$b" > "$D/c.our"; os=$?
	if cmp -s "$D/c.gnu" "$D/c.our" && [ "$gs" = "$os" ]; then
		agree=$((agree+1)); ok
	else
		differ=$((differ+1))
		bad "corpus-bytes $nm" "$(diff "$D/c.gnu" "$D/c.our" | head -12)
exit status: GNU $gs, ours $os"
	fi
	loop "$a" "$b"
done
echo
echo "$agree agree with GNU diff byte-for-byte, $differ differ"

# --- the adversarial family, kept separate because the two claims about it
# --- are different strengths.
# ---
# --- On inputs drawn from a 1-to-3 symbol line alphabet there are many
# --- distinct minimal edit scripts and only the boundary normaliser decides
# --- between them. It is also the only family where GNU diff's *default*
# --- output differs from its own --minimal output, because of
# --- discard_confusing_lines, which we do not implement. So:
# ---   hard assertion: we equal `diff --minimal -U N` on every pair. A
# ---     failure here is a bug in us.
# ---   measurement: how many also equal plain `diff -U N`. The shortfall is
# ---     GNU choosing a different tie-break, and the suite proves that by
# ---     checking GNU's own two answers also differ on exactly those pairs.
echo
echo "fuzz: $(python3 "$ROOT/t/gen.py" "$D/fuzz" --fuzz 400) adversarial pairs"
fagree=0; fdiffer=0; fbogus=0
for a in "$D"/fuzz/*.a; do
	b=${a%.a}.b
	nm=$(basename "$a" .a)
	u=${nm##*-U}
	diff --minimal -U "$u" "$a" "$b" > "$D/f.min"
	diff -U "$u" "$a" "$b" > "$D/f.def"
	"$M" -U "$u" "$a" "$b" > "$D/f.our"
	if cmp -s "$D/f.min" "$D/f.our"; then
		ok
	else
		bad "fuzz-minimal $nm" "$(diff "$D/f.min" "$D/f.our" | head -12)"
	fi
	if cmp -s "$D/f.def" "$D/f.our"; then
		fagree=$((fagree+1))
	else
		fdiffer=$((fdiffer+1))
		# The shortfall must be GNU disagreeing with itself. If GNU's
		# default and --minimal agree and we still differ, that is a
		# bug in us wearing a disguise.
		if cmp -s "$D/f.def" "$D/f.min"; then
			fbogus=$((fbogus+1))
			bad "fuzz-unexplained $nm" \
			    "GNU default == GNU --minimal, yet we differ"
		fi
	fi
done
echo "$fagree agree with plain \`diff -U N\`, $fdiffer differ"
echo "of the $fdiffer, $((fdiffer-fbogus)) are GNU disagreeing with its own"
echo "--minimal (discard_confusing_lines) and $fbogus are unexplained"

# --- fail-closed. Everything above checks that we agree with GNU when the
# --- input is good; nothing checked that we refuse bad input instead of
# --- producing a plausible wrong answer. Exit 2 is the only acceptable
# --- answer to all of these, and for the patch cases GNU patch is asked the
# --- same question so that "strict" can be distinguished from "broken".
status() {
	_n=$1; _w=$2; shift 2
	"$@" > "$D/n.out" 2> "$D/n.err"; _s=$?
	if [ "$_s" = "$_w" ]; then ok
	else bad "status $_n" "want exit $_w, got $_s
$(cat "$D/n.err")"; fi
}

status missing-file      2 "$M" "$D/does-not-exist" "$D/c1"
status missing-file-2    2 "$M" "$D/c1" "$D/does-not-exist"
status directory         2 "$M" "$D" "$D/c1"
status unknown-option    2 "$M" --no-such-option "$D/c1" "$D/c2"
status too-few-args      2 "$M" "$D/c1"
status too-many-args     2 "$M" "$D/c1" "$D/c2" "$D/c3"
status patch-missing     2 "$M" -p "$D/does-not-exist" "$D/c1"
status patch-no-orig     2 "$M" -p "$D/c1"

# A hunk whose context does not match the file. GNU patch without -F0 would
# search for somewhere it fits; we never do, and that is the point.
printf -- '@@ -1,3 +1,3 @@
 a
-WRONG
+x
 c
' > "$D/n.bad"
status patch-mismatch    2 "$M" -p "$D/n.bad" "$D/c1"
# A hunk naming a line past the end of the file.
printf -- '@@ -900,1 +900,1 @@
-a
+b
' > "$D/n.past"
status patch-past-end    2 "$M" -p "$D/n.past" "$D/c1"
# Hunks out of order, which we require to be increasing.
printf -- '@@ -3 +3 @@
-c
+z
@@ -1 +1 @@
-a
+y
' > "$D/n.ooo"
status patch-out-of-order 2 "$M" -p "$D/n.ooo" "$D/c1"
# A hunk that stops in the middle of its own declared line count.
printf -- '@@ -1,3 +1,3 @@
 a
-b
' > "$D/n.trunc"
status patch-truncated   2 "$M" -p "$D/n.trunc" "$D/c1"
# A header whose counts do not match the body it is followed by.
printf -- '@@ -1,2 +1,9 @@
 a
-b
+x
' > "$D/n.count"
status patch-miscounted  2 "$M" -p "$D/n.count" "$D/c1"

# A patch with no hunks at all is not an error, it is a no-op: the result is
# the original. GNU patch rejects it ("only garbage was found"); we are
# deliberately laxer, because `mydiff A A` legitimately prints nothing and
# feeding that back must be the identity.
printf 'not a patch at all
' > "$D/n.junk"
status patch-junk-is-noop 0 "$M" -p "$D/n.junk" "$D/c1"
if cmp -s "$D/n.out" "$D/c1"; then ok
else bad "patch-junk-identity" "a no-op patch did not reproduce the input"; fi

# And the one GNU patch agrees with us about: our strictness must not be
# strictness about something GNU thinks is fine. If GNU patch -F0 accepts the
# mismatching hunk, our refusal is a bug, not a policy.
cp "$D/c1" "$D/n.work"
if patch -s -F0 --no-backup-if-mismatch "$D/n.work" < "$D/n.bad" \
     > /dev/null 2>&1; then
	bad "patch-mismatch-agreement" \
	    "GNU patch -F0 accepted the hunk we rejected"
else
	ok
fi

printf '\n%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" = 0 ]
