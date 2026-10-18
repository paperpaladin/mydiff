# mydiff

Myers' diff algorithm in one C11 file. Computes an edit script between two
text files, prints it as a unified diff, and applies one back again. The
only test oracle is GNU diffutils and GNU patch. The script is minimal under
`--minimal`; by default it is whatever GNU's two heuristics make it, which
is the point: the bar is agreement with `diff -u`, byte for byte.

## make test

```
$ make clean && make test
rm -f mydiff mydiff-asan
rm -rf t/scratch
gcc -std=c11 -O2 -g -Wall -Wextra -Wpedantic -ffile-prefix-map=. -o mydiff mydiff.c
t/run.sh ./mydiff

corpus: 93 pairs

93 agree with GNU diff byte-for-byte, 0 differ

fuzz: 500 adversarial pairs
500 agree with plain `diff -U N`, 0 differ

2104 passed, 0 failed
```

`make test-asan` runs the same 2104 checks under
`-fsanitize=address,undefined`, and also passes. It runs one more, 2105,
which is about the sanitizer rather than about diff: see below.

The suite refuses to run at all, with exit 2 and two lines saying so, if
`diff`, `patch` or `python3` is missing. Every claim in it is a claim about
what one of those three printed, so a run without them is not a reduced run,
it is a pass count for nothing.

## Usage

```sh
mydiff A B                  # unified diff, 3 lines of context
mydiff -U 0 A B             # no context
mydiff --minimal A B        # disable the non-minimal heuristics
mydiff -p PATCH ORIG > NEW  # apply a unified diff
```

Exit status is GNU diff's: 0 same, 1 differ, 2 trouble.

```sh
$ mydiff a b | patch -F0 a      # works, which is half the point
$ diff -u a b > p; mydiff -p p a | cmp - b   # works, which is the other half
```

Four extra modes exist only because the test suite uses them: `--stat`
(line, class and changed-line counts), `--runs` (the raw edit script with no
formatting), `--hunks` (re-print the `@@` headers a patch file contains).
They are oracle hooks, not features, each one exposes an
intermediate result so that an independent GNU output format can check it in
isolation.

## Why the oracle is unusually good here

Most reimplementations can only be checked loosely. You can test that a
decompressor round-trips, or that a parser accepts valid input, but
"byte-identical to the reference" is usually off the table because formatting
is a matter of taste. Diff is different. Myers' algorithm produces a
*minimal* edit script, GNU diffutils implements the same algorithm, and the
unified format is fully specified down to where the newline goes. So exact
agreement is achievable, and the bar can be set at it:

That argument covers `--minimal` on both sides. GNU's default is not
minimal, because it discards confusing lines first and gives up once a
search gets expensive, and neither of those is derivable from the problem
statement: they are choices with constants in them. Agreement there is a
harder bar rather than a softer one, since nothing but a faithful copy of
the heuristic will reproduce it, and the suite holds both at once.

`mydiff A B` and `diff -u A B` must produce the same bytes, including the
two header lines with their nanosecond mtimes, which this program formats the
way GNU's `time_format` does precisely so that there is nothing to normalise
away before comparing. Any disagreement is then a real difference in
behaviour that can be chased down, not a formatting quibble to be argued
about.

And the loop closes in both directions against two independent tools:

1. `diff -u A B` equals `mydiff A B`, byte for byte.
2. GNU patch applied to our diff reproduces B from A.
3. Our `-p` applied to GNU's diff reproduces B from A.

(3) is not redundant with (1): a shared misunderstanding of the format would
pass (1) and fail (2) or (3). Only the self round-trip, `mydiff -p` on
`mydiff`'s own output, is weak, and the suite says so where it runs it.

## What does not work

This is the honest list. Several of these are deliberate and marked
`shortcut:` in the source with their ceiling and upgrade path. The rest are
simply absent.

The output is not the minimal diff, and the claim here is not that it is.
`discard_confusing_lines` is a heuristic: GNU throws away lines it judges
unhelpful before comparing, which changes *which* of several edit scripts
comes out and can make the answer longer than a minimal one. We implement
the same heuristic, constants included, copied from diffutils' `analyze.c`
rather than derived, because the only reason to have it is to agree with
`diff -u`. So the claim is "agrees with GNU diff", never "produces the
minimal diff". `--minimal`, on both programs, is what asks for the minimal
one, and the suite checks that pairing separately.

This is the part of the project with the shortest history of being wrong.
Until this was implemented, 12 of 400 adversarial pairs disagreed with plain
`diff -u` and agreed with `diff -u --minimal`; both numbers are 0 and 500
now. The heuristic's inner rules are not covered by inputs that merely look
adversarial: a 160-line file over a 3-symbol alphabet never builds a run of
discardable lines long enough for "a quarter of the run is provisional" or
"three definite discards in a row" to mean anything, and mutating those
constants left the suite green. 100 pairs built specifically to make long
discard runs are what kill those mutations.

