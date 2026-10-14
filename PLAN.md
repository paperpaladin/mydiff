# PLAN

## Contract

```sh
mydiff [-U N] [--minimal] A B        # unified diff of A and B, to stdout
mydiff -p PATCH ORIG                 # ORIG with PATCH applied, to stdout
mydiff --stat A B                    # line / class / changed-line counts
mydiff --runs A B                    # the raw edit script, no formatting
mydiff --hunks PATCH                 # re-print the @@ headers it parsed
```

Compute a shortest edit script between the lines of `A` and `B` by Myers'
algorithm, and print it in unified diff format. Default context is 3 lines;
`-U N` changes it. `--minimal` disables the two non-minimal heuristics that
GNU diff enables by default.

`-p` goes the other way: parse a unified diff and apply it to `ORIG`, writing
the result to stdout.

A line is the bytes up to and including `\n`. A trailing chunk with no `\n` is
still a line, and prints with `\ No newline at end of file` after it. An empty
file has zero lines.

Exit status is GNU diff's: `0` no differences, `1` differences, `2` trouble.
In `-p` mode, `0` applied cleanly, `2` did not.

The four non-default modes exist because the suite uses them. They are
oracle hooks, not features: `--stat` and `--runs` expose the intermediate
results so an independent GNU output format can check them before any
formatting code is involved, and `--hunks` makes the patch parser checkable
without applying anything.

## Hard invariant

`mydiff A B` and `diff -u A B` produce the same bytes.

Not "equivalent output", not "the same modulo formatting": the same bytes,
headers and mtime timestamps included, so there is nothing to normalise away
before comparing. Myers' algorithm produces a minimal edit script and GNU
diffutils implements the same algorithm, so exact agreement is achievable and
any disagreement is a real difference in behaviour and never a formatting preference.

Where that invariant does not hold, it holds against `diff -u --minimal`
instead, and the suite proves that the gap is GNU disagreeing with itself,
not us being wrong: see Non-goals.

Two corollaries, which are what make the project bidirectional:

1. GNU patch applied to our diff reproduces `B` from `A`, exactly.
2. Our `-p` applied to GNU diff's output reproduces `B` from `A`, exactly.

## Oracle

GNU diffutils 3.10 and GNU patch 2.8 are the authority. There is not one
hand-written expected diff anywhere in the suite. Ranked by how little
control we have over them:

1. `diff -u A B`, byte for byte. The whole stream, including the two
   header lines with their nanosecond mtimes, which we format the way GNU's
   `time_format` does so they can be compared, not skipped. 93
   generated corpus pairs and 400 adversarial ones.
2. GNU patch accepts what we write. `patch -F0` applied to our diff must
   turn `A` into `B`. Independent of diffutils and much pickier about the
   `\ No newline` marker.
3. We accept what GNU diff writes. `mydiff -p` on `diff -u` output must
   turn `A` into `B`. The other direction of the same loop.
4. `diff --minimal -U N`, byte for byte. Stricter than (1) in the one
   place (1) is wrong about us: GNU's default enables heuristics we do not
   implement, and the suite asserts this one unconditionally.
5. GNU's line-group formats for the raw edit script. A completely
   different output path in diffutils
   (`--old-group-format` and friends) prints run boundaries and changed line
   numbers with no context, no hunk merging and no @@ arithmetic, which pins
   the script down before any formatter exists.
6. `diff --old-line-format='-' --new-line-format='+'` for edit distance.
   Counts of changed lines per file, invariant under boundary normalisation,
   so minimality can be checked separately from placement.
7. `grep '^@@'` over real diff output, for the patch parser. Re-printing
   what we parsed must reproduce those lines, which catches a defaulted field.
8. `cat a b | sort -u | wc -l` for the equivalence classes. Weakest, and
   explicitly annotated as blind to the trailing-newline flag, because `cat`
   destroys that information before `sort` sees it.
9. The sanitizers and valgrind. Not correctness oracles; they only say
   the answer was not produced by reading out of bounds.

## Non-goals

- GNU's `discard_confusing_lines`. The one deliberate gap in the hard
  invariant. GNU diff by default discards lines it judges confusing before
  running the comparison, which changes which of several equally short edit
  scripts comes out. We do not implement it, so on inputs with a very small
  line alphabet and dense changes we return a different minimal script. The
  suite measures this instead of hiding it: 388 of 400 adversarial pairs
  match plain `diff -u`, all 400 match `diff -u --minimal`, and every one of
  the 12 is checked to be a case where GNU's own default and `--minimal`
  disagree. On the 93-pair corpus the gap never appears.
- Replacing patch(1). No fuzz, no offset search, no reversed-patch
  detection, no multi-file patches, no file creation or deletion, no
  `Index:`/git headers, no backups. Strict application only.
- Any output style but unified. No context (`-c`), normal, ed, RCS,
  side-by-side or if-then-else output.
- Any of diff's ~60 other options. No `-b`/`-w`/`-i`/`-B`, no recursive
  directory comparison, no `--label`, no colour, no binary-file detection.
- Streaming. Both files are read fully into memory. A 400000-line file is
  fine. A file larger than RAM is not.
- Character encodings. A line is bytes. No CRLF normalisation, no
  Unicode-aware comparison.

## A hang observed once, and not reproduced

On 2026-10-10 a `mydiff --stat` process was found holding a core at 99% for
fifteen hours and twenty-one minutes. The backtrace before killing it:

