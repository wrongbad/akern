/* main: akern's entry point and the only code that reads argv and the environment. Owns
   the options and environment defaults, among them a new session file in $TMPDIR without -s,
   the AKERN_HOME and recursion limit children inherit, and building the kernel: every module's
   state in one root, each module wired to the siblings it uses, and the actors in their fixed
   order; and the first lines: a new session's header, then hello. The config it fills is
   read-only once the kernel runs; the AKERN_MODEL children inherit is the model's. */

#include "akern.h"

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
    bus bus;
    processes processes;
    loop loop;
    input input;
    tools tools;
    model model;
    inbox inbox;
    actor actors[5];
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
    setenv(
        "AKERN_SESSION",
        cfg->session_path,
        1
    ); /* a sub-agent's goes in $AKERN_SESSION.d/agents/ */
}

/* The actors in the order they see each line: the loop (exit), input (status), calls (what the
   reply asks for), the model (whose turn it is), then the inbox, which decides what happens next.
   Each knows the others only by the lines they put on the bus. */
static void wire(struct kernel *k)
{
    const config *cfg = &k->config;
    bus *b = &k->bus;
    processes *ps = &k->processes;
    k->actors[0] = (actor){&k->loop, loop_on_line, NULL};
    k->actors[1] = (actor){&k->input, input_on_line, NULL};
    k->actors[2] = (actor){&k->tools, tools_on_line, tools_put_status};
    k->actors[3] = (actor){&k->model, model_on_line, model_put_status};
    k->actors[4] = (actor){&k->inbox, inbox_on_line, inbox_put_status};
    k->bus = (bus){.actors = k->actors, .nactors = LEN(k->actors), .config = cfg};
    k->processes = (processes){.config = cfg};
    k->loop = (loop){.bus = b, .input = &k->input, .processes = ps};
    k->input = (input){.bus = b, .actors = k->actors, .nactors = LEN(k->actors)};
    k->tools = (tools){.config = cfg, .bus = b, .processes = ps};
    k->model = (model){.config = cfg, .tools = &k->tools, .bus = b, .processes = ps};
    k->inbox = (inbox){.config = cfg, .bus = b, .settled = true};
}

/* A new session's first line. */
static void put_header(struct kernel *k)
{
    char id[NEW_ID_CHARS + 1], cwd[4096];
    bus_new_id(&k->bus, id);
    bus_printf(
        &k->bus,
        "{'type':'session','v':1,'id':%s,'cwd':%s,'created':%D}",
        id,
        getcwd(cwd, sizeof cwd) ? cwd : "",
        get_unix_seconds()
    );
}

/* The positional prompt, a send queued idle before stdin is read. */
static void queue_argv_prompt(bus *b, int nwords, char **words)
{
    if (!nwords)
        return;
    buf text = {0};
    for (int i = 0; i < nwords; i++)
        buf_printf(&text, "%s%s", i ? " " : "", words[i]);
    bus_tellf(
        b,
        "{'type':'command','command':{'type':'send','content':%S,'when':'idle'}}",
        buf_span(&text)
    );
    buf_free(&text);
}

int main(int argc, char **argv)
{
    struct kernel k = {0};
    int first = parse_args(&k.config, argc, argv);
    keep_std_fds_open();
    export_for_children(&k.config);
    wire(&k);
    tools_open(&k.tools);
    model_open(&k.model);
    loop_open(&k.loop);
    bus_open(&k.bus);
    bool fresh = bus_open_session(&k.bus);
    bus_replay(&k.bus, model_replay, &k.model);
    if (fresh)
        put_header(&k);
    bus_printf(
        &k.bus,
        "{'type':'hello','protocol':3,'session':%s,'model':%s,'params':{%r}}",
        k.config.session_path,
        k.config.provider_model,
        buf_cstr(&k.config.param_members)
    );
    queue_argv_prompt(&k.bus, argc - first, argv + first);

    int exit_code = loop_run(&k.loop);
    bus_tellf(&k.bus, "{'type':'close'}");
    bus_close(&k.bus);
    return exit_code;
}