Two mutations still survive and are not hidden here.

- `too_expensive` is computed from the number of undiscarded lines plus
  three, the way GNU computes it; computing it from the raw line count
  instead changes nothing, because the result is clamped up to 4096 and the
  unclamped value only exceeds 4096 somewhere past four million lines. The
  two are the same function at every size the suite can afford.
- `c.heuristic` can be set back to `!minimal` and nothing goes red. It
  guards GNU's big-snake shortcut, which GNU enables only with `-H`. We have
  no `-H`, so ours is now hard-wired off, and an instrumented build fired
  the shortcut on none of the 500 fuzz pairs, 93 corpus pairs or 11 bench
  pairs even with it forced on. That block is dead code, and it is dead in
  both directions: no test can tell the flag's two settings apart.

`-p` is not a replacement for patch(1), and is not trying to be.

- No fuzz and no offset search. Context and deleted lines must match the
  original exactly, at exactly the line the `@@` header names. GNU patch will
  cheerfully apply a hunk 40 lines away from where the header says. That is
  the right behaviour for a human applying a stale patch and the wrong
  behaviour for a closed test loop, because a patch whose line numbers were
  all one off would still apply and the loop would stop detecting it.
- Hunks must appear in increasing order of original line number.
- Single-file patches only. A patch containing several `--- / +++` sections
  will be applied as if it were all one file, and will almost certainly
  produce nonsense, not an error.
- No reversed-patch detection, no file creation or deletion, no `Index:` or
  git extended headers, no `.orig`/`.rej` files, no backups.
- Output is streamed. A patch that fails on its third hunk has already
  written the first two hunks' worth of lines to stdout. The exit status is 2
  and the output must be thrown away; nothing cleans up for you. Buffering it
  is the second unchecked item in `PLAN.md`.
- Only the unified format is parsed. Context, normal and ed-style diffs are
  not.

Only unified output. No `-c` context diffs, no normal or ed or RCS output,
no `--side-by-side`, no if-then-else formats.

Almost none of diff's other options. No `-b`, `-w`, `-i`, `-B`
(whitespace and case insensitivity), no `-r` for directories, no `--label`, no
colour, no binary-file detection, no `-N`/`--new-file`. Two files, both
regular, both text, is the whole input domain.

`-p` means something different here than in GNU diff. In `diff`, `-p`
shows which C function each hunk falls in. Here it selects patch mode. If you
have muscle memory for `diff -p`, this will surprise you.

The `too_expensive` bailout is still here and still non-minimal. Once a
single `diag()` call has spent 4096 edits it stops looking for the best
split and takes the furthest point either frontier reached. GNU does exactly
this, unconditionally, which is why the two still agree; both answers can be
longer than minimal. Only one pair in the suite reaches it (6000 distinct
lines against a permutation of themselves), and before that pair was added
nothing in the suite had ever executed that path.

Both files are read entirely into memory, plus an equivalence-class table
and two frontier arrays sized N+M. 400000 lines is comfortable; a file larger
than RAM will not work and there is no streaming mode.

A line is bytes. No CRLF normalisation: a file with `\r\n` endings
differs from the same file with `\n` endings on every line, the thing
`diff` also reports. No character-encoding awareness of any kind. Trailing
whitespace is a real difference, since there is no `-b`.

Timestamps are local time, read from `st_mtim` and formatted with
`localtime_r`, so the two header lines depend on `TZ`. GNU diff does the same,
which is why they still compare equal, but it means neither tool's output is
reproducible across time zones.

No stdin. Both arguments must be paths. `-` is not special, and neither is
`/dev/null`.

Line counts are `long`. A file with more than `LONG_MAX` lines is
unsupported, which on any machine that could hold such a file is not the
binding constraint.

## Performance

Myers is O((N+M)D) where D is the edit distance, so the interesting claim is
that cost tracks D, not file size. Measured (`make bench`, best of five,
20000 lines on each side for the first table):

```
edit distance at fixed size (20000 lines each side)
         D    mydiff_ms      diff_ms    ratio
         2           10            3    3.33x
        20           10            6    1.67x
       200           10            6    1.67x
      2000           11            7    1.57x
     10000           10            6    1.67x
     20000           11            7    1.57x

file size at fixed edit distance (100 edits)
     lines    mydiff_ms      diff_ms    ratio
      1000            1            1    1.00x
      5000            3            2    1.50x
     20000           10            5    2.00x
    100000           61           22    2.77x
    400000          263          110    2.39x

like for like at large D: both sides forced minimal, so the
comparison is of the same algorithm and not of GNU's heuristics
         D    mydiff_ms      diff_ms    ratio
     10000          173          143    1.21x
     20000          705          555    1.27x
```

