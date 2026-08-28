/*
 * dnf - a dnf-style front-end for Void Linux's xbps package manager.
 * Translates dnf-style commands into xbps-install/-query/-remove invocations
 * and runs them as child processes via posix_spawn, propagating the exit status.
 */
#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define DNF_VERSION "0.1.0"
#define MAX_ARGS 64

extern char **environ;

typedef struct {
    int yes;
    int verbose;
    int quiet;
    int refresh;
    int downloadonly;
    int print;
    int help;
    int version;
    const char *installroot;
} Options;

typedef struct {
    char *argv[MAX_ARGS];
    size_t argc;
} ArgVec;

typedef enum {
    C_INSTALL, C_REINSTALL, C_REMOVE, C_AUTOREMOVE,
    C_UPGRADE, C_CHECK_UPDATE, C_MAKECACHE, C_CLEAN,
    C_SEARCH, C_INFO, C_LIST, C_PROVIDES, C_DEPS,
    C_HELP, C_VERSION, C_UNKNOWN
} Cmd;

static void av_push(ArgVec *av, const char *s)
{
    if (av->argc >= MAX_ARGS - 1) {
        fprintf(stderr, "dnf: too many arguments\n");
        exit(2);
    }
    av->argv[av->argc++] = (char *)s;
}

/* Print a shell-single-quoted word to f (safe for copy/paste and tests). */
static void fprint_quoted(FILE *f, const char *s)
{
    int need = 0;
    const char *p;

    for (p = s; *p; p++) {
        if (strchr(" \t\n'\"\\$`&;|<>()*?[]#~", *p)) {
            need = 1;
            break;
        }
    }
    if (need || s[0] == '\0') {
        fputc('\'', f);
        for (p = s; *p; p++) {
            if (*p == '\'')
                fputs("'\\''", f);
            else
                fputc(*p, f);
        }
        fputc('\'', f);
    } else {
        fputs(s, f);
    }
}

static void av_print(const ArgVec *av)
{
    size_t i;

    for (i = 0; i < av->argc; i++) {
        fprint_quoted(stdout, av->argv[i]);
        if (i + 1 < av->argc)
            putchar(' ');
    }
    putchar('\n');
}

/* Case-insensitive substring match. */
static int contains_ci(const char *hay, const char *needle)
{
    size_t nh = strlen(hay), nn = strlen(needle), i, j;

    if (nn == 0)
        return 1;
    if (nn > nh)
        return 0;
    for (i = 0; i + nn <= nh; i++) {
        for (j = 0; j < nn; j++) {
            if (tolower((unsigned char)hay[i + j]) !=
                tolower((unsigned char)needle[j]))
                break;
        }
        if (j == nn)
            return 1;
    }
    return 0;
}
/* Spawn argv[0] with inherited stdio and propagate its exit status. */
static int spawn_and_wait(const ArgVec *av)
{
    pid_t pid;
    int rc, status;

    rc = posix_spawnp(&pid, av->argv[0], NULL, NULL, av->argv, environ);
    if (rc != 0) {
        fprintf(stderr, "dnf: failed to start %s: %s\n", av->argv[0],
                strerror(rc));
        return 1;
    }
    if (waitpid(pid, &status, 0) < 0) {
        perror("dnf: waitpid");
        return 1;
    }
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) {
        if (WTERMSIG(status) != SIGINT && WTERMSIG(status) != SIGQUIT)
            fprintf(stderr, "dnf: %s terminated by signal %d\n", av->argv[0],
                    WTERMSIG(status));
        return 128 + WTERMSIG(status);
    }
    return 1;
}

