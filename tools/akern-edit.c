#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum { CONTEXT_LINES = 3 };

typedef struct { char *p; size_t n; } text;

static int fail(const char *path, const char *why)
{
    fprintf(stderr, "akern-edit: %s: %s\n", path, why);
    return 1;
}

static int usage(void)
{
    puts("akern edit PATH OLD NEW [--all] — replace OLD with NEW; OLD must match exactly once unless --all");
    return 0;
}

static bool load(const char *path, text *t)
{
    FILE *f = fopen(path, "r");
    if (!f) return false;
    size_t cap = 4096;
    t->p = malloc(cap);
    t->n = 0;
    for (size_t r; t->p && (r = fread(t->p + t->n, 1, cap - t->n, f)) > 0;) {
        t->n += r;
        if (t->n == cap) {
            char *bigger = realloc(t->p, cap *= 2);
            if (!bigger) free(t->p);
            t->p = bigger;
        }
    }
    fclose(f);
    if (!t->p) errno = ENOMEM;
    return t->p != NULL;
}

static size_t count_matches(const text *t, const char *old)
{
    size_t n = 0, len = strlen(old);
    for (const char *p = t->p, *e = t->p + t->n; p + len <= e; p++)
        if (memcmp(p, old, len) == 0) n++, p += len - 1;
    return n;
}

static text replace_matches(const text *t, const char *old, const char *new, size_t *first_offset)
{
    size_t olen = strlen(old), nlen = strlen(new), hits = count_matches(t, old);
    text out = { malloc(t->n + hits * nlen + 1), 0 };
    *first_offset = (size_t)-1;
    for (const char *p = t->p, *e = t->p + t->n; p < e;) {
        if (p + olen <= e && memcmp(p, old, olen) == 0) {
            if (*first_offset == (size_t)-1) *first_offset = out.n;
            memcpy(out.p + out.n, new, nlen);
            out.n += nlen;
            p += olen;
        } else {
            out.p[out.n++] = *p++;
        }
    }
    return out;
}

static bool save_atomically(const char *path, const text *t)
{
    struct stat st;
    mode_t mode = stat(path, &st) == 0 ? st.st_mode & 07777 : 0644;
    char tmp_path[4200];
    snprintf(tmp_path, sizeof tmp_path, "%s.akern-edit.%ld", path, (long)getpid());
    int fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, mode);
    bool ok = fd >= 0 && write(fd, t->p, t->n) == (ssize_t)t->n && close(fd) == 0 && rename(tmp_path, path) == 0;
    if (!ok) unlink(tmp_path);
    return ok;
}

static long line_number_at(const text *t, size_t offset)
{
    long line = 1;
    for (size_t i = 0; i < offset && i < t->n; i++) line += t->p[i] == '\n';
    return line;
}

static void print_lines(const text *t, long from, long to)
{
    long line = 1;
    const char *start = t->p;
    for (const char *p = t->p, *e = t->p + t->n; p <= e; p++) {
        if (p < e && *p != '\n') continue;
        if (p == e && start == e) break;
        if (line >= from && line <= to) printf("%6ld\t%.*s\n", line, (int)(p - start), start);
        start = p + 1;
        line++;
    }
}

int main(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "--help") == 0) return usage();
    bool all = argc == 5 && strcmp(argv[4], "--all") == 0;
    if (argc != 4 && !all) return fail("usage", "akern-edit PATH OLD NEW [--all]");
    const char *path = argv[1], *old = argv[2], *new = argv[3];
    text t;
    if (!*old) return fail(path, "OLD must not be empty");
    if (!load(path, &t)) return fail(path, strerror(errno));

    size_t hits = count_matches(&t, old);
    char why[128];
    if (hits == 0) return fail(path, "OLD text not found");
    if (hits > 1 && !all) {
        snprintf(why, sizeof why, "OLD text matches %zu times", hits);
        return fail(path, why);
    }

    size_t first_offset;
    text out = replace_matches(&t, old, new, &first_offset);
    if (!save_atomically(path, &out)) return fail(path, strerror(errno));
    long first = line_number_at(&out, first_offset), new_lines = 1;
    for (const char *p = new; *p; p++) new_lines += *p == '\n';
    printf("Edited %s: replaced %zu occurrence%s. First change at line %ld:\n", path, hits, hits == 1 ? "" : "s", first);
    print_lines(&out, first - CONTEXT_LINES, first + new_lines - 1 + CONTEXT_LINES);
    return 0;
}
