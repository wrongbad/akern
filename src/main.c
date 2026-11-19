#include "main.h"

#include <fcntl.h>
#include <float.h>
#include <getopt.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "calls.h"
#include "ctl.h"
#include "driver.h"
#include "inbox.h"
#include "loop.h"
#include "proc.h"
#include "session.h"

enum { DEFAULT_RECURSION_LIMIT = 4 };

enum { MAX_PROCESSES = 256, KILL_GRACE, MAX_LINE_BYTES };

static const struct option long_options[] = {
    { "print",          no_argument,       NULL, 'p' },
    { "session",        required_argument, NULL, 's' },
    { "model",          required_argument, NULL, 'm' },
    { "system",         required_argument, NULL, 'S' },
    { "param",          required_argument, NULL, 'P' },
    { "max-processes",  required_argument, NULL, MAX_PROCESSES },
    { "kill-grace",     required_argument, NULL, KILL_GRACE },
    { "max-line-bytes", required_argument, NULL, MAX_LINE_BYTES },
    { NULL, 0, NULL, 0 },
};

struct kernel {
    config  config;
    session session;
    procs   procs;
    inbox   inbox;
    calls   calls;
    driver  driver;
    ctl     ctl;
    loop    loop;
};

static void usage(void)
{
    fputs("usage: akern-loop [-p] [-s session.jsonl] [-m provider/model] [-S system.md] [-P key=value]...\n"
          "                  [--max-processes N] [--kill-grace SECONDS] [--max-line-bytes N] [prompt...]\n"
          "  -p, --print              print each run's final answer instead of the event stream\n"
          "  -s, --session PATH       session file (default: a new one in $TMPDIR)\n"
          "  -m, --model P/M          provider/model (default: $AKERN_MODEL)\n"
          "  -S, --system PATH        system prompt file (default: $AKERN_HOME/system.md)\n"
          "  -P, --param KEY=VALUE    provider parameter; VALUE is sent as JSON if it is JSON, else as a string\n"
          "      --max-processes N    exec programs running at once (default 32)\n"
          "      --kill-grace SECONDS from SIGTERM to SIGKILL (default 2)\n"
          "      --max-line-bytes N   longest stdin or provider line, and most exec output kept (default 67108864)\n",
          stderr);
    exit(2);
}

static void bad_value(const char *flag)
{
    fprintf(stderr, "akern-loop: invalid --%s: %s\n", flag, optarg);
    usage();
}

static double flag_number(const char *flag, double min, double max)
{
    char *end;
    double n = strtod(optarg, &end);
    if (end == optarg || *end || !(n >= min && n <= max)) bad_value(flag);
    return n;
}

static double flag_count(const char *flag, double max)
{
    double n = flag_number(flag, 1, max);
    if (n != floor(n)) bad_value(flag);
    return n;
}

static int env_int(const char *name, int fallback)
{
    const char *v = getenv(name);
    return v && *v ? atoi(v) : fallback;
}

static void add_param(config *cfg, const char *key_value)
{
    const char *eq = strchr(key_value, '=');
    if (!eq || eq == key_value) bad_value("param");
    span key = { key_value, (size_t)(eq - key_value) }, value = S(eq + 1);
    if (cfg->param_members.n) buf_putc(&cfg->param_members, ',');
    if (json_valid(value)) json_printf(&cfg->param_members, "%S:%R", key, json_trim(value));
    else json_printf(&cfg->param_members, "%S:%S", key, value);
}

static char *home_dir(void)
{
    const char *set = getenv("AKERN_HOME");
    if (set && *set) return xstrdup(set);
    buf path = { 0 };
    buf_printf(&path, "%s/.akern", getenv("HOME") ? getenv("HOME") : ".");
    return path.p;
}

static char *home_file(const config *cfg, const char *name)
{
    buf path = { 0 };
    buf_printf(&path, "%s/%s", cfg->home_dir, name);
    return path.p;
}