/* Run av, capture stdout, and print only lines matching pattern (if any). */
static int run_and_filter(const ArgVec *av, const char *pattern)
{
    int pipefd[2], rc, status;
    pid_t pid;
    posix_spawn_file_actions_t fa;

    if (pipe(pipefd) != 0) {
        perror("dnf: pipe");
        return 1;
    }
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addclose(&fa, pipefd[0]);
    posix_spawn_file_actions_adddup2(&fa, pipefd[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&fa, pipefd[1]);
    rc = posix_spawnp(&pid, av->argv[0], &fa, NULL, av->argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    if (rc != 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        fprintf(stderr, "dnf: failed to start %s: %s\n", av->argv[0],
                strerror(rc));
        return 1;
    }
    close(pipefd[1]);
    FILE *f = fdopen(pipefd[0], "r");
    if (f) {
        char *line = NULL;
        size_t cap = 0;
        ssize_t n;

        while ((n = getline(&line, &cap, f)) != -1) {
            if (!pattern || contains_ci(line, pattern))
                fputs(line, stdout);
        }
        free(line);
        fclose(f);
    } else {
        close(pipefd[0]);
    }
    if (waitpid(pid, &status, 0) < 0) {
        perror("dnf: waitpid");
        return 1;
    }
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);
    return 1;
}

typedef struct {
    const char *opt;
    const char *hint;
} OptHint;

/* Common dnf options we deliberately do not map; give a useful hint. */
static const OptHint unmapped_options[] = {
    { "--best", "xbps always resolves the best version" },
    { "--nobest", "not supported by xbps" },
    { "--allowerasing", "xbps resolves conflicts itself; use xbps-install -I if needed" },
    { "--nodocs", "xbps has no per-package doc filtering" },
    { "--setopt", "not supported" },
    { "--releasever", "xbps has no releasever concept" },
    { "--cacheonly", "use xbps-install -M (memory-sync) instead" },
    { "--disablerepo", "not supported; edit /etc/xbps.d or use xbps-install -i" },
    { "--enablerepo", "add a repo with xbps-install -R/--repository URL" },
    { "--assumeno", "not supported; xbps only has --yes" },
    { "--downloaddir", "not supported; xbps uses its own cachedir" },
    { "--destdir", "not supported" },
    { "--skip-broken", "xbps resolves the whole transaction; not supported" },
    { "--security", "not supported" },
    { "--bugfix", "not supported" },
    { "--nogpgcheck", "xbps always verifies signatures" },
    { "--exclude", "not supported" },
    { "--obsoletes", "not supported" },
    { "--changelog", "not supported" },
    { "--comment", "not supported" },
    { "--oldpackage", "not supported" },
    { "--rpm", "not supported" },
    { "--repo", "not supported" },
};

static const OptHint *find_unmapped(const char *arg)
{
    size_t i;

    for (i = 0; i < sizeof(unmapped_options) / sizeof(unmapped_options[0]); i++) {
        size_t n = strlen(unmapped_options[i].opt);
        if (strncmp(arg, unmapped_options[i].opt, n) == 0 &&
            (arg[n] == '\0' || arg[n] == '='))
            return &unmapped_options[i];
    }
    return NULL;
}

/* Match "--name" or "--name=value"; value is set accordingly. */
static int long_matches(const char *arg, const char *name, const char **value)
{
    size_t n = strlen(name);

    if (strncmp(arg, name, n) != 0)
        return 0;
    if (arg[n] == '\0') {
        *value = NULL;
        return 1;
    }
    if (arg[n] == '=') {
        *value = arg + n + 1;
        return 1;
    }
    return 0;
}
static int parse_args(int argc, char **argv, Options *o, const char **cmdname,
                      const char **targets, size_t *ntargets)
{
    int after_ddash = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = NULL;

        if (!after_ddash && strcmp(a, "--") == 0) {
            after_ddash = 1;
            continue;
        }
        if (!after_ddash && a[0] == '-' && a[1] != '\0') {
            if (a[1] == '-') {
                if (strcmp(a, "--help") == 0)
                    o->help = 1;
                else if (strcmp(a, "--version") == 0)
                    o->version = 1;
                else if (strcmp(a, "--assumeyes") == 0 || strcmp(a, "--yes") == 0)
                    o->yes = 1;
                else if (strcmp(a, "--quiet") == 0)
                    o->quiet = 1;
                else if (strcmp(a, "--verbose") == 0)
                    o->verbose = 1;
                else if (strcmp(a, "--refresh") == 0)
                    o->refresh = 1;
                else if (strcmp(a, "--downloadonly") == 0)
                    o->downloadonly = 1;
                else if (strcmp(a, "--print") == 0)
                    o->print = 1;
                else if (long_matches(a, "--installroot", &v)) {
                    if (v) {
                        o->installroot = v;
                    } else {
                        if (i + 1 >= argc) {
                            fprintf(stderr,
                                    "dnf: option '--installroot' requires an argument\n");
                            return -1;
                        }
                        o->installroot = argv[++i];
                    }
                } else {
                    const OptHint *h = find_unmapped(a);
                    if (h) {
                        fprintf(stderr,
                                "dnf: option '%s' is not supported by this dnf-to-xbps wrapper%s%s\n",
                                a, h->hint ? ": " : "", h->hint ? h->hint : "");
                    } else {
                        fprintf(stderr, "dnf: unknown option '%s'\n", a);
                    }
                    return -1;
                }
            } else {
                for (size_t j = 1; a[j]; j++) {
                    switch (a[j]) {
                    case 'y': o->yes = 1; break;
                    case 'q': o->quiet = 1; break;
                    case 'v': o->verbose = 1; break;
                    case 'h': o->help = 1; break;
                    case 'V': o->version = 1; break;
                    default:
                        fprintf(stderr, "dnf: unknown option '-%c'\n", a[j]);
                        return -1;
                    }
                }
            }
        } else {
            if (*cmdname == NULL) {
                *cmdname = a;
            } else {
                if (*ntargets >= MAX_ARGS - 1) {
                    fprintf(stderr, "dnf: too many arguments\n");
                    return -1;
                }
                targets[(*ntargets)++] = a;
            }
        }
    }
    return 0;
}

