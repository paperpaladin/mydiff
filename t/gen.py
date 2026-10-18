#!/usr/bin/env python3
"""Generate the test corpus: pairs of files chosen to hit code paths.

Usage: gen.py DIR  -- writes DIR/NNN-name.a and DIR/NNN-name.b

Nothing here records an expected diff. The expected answer always comes from
GNU diffutils at run time; this only decides what to ask about. The cases are
picked for where real diff implementations are known to disagree, not to look
varied: the trailing-newline family is over-represented on purpose, and so are
the degenerate sizes (0 and 1 lines), because that is where off-by-one errors
in the @@ ranges live.
"""
import os
import random
import sys


def fuzz(outdir, count):
    """Adversarial pairs: tiny line alphabets, where many different minimal
    edit scripts exist and the only thing deciding between them is the
    boundary normaliser. This is the family that found every normalisation
    bug in this project, and it is also the only family where GNU diff's
    default output differs from its own --minimal output, so the harness
    checks the two claims separately."""
    os.makedirs(outdir, exist_ok=True)
    for f in os.listdir(outdir):
        os.remove(os.path.join(outdir, f))
    rnd = random.Random(4242)
    for i in range(count):
        alpha = rnd.choice([1, 2, 3, 5, 20, 200])
        L = rnd.randrange(0, 160)
        a = ["v%d" % rnd.randrange(alpha) for _ in range(L)]
        b = list(a)
        for _ in range(rnd.randrange(0, 35)):
            op = rnd.choice("idr")
            if op == "i":
                b.insert(rnd.randrange(len(b) + 1), "v%d" % rnd.randrange(alpha))
            elif op == "d" and b:
                del b[rnd.randrange(len(b))]
            elif b:
                b[rnd.randrange(len(b))] = "v%d" % rnd.randrange(alpha)
        sa = "".join(x + "\n" for x in a)
        sb = "".join(x + "\n" for x in b)
        if rnd.random() < 0.25 and sa:
            sa = sa[:-1]
        if rnd.random() < 0.25 and sb:
            sb = sb[:-1]
        u = rnd.choice([0, 1, 2, 3, 3, 3, 5, 10])
        base = os.path.join(outdir, "%04d-U%d" % (i + 1, u))
        with open(base + ".a", "w") as fh:
            fh.write(sa)
        with open(base + ".b", "w") as fh:
            fh.write(sb)
    confusing(outdir, count, rnd)
    print(count + CONFUSING)


CONFUSING = 100


