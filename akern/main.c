#include "main.h"

#include <errno.h>
#include <fcntl.h>
#include <float.h>
#include <getopt.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "calls.h"
#include "inbox.h"
#include "input.h"
#include "loop.h"
#include "output.h"
#include "proc.h"
#include "session.h"
#include "tools.h"
#include "turns.h"

enum { DEFAULT_RECURSION_LIMIT = 4 };

enum { MAX_PROCESSES = 256, KILL_GRACE };

static const struct option long_options[] = {
    {"print", no_argument, NULL, 'p'},
    {"session", required_argument, NULL, 's'},
    {"model", required_argument, NULL, 'm'},
    {"system", required_argument, NULL, 'S'},
    {"param", required_argument, NULL, 'P'},
    {"max-processes", required_argument, NULL, MAX_PROCESSES},
    {"kill-grace", required_argument, NULL, KILL_GRACE},
    {NULL, 0, NULL, 0},
};

struct kernel {
    config config;
    output output;
    session session;
    procs procs;
    inbox inbox;
    tools tools;
    calls calls;
    turns turns;
    input input;
    loop loop;
};

static void usage(void)
{
    fputs(
        "usage: akern [-p] [-s session.jsonl] [-m provider/model] [-S system.md] [-P "
        "key=value]...\n"
        "             [--max-processes N] [--kill-grace SECONDS] [prompt...]\n"
        "  -p, --print              print each run's final answer instead of the event stream\n"
        "  -s, --session PATH       session file (default: a new one in $TMPDIR)\n"
        "  -m, --model P/M          provider/model (default: $AKERN_MODEL)\n"
        "  -S, --system PATH        system prompt file (default: the output of "
        "$AKERN_HOME/system-prompt, else $AKERN_HOME/system/system-prompt)\n"
        "  -P, --param KEY=VALUE    provider parameter; VALUE is sent as JSON if it is JSON, else "
        "as a string\n"
        "      --max-processes N    tool programs running at once (default 32)\n"
        "      --kill-grace SECONDS from SIGTERM to SIGKILL (default 2)\n",
        stderr
    );
    exit(2);
}

static void bad_value(const char *flag)
{
    fprintf(stderr, "akern: invalid --%s: %s\n", flag, optarg);
    usage();
}

static double flag_number(const char *flag, double min, double max)
{
    char *end;
    double n = strtod(optarg, &end);
    if (end == optarg || *end || !(n >= min && n <= max))
        bad_value(flag);
    return n;
}

static double flag_count(const char *flag, double max)
{
    double n = flag_number(flag, 1, max);
    if (n != floor(n))
        bad_value(flag);
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
    if (!eq || eq == key_value)
        bad_value("param");
    span key = {key_value, (size_t)(eq - key_value)}, value = S(eq + 1);
    if (cfg->param_members.n)
        buf_putc(&cfg->param_members, ',');
    if (json_is_valid(value))
        json_printf(&cfg->param_members, "%S:%R", key, json_trim(value));
    else
        json_printf(&cfg->param_members, "%S:%S", key, value);
}

static char *home_dir(void)
{
    const char *set = getenv("AKERN_HOME");
    if (set && *set)
        return xstrdup(set);
    buf path = {0};
    buf_printf(&path, "%s/.akern", getenv("HOME") ? getenv("HOME") : ".");
    return path.p;
}

static char *new_session_path(void)
{
    const char *tmp_dir = getenv("TMPDIR") && *getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp";
    buf path = {0};
    buf_printf(&path, "%s/akern-session-XXXXXX", tmp_dir);
    int fd = mkstemp(buf_cstr(&path));
    if (fd < 0)
        die("mkstemp: %s", strerror(errno));
    close(fd);
    return path.p;
}

