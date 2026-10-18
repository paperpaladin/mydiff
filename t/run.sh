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

# Every claim in this file is a claim about what GNU diffutils, GNU patch or
# python3 printed. Without them there is nothing to compare against, and a
# run that quietly checked nothing would still print a pass count. Refuse,
# loudly, naming what is missing.
missing=
for tool in diff patch python3; do
	command -v "$tool" > /dev/null 2>&1 || missing="$missing $tool"
done
if [ -n "$missing" ]; then
	printf 'SKIP: the oracle is not installed:%s\n' "$missing" >&2
	printf 'SKIP: nothing was checked. This is not a pass.\n' >&2
	exit 2
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
# 6000 distinct lines against a permutation of themselves. Every line
# matches exactly one line on the other side, so discard_confusing_lines
# throws nothing away and the edit distance stays near 2N. That is the only
# input here that drives a single diag() call past too_expensive (4096) and
# so reaches the bailout that returns a deliberately non-minimal split.
# Nothing in this suite reached that path before: the bench pairs do, and
# they were timed and never compared, which is how `mydiff` came to disagree
# with `diff -u` by one line on the 20000-line bench pair without anything
# noticing. 6000 is the size: at 3000 the search finishes under the
# threshold and the path is not reached.
python3 -c 'import random
r = random.Random(11)
a = ["uniq%d" % i for i in range(6000)]
b = a[:]
r.shuffle(b)
open("'"$D"'/perm1", "w").write("".join(x + chr(10) for x in a))
open("'"$D"'/perm2", "w").write("".join(x + chr(10) for x in b))'
changed "$D/perm1" "$D/perm2"

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
         "nl1 c1" "big1 big2" "dis1 dis2" "perm1 perm2"; do
	set -- $p
	runs "$D/$1" "$D/$2"
done

# --- c7/c8 is where GNU's two answers differ, and it is now asserted from
# --- both ends rather than measured.
# ---
# --- 2000 lines over a 7-value alphabet, edit distance 2694. GNU's default
# --- output and its own --minimal output are different scripts of the same
# --- cost, because by default GNU runs discard_confusing_lines first and
# --- that changes which of the equally short scripts comes out. We now
# --- implement it, so we have to match BOTH: the default with the discarding
# --- on, --minimal with it off.
# ---
# --- The third assertion, that GNU still disagrees with itself here, is the
# --- one that keeps the other two honest. It is what makes this pair able to
# --- tell the two code paths apart at all: with the discard pass unwired our
# --- default output would silently become our --minimal output, still equal
# --- to `diff --minimal` and no longer equal to `diff`. Remove the call and
# --- this case goes red, which is the point of it.
for p in "c7 c8" "c8 c7"; do
	set -- $p
	runs_gnu_default "$D/$1" "$D/$2" > "$D/k.gd"
	runs_gnu_minimal "$D/$1" "$D/$2" > "$D/k.gm"
	"$M" --runs "$D/$1" "$D/$2" > "$D/k.o"
	"$M" --minimal --runs "$D/$1" "$D/$2" > "$D/k.om"
	if cmp -s "$D/k.o" "$D/k.gd" && cmp -s "$D/k.om" "$D/k.gm" \
	   && ! cmp -s "$D/k.gd" "$D/k.gm"
	then ok
	else bad "two-answers $1 $2" \
	         "ours default vs gnu default: $(cmp "$D/k.o" "$D/k.gd" 2>&1)
ours minimal vs gnu minimal: $(cmp "$D/k.om" "$D/k.gm" 2>&1)
gnu default vs gnu minimal: $(cmp "$D/k.gd" "$D/k.gm" 2>&1) (must differ)"; fi
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
         "nn3 c1" "c1 nn3" "nn3 nn3" "c4 nn1" "nn1 c4" "nn3 c5" \
         "perm1 perm2" "perm2 perm1"; do
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
         "c1 nn3" "c4 nn1" "nn1 c4" "nn3 c5" "perm1 perm2"; do
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
# The verdict for this family is `bad` per differing pair, so zero pairs is
# zero failures: a gen.py that emitted nothing would print "corpus: 0 pairs",
# agree 0, differ 0, and pass. The corpus is generated here rather than
# vendored, so it cannot go missing -- but it can shrink, and nothing was
# watching the number. 93 is what t/gen.py produces; it moves when gen.py
# does, in the same commit.
if [ "$((agree + differ))" -eq 93 ]; then
	ok
else
	bad "corpus size" "t/gen.py produced $((agree + differ)) pairs, expected 93"
fi

