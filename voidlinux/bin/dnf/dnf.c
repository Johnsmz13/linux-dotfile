/*
 * dnf - a dnf-style front-end for Void Linux's xbps package manager.
 * Uses libxbps directly for package operations.
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "xbps_backend.h"

#define DNF_VERSION "0.3.0"
#define MAX_ARGS 64

typedef struct {
    int yes;
    int verbose;
    int quiet;
    int refresh;
    int downloadonly;
    int help;
    int version;
    const char *installroot;
} Options;

typedef enum {
    C_INSTALL, C_REINSTALL, C_REMOVE, C_AUTOREMOVE,
    C_UPGRADE, C_CHECK_UPDATE, C_MAKECACHE, C_CLEAN,
    C_SEARCH, C_INFO, C_LIST, C_PROVIDES, C_DEPS,
    C_HELP, C_VERSION, C_UNKNOWN
} Cmd;

typedef struct {
    const char *opt;
    const char *hint;
} OptHint;

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
        "  install PKG...                 Install packages\n"
        "  reinstall PKG...               Reinstall packages\n"
        "  remove PKG...                  Remove packages\n"
        "  autoremove                     Remove orphaned packages\n"
        "  upgrade | update [PKG...]      Upgrade all or specific packages\n"
        "  check-update [PKG...]          Check for updates, no changes\n"
        "  makecache                      Refresh the repository index\n"
        "  clean all | packages           Remove outdated cached packages\n"
        "  search STR                     Search available packages\n"
        "  info PKG...                    Show package information\n"
        "  list [installed|available|all] [PAT]  List packages\n"
        "  provides | whatprovides FILE   Find the package owning FILE\n"
        "  deps PKG                       Show dependencies of PKG\n"
        "  help                           Show this help\n"
        "  version                        Show version\n"
        "\n"
        "Options:\n"
        "  -y, --assumeyes                Assume yes to all prompts\n"
        "  -v, --verbose                  Verbose output\n"
        "  -q, --quiet                    Quiet output\n"
        "      --refresh                  Sync the repository index first\n"
        "      --downloadonly             Download only, do not install\n"
        "      --installroot DIR          Use DIR as the root directory\n"
        "  -h, --help                     Show this help\n"
        "  -V, --version                  Show version\n"
        "\n"
        "Write commands (install, reinstall, remove, autoremove, upgrade, clean)\n"
        "require root privileges; run them with sudo.\n"
        "\n",
        DNF_VERSION);
}

static int is_list_mode(const char *s)
{
    return strcmp(s, "installed") == 0 || strcmp(s, "available") == 0 ||
           strcmp(s, "all") == 0;
}

int main(int argc, char **argv)
{
    Options o = {0};
    const char *cmdname = NULL;
    const char *targets[MAX_ARGS];
    size_t ntargets = 0;
    Cmd cmd;
    int rv;

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

    if (is_write_cmd(cmd) && geteuid() != 0) {
        fprintf(stderr,
                "dnf: '%s' requires root privileges (it modifies the system).\n",
                cmdname);
        fprintf(stderr, "Run with sudo: sudo dnf %s", cmdname);
        for (size_t i = 0; i < ntargets; i++)
            fprintf(stderr, " %s", targets[i]);
        fputc('\n', stderr);
        return 1;
    }

    /* Commands that don't need libxbps init */
    switch (cmd) {
    case C_HELP:
        usage(stdout);
        return 0;
    case C_VERSION:
        printf("dnf %s (xbps front-end for Void Linux)\n", DNF_VERSION);
        return 0;
    default:
        break;
    }

    /* Initialize libxbps */
    rv = dnf_xbps_init(o.installroot, o.verbose);
    if (rv != 0) {
        fprintf(stderr, "dnf: failed to initialize xbps: %s\n", strerror(rv));
        return 1;
    }

    switch (cmd) {
    case C_INSTALL:
        if (ntargets == 0) {
            fprintf(stderr, "dnf: install requires at least one package name\n");
            rv = 2;
            break;
        }
        if (o.refresh) {
            rv = dnf_makecache();
            if (rv != 0) {
                fprintf(stderr, "dnf: failed to sync repositories\n");
                break;
            }
        }
        rv = dnf_install(targets, ntargets, 0, o.yes, o.downloadonly);
        break;

    case C_REINSTALL:
        if (ntargets == 0) {
            fprintf(stderr, "dnf: reinstall requires at least one package name\n");
            rv = 2;
            break;
        }
        rv = dnf_install(targets, ntargets, 1, o.yes, o.downloadonly);
        break;

    case C_REMOVE:
        if (ntargets == 0) {
            fprintf(stderr, "dnf: remove requires at least one package name\n");
            rv = 2;
            break;
        }
        rv = dnf_remove(targets, ntargets, o.yes);
        break;

    case C_AUTOREMOVE:
        rv = dnf_autoremove(o.yes);
        break;

    case C_UPGRADE:
        rv = dnf_upgrade(targets, ntargets, o.yes, o.downloadonly);
        break;

    case C_CHECK_UPDATE:
        rv = dnf_check_update(targets, ntargets);
        break;

    case C_MAKECACHE:
        rv = dnf_makecache();
        break;

    case C_CLEAN:
        if (ntargets == 0) {
            fprintf(stderr, "dnf: clean requires a target: all or packages\n");
            rv = 2;
            break;
        }
        if (ntargets > 1) {
            fprintf(stderr, "dnf: clean takes a single target\n");
            rv = 2;
            break;
        }
        rv = dnf_clean(targets[0]);
        break;

    case C_SEARCH:
        if (ntargets != 1) {
            fprintf(stderr, "dnf: search requires exactly one pattern\n");
            rv = 2;
            break;
        }
        rv = dnf_search(targets[0]);
        break;

    case C_INFO:
        if (ntargets == 0) {
            fprintf(stderr, "dnf: info requires at least one package name\n");
            rv = 2;
            break;
        }
        rv = dnf_info(targets, ntargets);
        break;

    case C_LIST: {
        const char *mode = "installed";
        const char *pattern = NULL;
        size_t start = 0;

        if (ntargets >= 1 && is_list_mode(targets[0])) {
            mode = targets[0];
            start = 1;
        }
        if (start < ntargets) {
            pattern = targets[start];
            start++;
        }
        if (start < ntargets) {
            fprintf(stderr, "dnf: list takes at most one pattern\n");
            rv = 2;
            break;
        }
        rv = dnf_list(mode, pattern);
        break;
    }

    case C_PROVIDES:
        if (ntargets != 1) {
            fprintf(stderr, "dnf: provides requires exactly one file or pattern\n");
            rv = 2;
            break;
        }
        rv = dnf_provides(targets[0]);
        break;

    case C_DEPS:
        if (ntargets != 1) {
            fprintf(stderr, "dnf: deps requires exactly one package name\n");
            rv = 2;
            break;
        }
        rv = dnf_deps(targets[0]);
        break;

    default:
        fprintf(stderr, "dnf: internal error: unhandled command\n");
        rv = 2;
        break;
    }

    dnf_xbps_end();
    return rv;
}
