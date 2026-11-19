#include "files.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "msg.h"

enum {
    DEFAULT_READ_LINES = 2000,
    MAX_READ_BYTES     = 100000,
    MAX_LINE_CHARS     = 2000,
    MAX_MEDIA_BYTES    = 32 << 20,
    BINARY_SNIFF_BYTES = 8192,
};

static bool set_result_text(json_buf *result, bool ok, const char *fmt, ...)
{
    char text[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    content_from_text(result, S(text));
    return ok;
}

static bool read_media(FILE *f, const char *path, off_t size_bytes, const char *media_type, json_buf *result)
{
    if (size_bytes > MAX_MEDIA_BYTES)
        return set_result_text(result, false, "%s: %lld bytes is too large to view", path, (long long)size_bytes);
    buf bytes = { 0 };
    json_list blocks = { 0 };
    char chunk[65536];
    for (size_t n; (n = fread(chunk, 1, sizeof chunk, f)) > 0;) buf_put(&bytes, chunk, n);
    blocks_add_media(&blocks, media_type, buf_span(&bytes));
    json_printf(result, "[%R]", buf_span(&blocks));
    buf_free(&bytes);
    buf_free(&blocks);
    return true;
}

static bool starts_char(char byte)
{
    return ((unsigned char)byte & 0xC0) != 0x80;
}

static void put_clipped_line(buf *text, const char *line, size_t len)
{
    size_t end = len && line[len - 1] == '\n' ? len - 1 : len, cut = 0, kept_chars = 0, more_chars = 0;
    for (; cut < end && (kept_chars < MAX_LINE_CHARS || !starts_char(line[cut])); cut++) kept_chars += starts_char(line[cut]);
    if (cut == end) {
        buf_put(text, line, len);
        return;
    }
    for (size_t i = cut; i < end; i++) more_chars += starts_char(line[i]);
    buf_put(text, line, cut);
    buf_printf(text, "… (%zu more characters)\n", more_chars);
}

static bool read_lines(FILE *f, const char *path, long first_line, long max_lines, json_buf *result)
{
    buf text = { 0 };
    char *line = NULL;
    size_t cap = 0;
    long line_no = 0, last_shown = 0;
    bool bytes_spent = false;
    for (ssize_t len; (len = getline(&line, &cap, f)) >= 0;) {
        line_no++;
        if (line_no < first_line || line_no >= first_line + max_lines || bytes_spent) continue;
        if (text.n + (size_t)len > MAX_READ_BYTES && last_shown) {
            bytes_spent = true;
            continue;
        }
        put_clipped_line(&text, line, (size_t)len);
        last_shown = line_no;
    }
    free(line);
    long total_lines = line_no;
    if (total_lines && first_line > total_lines) {
        buf_free(&text);
        return set_result_text(result, false, "%s has only %ld lines", path, total_lines);
    }
    if (last_shown && last_shown < total_lines) {
        if (text.n && text.p[text.n - 1] != '\n') buf_putc(&text, '\n');
        buf_printf(&text, "[lines %ld-%ld of %ld; read on with offset %ld]", first_line, last_shown, total_lines,
                   last_shown + 1);
    }
    content_from_text(result, buf_span(&text));
    buf_free(&text);
    return true;
}

bool tool_read(json input, json_buf *result)
{
    char *path = json_text_dup(json_get(input, "path")), *media_type = json_text_dup(json_get(input, "type"));
    struct stat st;
    if (!path) return set_result_text(result, false, "path is required");
    long first_line = (long)json_num(json_get(input, "offset"), 1);
    long max_lines = (long)json_num(json_get(input, "limit"), DEFAULT_READ_LINES);
    bool ok;
    FILE *f = NULL;
    char sniff[BINARY_SNIFF_BYTES];
    size_t sniffed = 0;
    if (stat(path, &st) != 0) ok = set_result_text(result, false, "%s: %s", path, strerror(errno));
    else if (S_ISDIR(st.st_mode)) ok = set_result_text(result, false, "%s is a directory", path);
    else if (!(f = fopen(path, "rb"))) ok = set_result_text(result, false, "%s: %s", path, strerror(errno));
    else if (media_type) ok = read_media(f, path, st.st_size, media_type, result);
    else {
        sniffed = fread(sniff, 1, sizeof sniff, f);
        rewind(f);
        if (memchr(sniff, 0, sniffed))
            ok = set_result_text(result, false, "%s is a binary file (%lld bytes)", path, (long long)st.st_size);
        else
            ok = read_lines(f, path, first_line < 1 ? 1 : first_line, max_lines < 1 ? 1 : max_lines, result);
    }
    if (f) fclose(f);
    free(path);
    free(media_type);
    return ok;
}

static bool set_result_errno(json_buf *result, const char *path)
{
    if (errno == ENOENT) return set_result_text(result, false, "%s: directory does not exist", path);
    return set_result_text(result, false, "%s: %s", path, strerror(errno));
}

static bool append_file(const char *path, span text, json_buf *result)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    bool ok = fd >= 0 && write_all(fd, text);
    if (!ok) set_result_errno(result, path);
    if (fd >= 0) close(fd);
    return ok && set_result_text(result, true, "Appended %zu bytes to %s", text.n, path);
}

static bool replace_file(const char *path, span text, json_buf *result)
{
    struct stat st;
    bool existed = stat(path, &st) == 0;
    buf tmp_path = { 0 };
    buf_printf(&tmp_path, "%s.akern-%d", path, (int)getpid());
    int fd = open(tmp_path.p, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, existed ? st.st_mode & 07777 : 0644);
    bool ok = fd >= 0 && write_all(fd, text);
    if (fd >= 0 && close(fd) != 0) ok = false;
    if (ok && rename(tmp_path.p, path) != 0) ok = false;
    if (!ok) {
        set_result_errno(result, path);
        if (fd >= 0) unlink(tmp_path.p);
    }
    buf_free(&tmp_path);
    return ok && set_result_text(result, true, "%s %s (%zu bytes)", existed ? "Replaced" : "Created", path, text.n);
}

bool tool_write_file(json input, json_buf *result)
{
    char *path = json_text_dup(json_get(input, "path"));
    if (!path) return set_result_text(result, false, "write needs a path or a job");
    buf text = { 0 };
    json_text(json_get(input, "content"), &text);
    bool ok = json_true(json_get(input, "append")) ? append_file(path, buf_span(&text), result)
                                                    : replace_file(path, buf_span(&text), result);
    buf_free(&text);
    free(path);
    return ok;
}