# --- the adversarial family, now asserted against both of GNU's answers.
# ---
# --- On inputs drawn from a 1-to-3 symbol line alphabet there are many
# --- distinct minimal edit scripts and only the boundary normaliser decides
# --- between them. It is also the family where GNU diff's *default* output
# --- differs from its own --minimal output, because by default GNU discards
# --- confusing lines before comparing. Both are hard assertions now:
# ---   `mydiff -U N`           == `diff -U N`            (discarding on)
# ---   `mydiff --minimal -U N` == `diff --minimal -U N`  (discarding off)
# --- Until the discard pass existed only the second one could be made, and
# --- the first was a measurement that stood at 388 of 400. It is kept as a
# --- measurement as well as an assertion, because the printed number is what
# --- says how much of the gap is left, and it must now print 400 and 0.
# ---
# --- A diff that matches GNU can still be a diff that does not apply, so
# --- each pair also goes through GNU patch: our output, fed to an
# --- independent program, has to turn A back into B. That is the property
# --- the byte comparison does not check, and this family is where our new
# --- code path chooses the hunks.
echo
echo "fuzz: $(python3 "$ROOT/t/gen.py" "$D/fuzz" --fuzz 400) adversarial pairs"
# 400 of those are the small-alphabet pairs; gen.py adds 100 more built
# to make long runs of discardable lines, which is what reaches the
# inner rules of discard_confusing_lines. The count below is the total.
fagree=0; fdiffer=0
for a in "$D"/fuzz/*.a; do
	b=${a%.a}.b
	nm=$(basename "$a" .a)
	u=${nm##*-U}
	diff --minimal -U "$u" "$a" "$b" > "$D/f.min"
	diff -U "$u" "$a" "$b" > "$D/f.def"
	"$M" -U "$u" "$a" "$b" > "$D/f.our"
	"$M" --minimal -U "$u" "$a" "$b" > "$D/f.ourmin"
	if cmp -s "$D/f.min" "$D/f.ourmin"; then
		ok
	else
		bad "fuzz-minimal $nm" "$(diff "$D/f.min" "$D/f.ourmin" | head -12)"
	fi
	if cmp -s "$D/f.def" "$D/f.our"; then
		fagree=$((fagree+1)); ok
	else
		fdiffer=$((fdiffer+1))
		bad "fuzz-default $nm" "$(diff "$D/f.def" "$D/f.our" | head -12)"
	fi
	# Reconstruction: GNU patch applied to what we just wrote must
	# produce B. Empty output is only correct when A and B are equal.
	if [ -s "$D/f.our" ]; then
		cp "$a" "$D/f.work"
		if patch -s -F0 --no-backup-if-mismatch "$D/f.work" \
		         < "$D/f.our" 2> "$D/f.err"; then
			[ -f "$D/f.work" ] || : > "$D/f.work"
			if cmp -s "$D/f.work" "$b"; then ok
			else bad "fuzz-apply $nm" "patched A != B"; fi
		else
			bad "fuzz-apply $nm" "$(cat "$D/f.err")"
		fi
	else
		if cmp -s "$a" "$b"; then ok
		else bad "fuzz-apply $nm" "we printed nothing but A != B"; fi
	fi
done
echo "$fagree agree with plain \`diff -U N\`, $fdiffer differ"
if [ "$((fagree + fdiffer))" -eq 500 ]; then
	ok
else
	bad "fuzz size" "t/gen.py produced $((fagree + fdiffer)) pairs, expected 500"
fi

# --- the sanitizer's own blind spot, checked instead of assumed.
# ---
# --- A buffer larger than its data turns ASan off over that buffer: a read
# --- one past the end of the undiscarded sequence lands inside the next
# --- live allocation and nothing is reported. GNU packs all four of those
# --- arrays into one block; we allocate four and realloc each down to the
# --- length actually used, for this reason alone, and the only way to know
# --- the realloc is load-bearing is to plant the over-read and watch.
# ---
# --- Measured when this was added: the planted read trips ASan on 96 of the
# --- 500 fuzz pairs with the trim in place, and on 0 of 500 with the four
# --- arrays packed into one block. The second number is what a clean ASan
# --- run looks like when it is checking nothing.
# ---
# --- Only under `make test-asan`, which is where a compiler and the
# --- sanitizer flags are already in hand.
if [ -n "${MYDIFF_ASAN:-}" ]; then
	sed 's/x < xlim \&\& y < ylim/x <= xlim \&\& y <= ylim/' \
		"$ROOT/mydiff.c" > "$D/overread.c"
	if ! cmp -s "$ROOT/mydiff.c" "$D/overread.c" \
	   && ${CC:-gcc} $ASAN_CFLAGS -o "$D/overread" "$D/overread.c" \
	        2> "$D/overread.cc.err"; then
		seen=0
		for a in "$D"/fuzz/*.a; do
			nm=$(basename "$a" .a)
			ASAN_OPTIONS=exitcode=86:detect_leaks=0 \
			    "$D/overread" -U "${nm##*-U}" "$a" "${a%.a}.b" \
			    > /dev/null 2> "$D/overread.err"
			if [ $? = 86 ]; then seen=1; break; fi
		done
		if [ "$seen" = 1 ]; then ok
		else bad "asan-sees-overread" \
		         "a read past the end of the undiscarded sequence was
not reported on any of 500 pairs. Either the arrays are
packed into one allocation again, or the sed below no
longer matches the snake loop it is meant to break."; fi
	else
		bad "asan-sees-overread" \
		    "could not build the planted over-read:
$(cat "$D/overread.cc.err" 2>/dev/null)"
	fi
fi

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