static Cmd lookup_cmd(const char *name)
{
    if (strcmp(name, "install") == 0) return C_INSTALL;
    if (strcmp(name, "reinstall") == 0) return C_REINSTALL;
    if (strcmp(name, "remove") == 0) return C_REMOVE;
    if (strcmp(name, "autoremove") == 0) return C_AUTOREMOVE;
    if (strcmp(name, "upgrade") == 0 || strcmp(name, "update") == 0)
        return C_UPGRADE;
    if (strcmp(name, "check-update") == 0) return C_CHECK_UPDATE;
    if (strcmp(name, "makecache") == 0) return C_MAKECACHE;
    if (strcmp(name, "clean") == 0) return C_CLEAN;
    if (strcmp(name, "search") == 0) return C_SEARCH;
    if (strcmp(name, "info") == 0) return C_INFO;
    if (strcmp(name, "list") == 0) return C_LIST;
    if (strcmp(name, "provides") == 0 || strcmp(name, "whatprovides") == 0)
        return C_PROVIDES;
    if (strcmp(name, "deps") == 0) return C_DEPS;
    if (strcmp(name, "help") == 0) return C_HELP;
    if (strcmp(name, "version") == 0) return C_VERSION;
    return C_UNKNOWN;
}

static int is_write_cmd(Cmd c)
{
    return c == C_INSTALL || c == C_REINSTALL || c == C_REMOVE ||
           c == C_AUTOREMOVE || c == C_UPGRADE || c == C_CLEAN;
}