The claim holds: the first table is flat. Between D=2 and D=20000 the time
barely moves, because the whole 10ms is the fixed cost of reading and
hashing 1.8MB, and the `(N+M)D` term never gets to be visible: on these
pairs the changed lines match nothing on the other side, so
`discard_confusing_lines` throws them all away and the comparison that
remains is tiny. Force both programs to be minimal and the same pair costs
705ms, which is what that term actually looks like.

The 48x in this table's previous version was that difference. We now carry
GNU's heuristic, so the row is 1.57x instead, and the last table is the
honest measurement of the algorithm itself: 1.21x and 1.27x against GNU with
both sides minimal.

On ordinary inputs, a handful of changes in a file of any size, we are 1.5x
to 2.8x slower, and that is constant-factor cost in reading and hashing, not
asymptotics. The 400000-line row is the one to watch if that ever matters.

## Build

```sh
make            # gcc -std=c11 -O2 -g, no dependencies
make test       # the suite, needs diff, patch, python3
make test-asan  # the same suite under ASan + UBSan
make bench      # the tables above
make clean
```

`-ffile-prefix-map=$(CURDIR)=.` is in `CFLAGS` so that `-g` does not write
the build directory into DWARF `.debug_line_str`. The build is reproducible
across directory, timezone, locale, umask, hostname, user and `-j`.

Scratch files go in `t/scratch/`, under the repo, never `/tmp`. The suite
keys that directory on the binary name so `make test` and `make test-asan`
can run at the same time.

`make test-asan` also checks that ASan can still see anything at all. The
undiscarded sequences are allocated at the size they would need if nothing
were discarded, and most of what is allocated is then unused, so a read one
past the end of one of them lands inside a live allocation and is reported
by nobody. GNU packs all four arrays into a single block, which has exactly
that shape. Here each is a separate allocation and each is realloc'd down to
the length actually used, and the suite plants the over-read and requires
ASan to report it: with the trim, 96 of the 500 fuzz pairs trip it; with the
four arrays packed back into one block, 0 of 500 do, and the sanitizer run
reports a clean pass over a program that is reading out of bounds. Three
other repositories in this collection have had a bounds bug hidden that way.

`detect_leaks=0` is set for `test-asan`, deliberately. This program reads two
files, writes a diff and exits, so nothing is freed and LeakSanitizer reports
every allocation in it. Eight per run, all live until exit, drowning out the
bounds findings that are the reason to run ASan at all.

## Notes from building it

Three things cost real time and are worth writing down.

Boundary normalisation is most of the difficulty, not the algorithm. The
D-path search is about 120 lines and worked almost immediately. Making it
agree with GNU took two more rounds, because a shortest edit script is not
unique and the ambiguity is common, not exotic: given `a b` against
`a b a b`, inserting the new pair before or after the old one are both
minimal. GNU resolves it afterwards by sliding runs of changes forward and
merging them, and reproducing that exactly is where the bugs were.

GNU diff's analysis depends on `-U`. `diff -U0` and `diff -u` on the same
pair of files can report an insertion at different line numbers. This looks
like a bug in whichever program you trust less, and it is not: GNU leaves
`context` lines of the common prefix and suffix inside the region it
normalises, so a run can slide N lines further with `-U N` than with `-U 0`.
It is measurable from the outside, feed GNU an insertion into a run of
identical lines next to the common tail and the reported line number moves by
exactly N, and until that was found, five corpus pairs disagreed for
reasons that made no sense.

Two of the harness's checks could not fail when first written, which is
the failure mode that matters more than any bug in the code:

- The equivalence-class check compared our class count against
  `cat a b | sort -u | wc -l`. Every fixture ended in a newline, so deleting
  the trailing-newline flag from both the hash and the comparator still
  printed "5 passed". Worse, `sort -u` *cannot* check that flag at all, since
  `cat` welds the unterminated tail of the first file onto the first line of
  the second and the distinction is destroyed before sort runs. The check now
  says so in a comment instead of implying coverage it does not have.
- Every comparison in the suite was `[ "$(cmd)" = "$(cmd)" ]`. Command
  substitution strips trailing newlines from both sides, so a diff that ended
  without its final newline, exactly what this project exists to get right,
  compared equal. Everything goes through files and `cmp` now.
- And a third, found later: `--runs` printed changed line *contents*, so a run
  marked at the wrong offset with the same text compared equal. It prints line
  numbers now, which is how the `-U`-dependent behaviour above was finally
  pinned down.
