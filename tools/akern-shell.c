/* akern-shell: the shell tool's program (tools.md §1).
     akern-shell [--output-limit N] [--stdout-name NAME] -c COMMAND
   Runs bash -c COMMAND (sh where there is no bash), its stdout and stderr on one pipe. The output
   goes on, as it comes, to stdout (the kernel streams it as tool_progress; not an image's or PDF's)
   and whole to $AKERN_OUT. The call's result, a JSON content array, goes to fd 3 (stdout when fd 3
   is not open): a header line, [exit N] or [signal N] and what of the output is not shown, then the
   output, up to N bytes; or, when the command succeeded and printed an image or a PDF, that as a
   block. Signals to the process group reach the command, and this program outlives them to
   report; it exits as the command did. */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "json.h"

enum { SNIFF_BYTES = 12, TEXT_SNIFF_BYTES = 8192 };
static const size_t MAX_MEDIA_BYTES = (size_t)32 << 20; /* its base64 fits in a line */

static const char *media_type(const unsigned char *b, size_t n)
{
    static const struct {
        const char *type, *magic;
        size_t at, n;
    } kinds[] = {
        {"image/png", "\x89PNG\r\n\x1a\n", 0, 8},
        {"image/jpeg", "\xff\xd8\xff", 0, 3},
        {"image/gif", "GIF87a", 0, 6},
        {"image/gif", "GIF89a", 0, 6},
        {"image/webp", "WEBP", 8, 4},
        {"application/pdf", "%PDF-", 0, 5},
    };
    for (size_t i = 0; i < LEN(kinds); i++)
        if (n >= kinds[i].at + kinds[i].n && !memcmp(b + kinds[i].at, kinds[i].magic, kinds[i].n) &&
            (kinds[i].at == 0 || !memcmp(b, "RIFF", 4)))
            return kinds[i].type;
    return NULL;
}

/* Text worth showing: no NUL, and at most one byte in eight outside UTF-8 in the first 8 KiB; a
   character cut in two at the end of that window does not count. */
static bool looks_like_text(span s)
{
    const unsigned char *p = (const unsigned char *)s.p;
    size_t n = s.n < TEXT_SNIFF_BYTES ? s.n : TEXT_SNIFF_BYTES, bad = 0;
    for (size_t i = 0, len; i < n; i += len ? len : 1)
    {
        if (!p[i])
            return false;
        len = p[i] < 0x80 ? 1 : json_utf8_len(p + i, n - i);
        if (!len && n - i < 4 && n < s.n)
            break;
        bad += !len;
    }
    return bad * 8 <= n;
}

static int open_output(const char *path, char **error)
{
    if (path)
        make_parent_dirs(path);
    int fd = path ? open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644) : -1;
    if (path && fd < 0)
        *error = strerror(errno);
    return fd;
}

int main(int argc, char **argv)
{
    size_t limit = 100000;
    const char *command = NULL, *name = NULL;
    for (int i = 1; i + 1 < argc; i += 2)
        if (!strcmp(argv[i], "--output-limit"))
            limit = (size_t)strtod(argv[i + 1], NULL);
        else if (!strcmp(argv[i], "--stdout-name"))
            name = argv[i + 1];
        else if (!strcmp(argv[i], "-c"))
            command = argv[i + 1];
    if (!command)
        die("usage: akern-shell [--output-limit N] [--stdout-name NAME] -c COMMAND");
    int result_fd = fcntl(3, F_GETFD) < 0 ? 1 : 3, out[2];
    if (pipe(out) < 0)
        die("pipe: %s", strerror(errno));
    /* Signals to the group are the command's; this program ignores them, from before the fork. */
    int caught[] = {SIGHUP, SIGINT, SIGQUIT, SIGTERM, SIGUSR1, SIGUSR2, SIGPIPE};
    sigset_t all, before;
    sigfillset(&all);
    sigprocmask(SIG_BLOCK, &all, &before);
    pid_t pid = fork();
    if (pid == 0)
    {
        sigprocmask(SIG_SETMASK, &before, NULL);
        dup2(out[1], 1), dup2(out[1], 2), close(out[0]), close(out[1]), close(3);
        execlp("bash", "bash", "-c", command, (char *)NULL);
        if (errno == ENOENT) /* no bash: sh */
            execlp("sh", "sh", "-c", command, (char *)NULL);
        dprintf(2, "bash: %s\n", strerror(errno));
        _exit(127);
    }
    for (size_t i = 0; i < LEN(caught); i++)
        signal(caught[i], SIG_IGN);
    sigprocmask(SIG_SETMASK, &before, NULL);
    close(out[1]);

    char *path = getenv("AKERN_OUT"), *file_error = NULL;
    int file = open_output(path, &file_error);
    const char *media = NULL;
    bool sniffed = false;
    buf kept = {0};
    size_t total = 0;
    char chunk[65536];
    for (ssize_t n; (n = read(out[0], chunk, sizeof chunk)) != 0;)
    {
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0)
            break;
        if (file >= 0 && !write_all(file, (span){chunk, (size_t)n}))
            file_error = strerror(errno), close(file), file = -1;
        total += (size_t)n;
        size_t room = !sniffed ? SIZE_MAX : (media ? MAX_MEDIA_BYTES : limit) - kept.n;
        buf_put(&kept, chunk, (size_t)n < room ? (size_t)n : room);
        if (!sniffed && kept.n >= SNIFF_BYTES)
        {
            sniffed = true, media = media_type((unsigned char *)kept.p, kept.n);
            if (!media && kept.n > limit)
                kept.n = limit;
        }
        if (!media)
            write_all(1, (span){chunk, (size_t)n});
    }
    if (!sniffed && (media = media_type((unsigned char *)kept.p, kept.n)) == NULL && kept.n > limit)
        kept.n = limit;
    int status;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
    bool ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;

    json_buf result = {0};
    if (ok && media && kept.n == total)
    {
        bool image = !strncmp(media, "image/", 6);
        json_printf(&result, "[{'type':%s", image ? "image" : "document");
        if (!image && name)
            json_printf(&result, ",'name':%s", name);
        json_printf(&result, ",'source':{'type':'base64','media_type':%s,'data':'", media);
        buf_put_base64(&result, buf_span(&kept));
        buf_puts(&result, "\"}}]");
    }
    else
    {
        buf text = {0};
        bool shown = !(media && !ok) && looks_like_text(buf_span(&kept));
        buf_printf(
            &text,
            "[%s %d",
            WIFSIGNALED(status) ? "signal" : "exit",
            WIFSIGNALED(status) ? WTERMSIG(status) : WEXITSTATUS(status)
        );
        if (!shown)
            buf_printf(
                &text,
                "; %zu bytes of %s not shown",
                total,
                media ? media : "binary output"
            );
        else if (total > kept.n)
            buf_printf(&text, "; cut at %zu of %zu bytes", kept.n, total);
        if (path && (!shown || total > kept.n))
            buf_printf(
                &text,
                file_error ? ", saving all of it to %s failed: %s" : ", all of it in %s",
                path,
                file_error
            );
        else if (file_error)
            buf_printf(&text, "; saving output to %s failed: %s", path, file_error);
        buf_putc(&text, ']');
        if (shown && kept.n)
            buf_putc(&text, '\n'), buf_putspan(&text, buf_span(&kept));
        json_printf(&result, "[{'type':'text','text':%S}]", buf_span(&text));
    }
    write_all(result_fd, buf_span(&result));
    if (WIFSIGNALED(status))
        signal(WTERMSIG(status), SIG_DFL), raise(WTERMSIG(status));
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}