static int parse_args(config *cfg, int argc, char **argv)
{
    *cfg = (config){.max_processes = 32, .kill_grace_seconds = 2};
    for (int ch; (ch = getopt_long(argc, argv, "+ps:m:S:P:", long_options, NULL)) != -1;)
    {
        switch (ch)
        {
        case 'p':
            cfg->print_answers = true;
            break;
        case 's':
            cfg->session_path = xstrdup(optarg);
            break;
        case 'm':
            cfg->provider_model = xstrdup(optarg);
            break;
        case 'S':
            cfg->system_path = xstrdup(optarg);
            break;
        case 'P':
            add_param(cfg, optarg);
            break;
        case MAX_PROCESSES:
            cfg->max_processes = (int)flag_count("max-processes", INT_MAX);
            break;
        case KILL_GRACE:
            cfg->kill_grace_seconds = flag_number("kill-grace", 0, DBL_MAX);
            break;
        default:
            usage();
        }
    }
    if (!cfg->provider_model && getenv("AKERN_MODEL"))
        cfg->provider_model = xstrdup(getenv("AKERN_MODEL"));
    cfg->recursion_limit = env_int("AKERN_RE_LIMIT", DEFAULT_RECURSION_LIMIT);
    if (cfg->recursion_limit <= 0)
        die("recursion limit reached: this kernel would be nested too deep "
            "(AKERN_RE_LIMIT)");
    cfg->home_dir = home_dir();
    if (!cfg->session_path)
        cfg->session_path = new_session_path();
    buf_cstr(&cfg->param_members);
    return optind;
}

static void keep_std_fds_open(void)
{
    for (int fd = 0; fd <= 2; fd++)
        if (fcntl(fd, F_GETFD) < 0 && open("/dev/null", O_RDWR) != fd)
            die("cannot open /dev/null");
}

static void export_for_children(const config *cfg)
{
    char below[16];
    snprintf(below, sizeof below, "%d", cfg->recursion_limit - 1);
    setenv("AKERN_RE_LIMIT", below, 1);
    setenv("AKERN_HOME", cfg->home_dir, 1);
}

static void wire(struct kernel *k)
{
    const config *cfg = &k->config;
    output *out = &k->output;
    k->session = (session){.config = cfg};
    k->procs = (procs){.config = cfg};
    k->inbox = (inbox){.output = out};
    k->calls = (calls){
        .tools = &k->tools,
        .config = cfg,
        .output = out,
        .procs = &k->procs,
        .inbox = &k->inbox
    };
    k->turns = (turns){
        .config = cfg,
        .output = out,
        .calls = &k->calls,
        .inbox = &k->inbox,
        .procs = &k->procs,
        .session = &k->session
    };
    k->input = (input){.config = cfg, .output = out, .inbox = &k->inbox, .turns = &k->turns};
    k->loop = (loop){
        .config = cfg,
        .output = out,
        .input = &k->input,
        .turns = &k->turns,
        .calls = &k->calls,
        .procs = &k->procs,
        .session = &k->session
    };
}

static void queue_argv_prompt(inbox *in, int nwords, char **words)
{
    if (!nwords)
        return;
    buf text = {0};
    json_buf content = {0};
    for (int i = 0; i < nwords; i++)
        buf_printf(&text, "%s%s", i ? " " : "", words[i]);
    json_put_string(&content, buf_span(&text));
    inbox_add(in, FROM_PROMPT, WHEN_IDLE, buf_span(&content));
    buf_free(&text);
    buf_free(&content);
}

int main(int argc, char **argv)
{
    struct kernel k = {0};
    int first = parse_args(&k.config, argc, argv);
    keep_std_fds_open();
    export_for_children(&k.config);
    wire(&k);
    tools_open(&k.tools, k.config.home_dir);
    turns_open(&k.turns);
    loop_open(&k.loop);
    output_open(&k.output, k.config.print_answers);
    session_open(&k.session);
    turns_apply_startup_settings(&k.turns);
    output_emit(
        &k.output,
        "{'type':'hello','protocol':1,'session':%s,'head':%s,'model':%s}",
        k.config.session_path,
        session_get_head_id(&k.session),
        k.turns.provider_model
    );
    queue_argv_prompt(&k.inbox, argc - first, argv + first);

    int exit_code = loop_run(&k.loop);
    turns_append_leftover_results(&k.turns);
    output_flush(&k.output);
    session_close(&k.session);
    return exit_code;
}