static void unsupported_hint(const char *name)
{
    if (strncmp(name, "group", 5) == 0)
        fprintf(stderr,
                "dnf: '%s' is not supported: xbps has no package groups; "
                "install individual packages or meta-packages instead.\n", name);
    else if (strcmp(name, "history") == 0)
        fprintf(stderr,
                "dnf: 'history' is not supported: xbps keeps no dnf-style "
                "transaction history; see /var/log/xbps.\n");
    else if (strcmp(name, "distro-sync") == 0)
        fprintf(stderr,
                "dnf: 'distro-sync' is not supported: upgrade everything with "
                "'dnf upgrade' (xbps-install -Su).\n");
    else if (strcmp(name, "downgrade") == 0)
        fprintf(stderr,
                "dnf: 'downgrade' is not supported: download an older .xbps "
                "and run 'sudo xbps-install -f /path/pkg.xbps'.\n");
    else if (strcmp(name, "swap") == 0)
        fprintf(stderr,
                "dnf: 'swap' is not supported: remove the old package and "
                "install the new one.\n");
    else if (strcmp(name, "repoquery") == 0)
        fprintf(stderr,
                "dnf: 'repoquery' is not supported: use 'dnf info PKG' or "
                "'dnf deps PKG' instead.\n");
    else if (strcmp(name, "deplist") == 0)
        fprintf(stderr,
                "dnf: 'deplist' is not supported: use 'dnf deps PKG' instead.\n");
    else
        fprintf(stderr, "dnf: unknown command '%s'\n", name);
}

static void usage(FILE *f)
{
    fprintf(f,
        "dnf %s - a dnf-style front-end for Void Linux's xbps package manager.\n"
        "\n"
        "Usage: dnf [options] COMMAND [ARGS...]\n"
        "\n"
        "Commands:\n"
        "  install PKG...                 Install packages (xbps-install)\n"
        "  reinstall PKG...               Reinstall packages (xbps-install -f)\n"
        "  remove PKG...                  Remove packages (xbps-remove)\n"
        "  autoremove                     Remove orphaned packages (xbps-remove -o)\n"
        "  upgrade | update [PKG...]      Upgrade all or specific packages (xbps-install -Su)\n"
        "  check-update [PKG...]          Check for updates, no changes (xbps-install -Sun)\n"
        "  makecache                      Refresh the repository index (xbps-install -S)\n"
        "  clean all | packages           Remove outdated cached packages (xbps-remove -O)\n"
        "  search STR                     Search available packages (xbps-query -Rs)\n"
        "  info PKG...                    Show package information (xbps-query -RS)\n"
        "  list [installed|available|all] [PAT]  List packages, optionally filtered\n"
        "  provides | whatprovides FILE   Find the package owning FILE (xbps-query -Ro)\n"
        "  deps PKG                       Show dependencies of PKG (xbps-query -Rx)\n"
        "  help                           Show this help\n"
        "  version                        Show version\n"
        "\n"
        "Options:\n"
        "  -y, --assumeyes                Assume yes to all prompts\n"
        "  -v, --verbose                  Verbose output\n"
        "  -q, --quiet                    Quiet output\n"
        "      --refresh                  Sync the repository index first (install)\n"
        "      --downloadonly             Download only, do not install\n"
        "      --installroot DIR          Use DIR as the root directory\n"
        "      --print                    Print the translated xbps command and exit\n"
        "  -h, --help                     Show this help\n"
        "  -V, --version                  Show version\n"
        "\n"
        "Write commands (install, reinstall, remove, autoremove, upgrade, clean)\n"
        "require root privileges; run them with sudo. Read commands run as-is.\n"
        "\n",
        DNF_VERSION);
}
static int is_list_mode(const char *s)
{
    return strcmp(s, "installed") == 0 || strcmp(s, "available") == 0 ||
           strcmp(s, "all") == 0;
}

static void build_list_base(ArgVec *av, const char *mode, const Options *o)
{
    av_push(av, "xbps-query");
    if (strcmp(mode, "available") == 0) {
        av_push(av, "-R");
        av_push(av, "-s");
        av_push(av, "");
    } else {
        av_push(av, "-l");
    }
    if (o->verbose)
        av_push(av, "-v");
    if (o->installroot) {
        av_push(av, "-r");
        av_push(av, o->installroot);
    }
}

static int run_one_list(const ArgVec *base, const char *pattern, int print)
{
    if (print) {
        av_print(base);
        if (pattern)
            printf("# (output filtered by pattern: %s)\n", pattern);
        return 0;
    }
    if (pattern)
        return run_and_filter(base, pattern);
    return spawn_and_wait(base);
}