```
compareseq (xlim=51, ylim=-169)      <- from compareseq(xlim=223, ylim=35)
```

`ylim = -169` against a `yoff` of 0. `compareseq` recurses on
`(xoff, part.xmid, yoff, part.ymid)`, so `diag()` returned a split point
below the bottom of the range it was splitting. Both halves then get an
inverted range, `diag()` scans one without a bound, and it never returns.

Not reproduced, and not claimed as fixed. The process started at 11:16,
three commits before the code that exists now, and its inputs are
deterministic (`awk srand(7)` and `srand(9)`) and complete in under a second
against the current binary. It was most likely fixed by one of 31e7ac9,
bb6313d or ae52337 and never cleaned up: its parent run was killed from
outside, and the child was reparented and forgotten.

What was wrong regardless is that nothing noticed for fifteen hours. Two
things were added, both about what the next occurrence costs:

1. `compareseq` checks that the split point lies inside the range it splits
   and aborts with the numbers if not. A split outside its own range is
   wrong under every input, reproducible or not, and the check is two
   comparisons per level.
2. Every invocation in `t/run.sh` is bounded (`M_TIMEOUT`, default 30s,
   roughly a hundred times the slowest case) by pointing `$M` at a
   wrapper, not editing thirty-two call sites, so anything added later is
   covered without having to remember.

Both were checked by planting the failure, not by reasoning:
`part.ymid = yoff - 1` gives `split (500,-1) outside x[0,500] y[0,501]`, and
a binary that sleeps forever gives exit 124 at the bound.

The wrapper lives in a `mktemp` directory, not in `t/scratch`, because the
suite wipes that: the first version put it there and 1339 assertions
failed against a binary deleted out from under them.

Termination is a property a diff algorithm has to have. This suite tested
every answer the program gives and never once tested that it gives one.

## Commit sequence

- [x] Read two files and split them into lines, GNU's way
      why: the line convention is not a free choice, byte-identical output
      means a trailing chunk with no newline is a line, and `"a"` and `"a\n"`
      must compare unequal. Everything else depends on getting this first.
- [x] Hash lines into dense equivalence classes; `sort -u` is the oracle
      why: the Myers inner loop runs O((N+M)D) times and must compare ints,
      not strings. Also the first harness defect: the check could not fail
      until a collision-prone fixture was added.
- [x] Myers' middle-snake D-path search, in O(N+M) space
      why: the core. Linear space, not the textbook stored-trace
      version, with GNU's tie-breaks copied, not invented, because the
      tie-breaks decide which shortest script comes out.
- [x] Group changed flags into an edit script
      why: two boolean arrays kill the ordering ambiguity that makes minimal
      scripts non-unique. Checked against a GNU output path that shows run
      boundaries and nothing else.
- [x] Normalise run boundaries the way GNU does, and only over the middle
      why: without it we return perfectly minimal scripts that disagree with
      GNU. Two bugs here, the second one only visible because GNU appeared to
      contradict itself.
- [x] Unified output: hunk grouping, context windows, @@ ranges, headers
      why: the actual deliverable, and where the harness turned out to be
      comparing shell strings and therefore blind to trailing newlines.
- [x] The no-trailing-newline marker
      why: without it the diff is worse than cosmetically wrong. It is
      unparseable: the next line's prefix is glued onto this one.
- [x] -U N
      why: moves the hunk-merge threshold (2N+1) as well as the window, so it
      re-partitions hunks and exercises the grouping at five settings.
- [x] Parse unified hunk headers, with re-printing as the check
      why: four spellings of the same header, and a parser that defaults the
      wrong field applies hunks at the wrong line silently.
- [x] Apply a unified diff
      why: closes the loop in both directions, the point of the
      project: two reference tools checking each other through us.
- [x] The 93-pair corpus
      why: the cases where implementations actually diverge, generated rather
      than recorded. Found the context-dependent-analysis behaviour.
- [x] The adversarial fuzz family, with its two claims kept apart
      why: a hard assertion against `--minimal` and a measurement against the
      default, so the known gap cannot hide a real bug.
- [x] make test-asan and make bench
      why: a diff parses two files and a patch file chosen by somebody else.
      And the O((N+M)D) claim should be measured, not asserted.
- [ ] `discard_confusing_lines`, to close the last 12 of 400
      why: it is the only remaining difference from `diff -u`'s default and
      the only reason the hard invariant needs an asterisk. It is also GNU's
      largest speed advantage: 49x at D=20000, so this is the same item
      as the performance gap, not a separate one. Deferred because it is a
      heuristic with thresholds, so it must be reconstructed by measurement
      against GNU, not by reasoning, and the current suite is exactly
      the instrument for that.
- [ ] Buffer `-p` output instead of streaming it
      why: a patch that fails on hunk three has already written hunks one and
      two to stdout. Exit 2 says the output is garbage but a careless caller
      will have kept it. Costs one copy of the file in memory.
- [ ] `-c` context output
      why: the second most common format, and it reuses the whole analysis;
      only the printer changes. Mostly it would be a second independent check
      that the hunk grouping is right, since `diff -c` merges hunks by the
      same rule.
- [ ] `-b` and `-w`
      why: the first options that change the *comparison*, not the
      output, so they test whether the equivalence-class layer is actually a
      layer. Cheap: a different hash and comparator, nothing else moves.