static int parse_args(config *cfg, int argc, char **argv)
{
    *cfg = (config){ .max_processes = 32, .kill_grace_seconds = 2, .max_line_bytes = 64u << 20 };
    for (int ch; (ch = getopt_long(argc, argv, "+ps:m:S:P:", long_options, NULL)) != -1;) {
        switch (ch) {
        case 'p':            cfg->print_answers = true; break;
        case 's':            cfg->session_path = optarg; break;
        case 'm':            cfg->provider_model = xstrdup(optarg); break;
        case 'S':            cfg->system_path = xstrdup(optarg); break;
        case 'P':            add_param(cfg, optarg); break;
        case MAX_PROCESSES:  cfg->max_processes = (int)flag_count("max-processes", INT_MAX); break;
        case KILL_GRACE:     cfg->kill_grace_seconds = flag_number("kill-grace", 0, DBL_MAX); break;
        case MAX_LINE_BYTES: cfg->max_line_bytes = (size_t)flag_count("max-line-bytes", SIZE_MAX / 2); break;
        default:             usage();
        }
    }
    if (!cfg->provider_model && getenv("AKERN_MODEL")) cfg->provider_model = xstrdup(getenv("AKERN_MODEL"));
    if (!cfg->provider_model) die("no model: pass -m provider/model or set AKERN_MODEL");
    cfg->recursion_limit = env_int("AKERN_RECURSION_LIMIT", DEFAULT_RECURSION_LIMIT);
    if (cfg->recursion_limit <= 0)
        die("recursion limit reached: this kernel would be nested too deep (AKERN_RECURSION_LIMIT)");
    cfg->home_dir = home_dir();
    if (!cfg->system_path) cfg->system_path = home_file(cfg, "system.md");
    cfg->tmp_dir = getenv("TMPDIR") && *getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp";
    buf_cstr(&cfg->param_members);
    return optind;
}

static void keep_std_fds_open(void)
{
    for (int fd = 0; fd <= 2; fd++)
        if (fcntl(fd, F_GETFD) < 0 && open("/dev/null", O_RDWR) != fd) die("cannot open /dev/null");
}

static void export_env(const config *cfg)
{
    char below[16];
    snprintf(below, sizeof below, "%d", cfg->recursion_limit - 1);
    setenv("AKERN_RECURSION_LIMIT", below, 1);
    setenv("AKERN_MODEL", cfg->provider_model, 1);
}

static void wire(struct kernel *k)
{
    const config *cfg = &k->config;
    sink *events = &k->ctl.out;
    k->session = (session){ .config = cfg, .events = events, .loop = &k->loop };
    k->procs   = (procs){ .config = cfg };
    k->inbox   = (inbox){ .events = events };
    k->calls   = (calls){ .config = cfg, .events = events, .procs = &k->procs, .inbox = &k->inbox,
                          .session = &k->session };
    k->driver  = (driver){ .config = cfg, .events = events, .ctl = &k->ctl, .calls = &k->calls,
                           .inbox = &k->inbox, .procs = &k->procs, .session = &k->session,
                           .provider_model = xstrdup(cfg->provider_model),
                           .system_path = xstrdup(cfg->system_path) };
    buf_set(&k->driver.param_members, buf_span(&cfg->param_members));
    k->ctl     = (ctl){ .config = cfg, .inbox = &k->inbox, .driver = &k->driver, .calls = &k->calls,
                        .session = &k->session };
    k->loop    = (loop){ .config = cfg, .driver = &k->driver, .procs = &k->procs, .calls = &k->calls,
                         .ctl = &k->ctl };
}

static void queue_argv_prompt(inbox *in, int nwords, char **words)
{
    if (!nwords) return;
    buf text = { 0 };
    json_buf content = { 0 };
    for (int i = 0; i < nwords; i++) buf_printf(&text, "%s%s", i ? " " : "", words[i]);
    json_put_string(&content, buf_span(&text));
    inbox_add(in, FROM_PROMPT, WHEN_IDLE, buf_span(&content));
    buf_free(&text);
    buf_free(&content);
}

int main(int argc, char **argv)
{
    struct kernel k = { 0 };
    int first = parse_args(&k.config, argc, argv);
    keep_std_fds_open();
    wire(&k);
    loop_open(&k.loop);
    ctl_open(&k.ctl);
    session_open(&k.session);
    export_env(&k.config);
    event(&k.ctl.out, "{'type':'hello','protocol':1,'session':%s,'head':%s,'model':%s}",
          session_path(&k.session), session_head_id(&k.session), k.driver.provider_model);
    queue_argv_prompt(&k.inbox, argc - first, argv + first);

    int exit_code = loop_run(&k.loop);
    driver_append_leftover_results(&k.driver);
    ctl_flush(&k.ctl);
    session_close(&k.session);
    return exit_code;
}
