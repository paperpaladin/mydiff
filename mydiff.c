/* mydiff -- Myers diff in one file: compute a minimal edit script between two
 * text files and print it as a unified diff, and apply one back again.
 *
 * The oracle is GNU diffutils. `diff -u A B` and `mydiff A B` are required to
 * agree byte for byte, headers and timestamps included, so the file-reading
 * and line-splitting conventions below are GNU's, not ones of our choosing:
 * a line is the bytes up to and including '\n', a final chunk with no '\n' is
 * still a line, and an empty file has zero lines.
 */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

static const char *prog = "mydiff";

static void die(const char *what, const char *name)
{
	fprintf(stderr, "%s: %s%s%s: %s\n", prog, what, name ? ": " : "",
	        name ? name : "", strerror(errno));
	exit(2);
}

/* A line, with its newline split off. Two lines are equal iff both the bytes
 * and the newline flag match: that is what makes "a" and "a\n" different
 * lines, which is the whole no-trailing-newline question. */
struct line {
	const char *p;
	size_t n;	/* bytes, excluding the terminator */
	int nl;		/* 1 if the line ended with '\n' in the file */
};

struct file {
	const char *name;
	char *buf;
	size_t len;
	struct line *l;
	long n;
	struct timespec mtime;
};

static void *xmalloc(size_t n)
{
	void *p = malloc(n ? n : 1);
	if (!p)
		die("out of memory", NULL);
	return p;
}

static void slurp(struct file *f, const char *name)
{
	FILE *fp;
	struct stat st;
	size_t cap = 0, got = 0;
	long i;

	f->name = name;
	fp = fopen(name, "rb");
	if (!fp)
		die("cannot read", name);
	if (fstat(fileno(fp), &st) != 0)
		die("cannot stat", name);
	f->mtime = st.st_mtim;

	/* st_size is a hint only: /proc and friends report 0. */
	cap = st.st_size > 0 ? (size_t)st.st_size + 1 : 65536;
	f->buf = xmalloc(cap);
	for (;;) {
		size_t r = fread(f->buf + got, 1, cap - got, fp);
		got += r;
		if (got < cap)
			break;
		cap *= 2;
		f->buf = realloc(f->buf, cap);
		if (!f->buf)
			die("out of memory", NULL);
	}
	if (ferror(fp))
		die("read error", name);
	fclose(fp);
	f->len = got;

	/* Count lines, then fill. Two passes beats growing an array. */
	f->n = 0;
	for (i = 0; (size_t)i < got; i++)
		if (f->buf[i] == '\n')
			f->n++;
	if (got && f->buf[got - 1] != '\n')
		f->n++;

	f->l = xmalloc(sizeof *f->l * (size_t)(f->n + 1));
	{
		size_t start = 0;
		long k = 0;
		for (i = 0; (size_t)i < got; i++) {
			if (f->buf[i] != '\n')
				continue;
			f->l[k].p = f->buf + start;
			f->l[k].n = (size_t)i - start;
			f->l[k].nl = 1;
			k++;
			start = (size_t)i + 1;
		}
		if (start < got) {
			f->l[k].p = f->buf + start;
			f->l[k].n = got - start;
			f->l[k].nl = 0;
			k++;
		}
		f->n = k;
	}
}

int main(int argc, char **argv)
{
	struct file a, b;

	if (argc != 3) {
		fprintf(stderr, "usage: %s A B\n", prog);
		return 2;
	}
	slurp(&a, argv[1]);
	slurp(&b, argv[2]);
	printf("%ld %ld\n", a.n, b.n);
	return 0;
}