def confusing(outdir, first, rnd):
    """Pairs built to be long runs of discardable lines.

    The pairs above do not reach most of discard_confusing_lines. Its outer
    rule, "throw away a line that matches nothing and a line that matches
    very many", fires on anything; its inner rules only apply inside a RUN of
    discardable lines, and they measure that run: a quarter of it provisional
    cancels the lot, a subrun of about sqrt(length/4) provisionals survives,
    three definite discards in a row or the first one 8 lines in ends the
    cancelling at each end. A run has to be dozens of lines long before those
    numbers mean anything, and 160 lines of a 3-symbol alphabet never builds
    one. Mutating those constants left the suite green.

    So each pair here is a frame of lines that appear exactly once on both
    sides (never discardable), wrapped around long stretches that mix lines
    unique to one file (definitely discardable) with lines repeated far more
    than the threshold (provisionally discardable), at a ratio drawn per
    pair so that the quarter rule lands on both sides of its boundary."""
    for i in range(CONFUSING):
        runlen = rnd.choice([8, 17, 33, 70, 140])
        # Fraction of each run that is provisional rather than definite.
        prov = rnd.choice([0.0, 0.1, 0.2, 0.25, 0.4, 0.6, 0.9])
        frames = rnd.randrange(2, 6)
        hot = ["h%d" % k for k in range(rnd.choice([1, 2, 3]))]
        a, b, uniq = [], [], [0]

        # Half the pairs open with a hot line on the A side. The class
        # numbers are handed out in order, so that makes the hot value
        # class 0, and class 0 is the one GNU's marking loop skips --
        # because -I and -B put the lines they ignore there, and we have
        # neither option. Copying that skip would exempt this value from
        # the heuristic; with no pair that starts this way, copying it
        # changed nothing anywhere and the suite stayed green.
        if hot and rnd.random() < 0.5:
            a.append(hot[0])
            b.append("blead%d" % i)

        def run(dst, tag):
            for _ in range(runlen):
                if rnd.random() < prov:
                    dst.append(rnd.choice(hot))
                else:
                    uniq[0] += 1
                    dst.append("%s%d" % (tag, uniq[0]))

        for f in range(frames):
            anchor = ["anchor%d_%d" % (i, f * 3 + k) for k in range(3)]
            a += anchor
            b += anchor
            run(a, "onlyA")
            run(b, "onlyB")
            # The hot lines have to be frequent on BOTH sides or they are
            # merely unmatched, which is the other rule.
            for _ in range(runlen):
                a.append(rnd.choice(hot))
                b.append(rnd.choice(hot))
        # Two values landing exactly on the provisional threshold, which
        # is "more than 5" for a file this short. One appears 5 times on
        # the B side and one 6 times, so > 5 and >= 5 are different
        # answers for the first, 5 and 6 different answers for the second.
        # Without them the threshold could be moved by one in either
        # direction and nothing in the suite noticed.
        for val, n in (("warm5_%d" % i, 5), ("warm6_%d" % i, 6)):
            for _ in range(n):
                b.insert(rnd.randrange(len(b) + 1), val)
            for _ in range(rnd.randrange(1, 4)):
                a.insert(rnd.randrange(len(a) + 1), val)

        u = rnd.choice([0, 1, 3, 10])
        base = os.path.join(outdir, "%04d-U%d" % (first + i + 1, u))
        with open(base + ".a", "w") as fh:
            fh.write("".join(x + chr(10) for x in a))
        with open(base + ".b", "w") as fh:
            fh.write("".join(x + chr(10) for x in b))


def bench(outdir):
    """Pairs for the benchmark: edit distance varied at fixed file size, then
    file size varied at fixed edit distance. Myers is O((N+M)D), so the first
    series should grow roughly linearly in D and the second roughly linearly
    in N -- the point being that D, not file size, is what costs."""
    os.makedirs(outdir, exist_ok=True)
    for f in os.listdir(outdir):
        os.remove(os.path.join(outdir, f))
    rnd = random.Random(1)

    def write(name, a, b):
        with open(os.path.join(outdir, name + ".a"), "w") as fh:
            fh.write("".join(x + "\n" for x in a))
        with open(os.path.join(outdir, name + ".b"), "w") as fh:
            fh.write("".join(x + "\n" for x in b))

    N = 20000
    base = ["line %d of a file that looks a bit like source" % i
            for i in range(N)]
    # D series: k replacements costs 2k edits, scattered so no two are
    # adjacent and the edit distance really is 2k.
    for k in (1, 10, 100, 1000, 5000, 10000):
        b = list(base)
        for i in rnd.sample(range(N), k):
            b[i] = "EDITED %d" % i
        write("d-%06d" % (2 * k), base, b)
    # N series: 50 edits regardless of size.
    for n in (1000, 5000, 20000, 100000, 400000):
        a = base[:n] if n <= N else ["line %d of a file that looks a bit "
                                     "like source" % i for i in range(n)]
        b = list(a)
        for i in rnd.sample(range(n), min(50, n)):
            b[i] = "EDITED %d" % i
        write("n-%06d" % n, a, b)
    print("ok")