static int run_list(const Options *o, const char **targets, size_t nt)
{
    const char *mode = "installed";
    const char *pattern = NULL;
    size_t start = 0;

    if (nt >= 1 && is_list_mode(targets[0])) {
        mode = targets[0];
        start = 1;
    }
    if (start < nt) {
        pattern = targets[start];
        start++;
    }
    if (start < nt) {
        fprintf(stderr, "dnf: list takes at most one pattern\n");
        return 2;
    }

    if (strcmp(mode, "all") == 0) {
        ArgVec a = {0}, b = {0};
        int r1, r2;

        build_list_base(&a, "installed", o);
        build_list_base(&b, "available", o);
        if (o->print) {
            av_print(&a);
            av_print(&b);
            if (pattern)
                printf("# (output filtered by pattern: %s)\n", pattern);
            return 0;
        }
        r1 = run_one_list(&a, pattern, 0);
        r2 = run_one_list(&b, pattern, 0);
        return r1 != 0 ? r1 : r2;
    }

    ArgVec base = {0};
    build_list_base(&base, mode, o);
    return run_one_list(&base, pattern, o->print);
}

int main(int argc, char **argv)
{
    Options o = {0};
    const char *cmdname = NULL;
    const char *targets[MAX_ARGS];
    size_t ntargets = 0;
    Cmd cmd;
    ArgVec av = {0};

    if (parse_args(argc, argv, &o, &cmdname, targets, &ntargets) != 0)
        return 2;

    cmd = cmdname ? lookup_cmd(cmdname) : C_UNKNOWN;

    if (o.help || cmd == C_HELP) {
        usage(stdout);
        return 0;
    }
    if (o.version || cmd == C_VERSION) {
        printf("dnf %s (xbps front-end for Void Linux)\n", DNF_VERSION);
        return 0;
    }
    if (cmdname == NULL) {
        usage(stderr);
        return 2;
    }
    if (cmd == C_UNKNOWN) {
        unsupported_hint(cmdname);
        return 2;
    }

    if (cmd == C_LIST)
        return run_list(&o, targets, ntargets);

    switch (cmd) {
    case C_INSTALL:
        av_push(&av, "xbps-install");
        if (o.refresh) av_push(&av, "-S");
        if (o.downloadonly) av_push(&av, "-D");
        if (o.yes) av_push(&av, "-y");
        if (o.verbose) av_push(&av, "-v");
        if (o.installroot) { av_push(&av, "-r"); av_push(&av, o.installroot); }
        for (size_t i = 0; i < ntargets; i++) av_push(&av, targets[i]);
        break;
    case C_REINSTALL:
        av_push(&av, "xbps-install");
        av_push(&av, "-f");
        if (o.downloadonly) av_push(&av, "-D");
        if (o.yes) av_push(&av, "-y");
        if (o.verbose) av_push(&av, "-v");
        if (o.installroot) { av_push(&av, "-r"); av_push(&av, o.installroot); }
        for (size_t i = 0; i < ntargets; i++) av_push(&av, targets[i]);
        break;
    case C_REMOVE:
        av_push(&av, "xbps-remove");
        if (o.yes) av_push(&av, "-y");
        if (o.verbose) av_push(&av, "-v");
        if (o.installroot) { av_push(&av, "-r"); av_push(&av, o.installroot); }
        for (size_t i = 0; i < ntargets; i++) av_push(&av, targets[i]);
        break;
    case C_AUTOREMOVE:
        av_push(&av, "xbps-remove");
        av_push(&av, "-o");
        if (o.yes) av_push(&av, "-y");
        if (o.verbose) av_push(&av, "-v");
        if (o.installroot) { av_push(&av, "-r"); av_push(&av, o.installroot); }
        break;
    case C_UPGRADE:
        av_push(&av, "xbps-install");
        av_push(&av, "-Su");
        if (o.downloadonly) av_push(&av, "-D");
        if (o.yes) av_push(&av, "-y");
        if (o.verbose) av_push(&av, "-v");
        if (o.installroot) { av_push(&av, "-r"); av_push(&av, o.installroot); }
        for (size_t i = 0; i < ntargets; i++) av_push(&av, targets[i]);
        break;
    case C_CHECK_UPDATE:
        av_push(&av, "xbps-install");
        av_push(&av, "-Sun");
        if (o.verbose) av_push(&av, "-v");
        if (o.installroot) { av_push(&av, "-r"); av_push(&av, o.installroot); }
        for (size_t i = 0; i < ntargets; i++) av_push(&av, targets[i]);
        break;
    case C_MAKECACHE:
        av_push(&av, "xbps-install");
        av_push(&av, "-S");
        if (o.verbose) av_push(&av, "-v");
        break;
    case C_CLEAN: {
        const char *t;
        if (ntargets == 0) {
            fprintf(stderr, "dnf: clean requires a target: all or packages\n");
            return 2;
        }
        if (ntargets > 1) {
            fprintf(stderr, "dnf: clean takes a single target\n");
            return 2;
        }
        t = targets[0];
        if (strcmp(t, "all") != 0 && strcmp(t, "packages") != 0) {
            fprintf(stderr,
                    "dnf: clean '%s' is not supported; supported: all, packages (xbps-remove -O)\n",
                    t);
            return 2;
        }
        av_push(&av, "xbps-remove");
        av_push(&av, "-O");
        if (o.verbose) av_push(&av, "-v");
        if (o.installroot) { av_push(&av, "-r"); av_push(&av, o.installroot); }
        break;
    }
    case C_SEARCH:
        if (ntargets != 1) {
            fprintf(stderr, "dnf: search requires exactly one pattern\n");
            return 2;
        }
        av_push(&av, "xbps-query");
        av_push(&av, "-Rs");
        if (o.verbose) av_push(&av, "-v");
        if (o.installroot) { av_push(&av, "-r"); av_push(&av, o.installroot); }
        av_push(&av, targets[0]);
        break;
    case C_INFO:
        if (ntargets == 0) {
            fprintf(stderr, "dnf: info requires at least one package name\n");
            return 2;
        }
        av_push(&av, "xbps-query");
        av_push(&av, "-RS");
        if (o.verbose) av_push(&av, "-v");
        if (o.installroot) { av_push(&av, "-r"); av_push(&av, o.installroot); }
        for (size_t i = 0; i < ntargets; i++) av_push(&av, targets[i]);
        break;
    case C_PROVIDES:
        if (ntargets != 1) {
            fprintf(stderr, "dnf: provides requires exactly one file or pattern\n");
            return 2;
        }
        av_push(&av, "xbps-query");
        av_push(&av, "-Ro");
        if (o.verbose) av_push(&av, "-v");
        if (o.installroot) { av_push(&av, "-r"); av_push(&av, o.installroot); }
        av_push(&av, targets[0]);
        break;
    case C_DEPS:
        if (ntargets != 1) {
            fprintf(stderr, "dnf: deps requires exactly one package name\n");
            return 2;
        }
        av_push(&av, "xbps-query");
        av_push(&av, "-Rx");
        if (o.verbose) av_push(&av, "-v");
        if (o.installroot) { av_push(&av, "-r"); av_push(&av, o.installroot); }
        av_push(&av, targets[0]);
        break;
    default:
        fprintf(stderr, "dnf: internal error: unhandled command\n");
        return 2;
    }

    if (o.print) {
        av_print(&av);
        return 0;
    }

    if (is_write_cmd(cmd) && geteuid() != 0) {
        fprintf(stderr,
                "dnf: '%s' requires root privileges (it modifies the system).\n",
                cmdname);
        fprintf(stderr, "Run with sudo: sudo ");
        for (int i = 0; i < argc; i++) {
            fprint_quoted(stderr, argv[i]);
            if (i + 1 < argc)
                fputc(' ', stderr);
        }
        fputc('\n', stderr);
        return 1;
    }

    return spawn_and_wait(&av);
}