def main(outdir):
    os.makedirs(outdir, exist_ok=True)
    for f in os.listdir(outdir):
        os.remove(os.path.join(outdir, f))
    n = 0

    def pair(name, a, b):
        nonlocal n
        n += 1
        base = os.path.join(outdir, "%03d-%s" % (n, name))
        with open(base + ".a", "w") as fh:
            fh.write(a)
        with open(base + ".b", "w") as fh:
            fh.write(b)

    def lines(xs):
        return "".join(x + "\n" for x in xs)

    rnd = random.Random(20261009)
    base = ["line %d: %s" % (i, "x" * (i % 17)) for i in range(200)]

    # --- the degenerate sizes, where @@ range arithmetic goes wrong
    pair("both-empty", "", "")
    pair("empty-to-one", "", "only\n")
    pair("one-to-empty", "only\n", "")
    pair("empty-to-many", "", lines(base[:40]))
    pair("many-to-empty", lines(base[:40]), "")
    pair("identical", lines(base), lines(base))
    pair("one-line-change", "a\n", "b\n")
    pair("blank-only", "\n", "\n\n\n")

    # --- random line edits at three densities
    for tag, nedits in (("sparse", 3), ("medium", 25), ("dense", 120)):
        b = list(base)
        for _ in range(nedits):
            op = rnd.choice("idr")
            if op == "i":
                b.insert(rnd.randrange(len(b) + 1), "inserted %d" % rnd.randrange(1000))
            elif op == "d" and b:
                del b[rnd.randrange(len(b))]
            elif b:
                b[rnd.randrange(len(b))] = "replaced %d" % rnd.randrange(1000)
        pair("edits-" + tag, lines(base), lines(b))

    # --- insertions at the very head and the very tail. Both are where the
    # --- context window has to clamp instead of running off the end.
    pair("head-insert", lines(base), lines(["new head %d" % i for i in range(5)] + base))
    pair("tail-insert", lines(base), lines(base + ["new tail %d" % i for i in range(5)]))
    pair("head-delete", lines(base), lines(base[5:]))
    pair("tail-delete", lines(base), lines(base[:-5]))
    pair("head-and-tail", lines(base), lines(["h"] + base[1:-1] + ["t"]))
    pair("one-line-file-head", "only\n", lines(["new", "only"]))
    pair("one-line-file-tail", "only\n", lines(["only", "new"]))

    # --- whole-file replacement, and the pathological no-shared-lines case
    pair("whole-replace", lines(base), lines(["q %d" % i for i in range(200)]))
    pair("disjoint-equal-size", lines(["aaa%d" % i for i in range(300)]),
         lines(["bbb%d" % i for i in range(300)]))
    pair("disjoint-lopsided", lines(["aaa%d" % i for i in range(400)]),
         lines(["bbb%d" % i for i in range(7)]))

    # --- no trailing newline, hard. Every combination of (last line same or
    # --- different) x (A terminated or not) x (B terminated or not), plus the
    # --- single-line and empty variants, because this is where real
    # --- implementations diverge.
    for atail in (True, False):
        for btail in (True, False):
            for samelast in (True, False):
                a = lines(base[:20])
                bb = list(base[:20])
                if not samelast:
                    bb[-1] = "changed last"
                b = lines(bb)
                if not atail:
                    a = a[:-1]
                if not btail:
                    b = b[:-1]
                pair("nonl-a%d-b%d-same%d" % (atail, btail, samelast), a, b)
    pair("nonl-one-line", "x", "x\n")
    pair("nonl-one-line-rev", "x\n", "x")
    pair("nonl-empty", "", "x")
    pair("nonl-only-newline-diff", lines(base[:30]), lines(base[:30])[:-1])
    pair("nonl-both", lines(base[:30])[:-1], lines(base[:29] + ["other"])[:-1])
    pair("nonl-context", lines(base[:30])[:-1],
         lines(["changed"] + base[1:30])[:-1])

    # --- very long lines
    pair("long-lines",
         lines(["%d:%s" % (i, "y" * 5000) for i in range(20)]),
         lines(["%d:%s" % (i, "y" * 5000) for i in range(20)][:9]
               + ["9:" + "z" * 5000]
               + ["%d:%s" % (i, "y" * 5000) for i in range(10, 20)]))
    pair("one-huge-line", "a" * 300000 + "\n", "a" * 150000 + "b" + "a" * 149999 + "\n")
    pair("long-line-no-nl", "q" * 100000, "q" * 100000 + "\n")

    # --- differing only in trailing whitespace, which a diff must treat as a
    # --- real change (we have no -b/-w)
    pair("trailing-space", lines(base[:30]),
         lines([x + " " for x in base[:30]]))
    pair("trailing-space-one", lines(base[:30]),
         lines(base[:15] + [base[15] + "   "] + base[16:30]))
    pair("trailing-tab", lines(base[:30]),
         lines([x + "\t" for x in base[:30]]))
    pair("cr-endings", lines(base[:30]), lines([x + "\r" for x in base[:30]]))

    # --- 10000 lines, 50 scattered edits: D stays near 100 while N+M is
    # --- 20000, which is the case Myers is actually for.
    big = ["l%d" % i for i in range(10000)]
    big2 = list(big)
    for i in rnd.sample(range(10000), 50):
        big2[i] = "EDITED %d" % i
    pair("big-50-edits", lines(big), lines(big2))
    big3 = list(big)
    for i in sorted(rnd.sample(range(10000), 50), reverse=True):
        del big3[i]
    pair("big-50-deletes", lines(big), lines(big3))
    big4 = list(big)
    for i in sorted(rnd.sample(range(10000), 50), reverse=True):
        big4.insert(i, "ADDED %d" % i)
    pair("big-50-inserts", lines(big), lines(big4))

    # --- a small alphabet, where many different minimal scripts exist and the
    # --- boundary normaliser decides which one. This is the family that
    # --- caught both shift_boundaries bugs.
    for alpha in (2, 3, 7):
        a = ["v%d" % rnd.randrange(alpha) for _ in range(300)]
        b = list(a)
        for _ in range(80):
            op = rnd.choice("idr")
            if op == "i":
                b.insert(rnd.randrange(len(b) + 1), "v%d" % rnd.randrange(alpha))
            elif op == "d" and b:
                del b[rnd.randrange(len(b))]
            elif b:
                b[rnd.randrange(len(b))] = "v%d" % rnd.randrange(alpha)
        pair("alphabet-%d" % alpha, lines(a), lines(b))

    # --- repeated blocks, so the same text occurs at several offsets
    blk = ["BEGIN", "body 1", "body 2", "END"]
    pair("repeated-blocks", lines(blk * 20), lines(blk * 21))
    pair("repeated-blocks-rev", lines(blk * 21), lines(blk * 20))
    pair("moved-block", lines(base[:50]),
         lines(base[10:20] + base[:10] + base[20:50]))

    # --- a real file against itself with a patch applied: C source, which has
    # --- the brace-and-blank-line repetition that trips naive heuristics
    here = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..",
                        "mydiff.c")
    src = open(here).read().split("\n")
    pair("self-source", "\n".join(src) + "\n",
         "\n".join(src[:100] + ["/* inserted */"] + src[120:]) + "\n")
    pair("self-source-shuffled", "\n".join(src) + "\n",
         "\n".join(src[::-1]) + "\n")

    # --- plain random pairs, to catch what the hand-picked cases do not
    for i in range(40):
        alpha = rnd.choice([2, 4, 10, 60, 500])
        L = rnd.randrange(0, 150)
        a = ["w%d" % rnd.randrange(alpha) for _ in range(L)]
        b = list(a)
        for _ in range(rnd.randrange(0, 30)):
            op = rnd.choice("idr")
            if op == "i":
                b.insert(rnd.randrange(len(b) + 1), "w%d" % rnd.randrange(alpha))
            elif op == "d" and b:
                del b[rnd.randrange(len(b))]
            elif b:
                b[rnd.randrange(len(b))] = "w%d" % rnd.randrange(alpha)
        sa, sb = lines(a), lines(b)
        if rnd.random() < 0.3 and sa:
            sa = sa[:-1]
        if rnd.random() < 0.3 and sb:
            sb = sb[:-1]
        pair("random-%02d" % i, sa, sb)

    print(n)


if __name__ == "__main__":
    if len(sys.argv) > 2 and sys.argv[2] == "--bench":
        bench(sys.argv[1])
    elif len(sys.argv) > 3 and sys.argv[2] == "--fuzz":
        fuzz(sys.argv[1], int(sys.argv[3]))
    else:
        main(sys.argv[1] if len(sys.argv) > 1 else "corpus")
