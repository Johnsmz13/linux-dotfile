/*
 * xbps_backend.c - libxbps backend for dnf wrapper.
 * Implements package operations directly via libxbps API.
 */
#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xbps.h>

#include "xbps_backend.h"

static struct xbps_handle xh;
static int saved_stdin = -1;

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

/* Extract package name from "foo-1.0_1" into dst. */
static void pkg_name_from_pkgver(char *dst, size_t len, const char *pkgver)
{
    const char *p;
    size_t n;

    p = pkgver + strlen(pkgver);
    /* walk back past revision: _N */
    while (p > pkgver && *(p - 1) != '_')
        p--;
    if (p > pkgver)
        p--;
    /* walk back past version: digits, dots, etc. */
    while (p > pkgver && *(p - 1) != '-')
        p--;
    n = (size_t)(p - pkgver);
    if (n == 0)
        n = strlen(pkgver);
    if (n >= len)
        n = len - 1;
    memcpy(dst, pkgver, n);
    dst[n] = '\0';
}

/* Map errno to user-friendly message. */
static const char *strerror_xbps(int rv)
{
    switch (rv) {
    case EEXIST:  return "package already installed or up-to-date";
    case ENOENT:  return "package not found in repositories";
    case ENOSPC:  return "not enough disk space";
    case ENODEV:  return "missing dependencies";
    case EAGAIN:  return "package conflicts detected";
    case EPERM:   return "permission denied";
    case ENOTSUP: return "no repositories available";
    case ENXIO:   return "dependency resolution failed";
    case EBUSY:   return "xbps itself needs to be updated first";
    default:      return strerror(rv);
    }
}

/* State callback for transaction progress. */
static int state_cb(const struct xbps_state_cb_data *xscd, void *arg)
{
    (void)arg;

    switch (xscd->state) {
    case XBPS_STATE_INSTALL:
        printf("Installing %s\n", xscd->arg ? xscd->arg : "");
        break;
    case XBPS_STATE_INSTALL_DONE:
        printf("Installed  %s\n", xscd->arg ? xscd->arg : "");
        break;
    case XBPS_STATE_UPDATE:
        printf("Updating %s\n", xscd->arg ? xscd->arg : "");
        break;
    case XBPS_STATE_UPDATE_DONE:
        printf("Updated  %s\n", xscd->arg ? xscd->arg : "");
        break;
    case XBPS_STATE_REMOVE:
        printf("Removing %s\n", xscd->arg ? xscd->arg : "");
        break;
    case XBPS_STATE_REMOVE_DONE:
        printf("Removed  %s\n", xscd->arg ? xscd->arg : "");
        break;
    case XBPS_STATE_DOWNLOAD:
        printf("Downloading %s\n", xscd->arg ? xscd->arg : "");
        break;
    case XBPS_STATE_TRANS_DOWNLOAD:
        printf("Downloading packages...\n");
        break;
    case XBPS_STATE_TRANS_RUN:
        printf("Running transaction...\n");
        break;
    case XBPS_STATE_TRANS_CONFIGURE:
        printf("Configuring packages...\n");
        break;
    case XBPS_STATE_TRANS_FAIL:
        fprintf(stderr, "dnf: transaction failed: %s\n",
                xscd->desc ? xscd->desc : "unknown error");
        break;
    case XBPS_STATE_REPOSYNC:
        printf("Syncing repository %s\n", xscd->arg ? xscd->arg : "");
        break;
    default:
        break;
    }
    return 0;
}

/* Print transaction summary table. */
static void print_transaction_summary(void)
{
    xbps_object_iterator_t iter;
    xbps_object_t obj;
    xbps_array_t pkgs;

    if (!xh.transd)
        return;

    pkgs = xbps_dictionary_get(xh.transd, "packages");
    if (!pkgs || xbps_array_count(pkgs) == 0)
        return;

    printf("\n");
    printf("%-20s %-10s %-20s %-20s %s\n",
           "Name", "Action", "Version", "New version", "Download size");
    printf("%-20s %-10s %-20s %-20s %s\n",
           "----", "------", "-------", "-----------", "-------------");

    iter = xbps_array_iterator(pkgs);
    if (!iter)
        return;

    while ((obj = xbps_object_iterator_next(iter)) != NULL) {
        xbps_dictionary_t pkgd = (xbps_dictionary_t)obj;
        const char *pkgver = NULL, *repo = NULL;
        const char *action_str = "unknown";
        uint64_t dlsize = 0;
        uint8_t tp;
        char name[64] = {0};
        char version[64] = {0};

        xbps_dictionary_get_cstring_nocopy(pkgd, "pkgver", &pkgver);
        xbps_dictionary_get_cstring_nocopy(pkgd, "repository", &repo);
        xbps_dictionary_get_uint64(pkgd, "filename-size", &dlsize);
        xbps_dictionary_get_uint8(pkgd, "trans-action", &tp);

        switch (tp) {
        case XBPS_TRANS_INSTALL:  action_str = "install"; break;
        case XBPS_TRANS_REINSTALL: action_str = "reinstall"; break;
        case XBPS_TRANS_UPDATE:   action_str = "update"; break;
        case XBPS_TRANS_REMOVE:   action_str = "remove"; break;
        default: break;
        }

        if (pkgver) {
            pkg_name_from_pkgver(name, sizeof(name), pkgver);
            const char *v = pkgver + strlen(name);
            if (*v == '-')
                v++;
            snprintf(version, sizeof(version), "%s", v);
        }

        char dlbuf[32] = "-";
        if (dlsize > 0) {
            /* humanize: simple KB/MB conversion */
            if (dlsize >= 1048576)
                snprintf(dlbuf, sizeof(dlbuf), "%lluMB",
                         (unsigned long long)(dlsize / 1048576));
            else if (dlsize >= 1024)
                snprintf(dlbuf, sizeof(dlbuf), "%lluKB",
                         (unsigned long long)(dlsize / 1024));
            else
                snprintf(dlbuf, sizeof(dlbuf), "%lluB",
                         (unsigned long long)dlsize);
        }

        printf("%-20s %-10s %-20s %-20s %s\n",
               name, action_str, "-", version, dlbuf);
    }

    xbps_object_iterator_release(iter);
    printf("\n");
}

/* Prompt user for confirmation. Returns 1 if confirmed, 0 otherwise. */
static int prompt_confirm(int assume_yes)
{
    char buf[16];

    if (assume_yes)
        return 1;

    printf("Do you want to continue? [y/N] ");
    fflush(stdout);

    if (!fgets(buf, sizeof(buf), stdin))
        return 0; /* EOF = abort */

    /* Only 'y' or 'Y' confirms; anything else (including Enter) = abort */
    return (buf[0] == 'y' || buf[0] == 'Y');
}

/* Pipe "y\n" to stdin so xbps_transaction_commit() auto-accepts. */
static void pipe_yes_to_stdin(void)
{
    int pipefd[2];

    if (pipe(pipefd) != 0)
        return;
    write(pipefd[1], "y\n", 2);
    close(pipefd[1]);
    dup2(pipefd[0], STDIN_FILENO);
    close(pipefd[0]);
}

int dnf_xbps_init(const char *rootdir, int verbose)
{
    saved_stdin = dup(STDIN_FILENO);

    memset(&xh, 0, sizeof(xh));
    xh.state_cb = state_cb;
    if (verbose)
        xh.flags |= XBPS_FLAG_VERBOSE;
    if (rootdir)
        snprintf(xh.rootdir, sizeof(xh.rootdir), "%s", rootdir);

    return xbps_init(&xh);
}

void dnf_xbps_end(void)
{
    xbps_end(&xh);
    if (saved_stdin >= 0) {
        dup2(saved_stdin, STDIN_FILENO);
        close(saved_stdin);
        saved_stdin = -1;
    }
}

int dnf_makecache(void)
{
    return xbps_rpool_sync(&xh, NULL);
}

int dnf_install(const char **pkgs, size_t npkgs, int force, int assume_yes,
                int downloadonly)
{
    int rv, any_queued = 0;

    if (downloadonly)
        xh.flags |= XBPS_FLAG_DOWNLOAD_ONLY;

    rv = xbps_pkgdb_lock(&xh);
    if (rv != 0) {
        fprintf(stderr, "dnf: failed to lock pkgdb: %s\n", strerror_xbps(rv));
        return rv;
    }

    for (size_t i = 0; i < npkgs; i++) {
        if (!force && xbps_pkg_is_installed(&xh, pkgs[i]) == 1) {
            fprintf(stderr, "dnf: '%s' is already installed.\n", pkgs[i]);
            continue;
        }
        rv = xbps_transaction_install_pkg(&xh, pkgs[i], force);
        if (rv != 0) {
            if (rv == EEXIST) {
                fprintf(stderr, "dnf: '%s' is already installed.\n", pkgs[i]);
                continue;
            }
            fprintf(stderr, "dnf: failed to queue '%s': %s\n",
                    pkgs[i], strerror_xbps(rv));
            xbps_pkgdb_unlock(&xh);
            return rv;
        }
        any_queued = 1;
    }

    if (!any_queued) {
        xbps_pkgdb_unlock(&xh);
        return 0;
    }

    rv = xbps_transaction_prepare(&xh);
    if (rv != 0) {
        fprintf(stderr, "dnf: transaction failed: %s\n", strerror_xbps(rv));
        xbps_pkgdb_unlock(&xh);
        return rv;
    }

    print_transaction_summary();

    if (!prompt_confirm(assume_yes)) {
        fprintf(stderr, "Aborted.\n");
        xbps_pkgdb_unlock(&xh);
        return 1;
    }

    if (assume_yes)
        pipe_yes_to_stdin();

    rv = xbps_transaction_commit(&xh);
    if (rv != 0)
        fprintf(stderr, "dnf: install failed: %s\n", strerror_xbps(rv));

    xbps_pkgdb_unlock(&xh);
    return rv;
}

int dnf_remove(const char **pkgs, size_t npkgs, int assume_yes)
{
    int rv;

    rv = xbps_pkgdb_lock(&xh);
    if (rv != 0) {
        fprintf(stderr, "dnf: failed to lock pkgdb: %s\n", strerror_xbps(rv));
        return rv;
    }

    for (size_t i = 0; i < npkgs; i++) {
        if (xbps_pkg_is_installed(&xh, pkgs[i]) != 1) {
            fprintf(stderr, "dnf: '%s' is not installed.\n", pkgs[i]);
            continue;
        }
        rv = xbps_transaction_remove_pkg(&xh, pkgs[i], false);
        if (rv != 0) {
            fprintf(stderr, "dnf: failed to queue '%s': %s\n",
                    pkgs[i], strerror_xbps(rv));
            xbps_pkgdb_unlock(&xh);
            return rv;
        }
    }

    rv = xbps_transaction_prepare(&xh);
    if (rv != 0) {
        fprintf(stderr, "dnf: transaction failed: %s\n", strerror_xbps(rv));
        xbps_pkgdb_unlock(&xh);
        return rv;
    }

    print_transaction_summary();

    if (!prompt_confirm(assume_yes)) {
        fprintf(stderr, "Aborted.\n");
        xbps_pkgdb_unlock(&xh);
        return 1;
    }

    if (assume_yes)
        pipe_yes_to_stdin();

    rv = xbps_transaction_commit(&xh);
    if (rv != 0)
        fprintf(stderr, "dnf: remove failed: %s\n", strerror_xbps(rv));

    xbps_pkgdb_unlock(&xh);
    return rv;
}

int dnf_upgrade(const char **pkgs, size_t npkgs, int assume_yes,
                int downloadonly)
{
    int rv;

    if (downloadonly)
        xh.flags |= XBPS_FLAG_DOWNLOAD_ONLY;

    rv = xbps_pkgdb_lock(&xh);
    if (rv != 0) {
        fprintf(stderr, "dnf: failed to lock pkgdb: %s\n", strerror_xbps(rv));
        return rv;
    }

    if (npkgs == 0) {
        rv = xbps_transaction_update_packages(&xh);
    } else {
        for (size_t i = 0; i < npkgs; i++) {
            rv = xbps_transaction_update_pkg(&xh, pkgs[i], false);
            if (rv != 0 && rv != EEXIST) {
                fprintf(stderr, "dnf: failed to queue '%s': %s\n",
                        pkgs[i], strerror_xbps(rv));
                xbps_pkgdb_unlock(&xh);
                return rv;
            }
        }
        rv = 0;
    }

    if (rv != 0) {
        if (rv == EEXIST)
            fprintf(stderr, "dnf: all packages are up-to-date.\n");
        else
            fprintf(stderr, "dnf: failed to prepare upgrade: %s\n",
                    strerror_xbps(rv));
        xbps_pkgdb_unlock(&xh);
        return rv;
    }

    rv = xbps_transaction_prepare(&xh);
    if (rv != 0) {
        if (rv == EEXIST)
            fprintf(stderr, "dnf: all packages are up-to-date.\n");
        else
            fprintf(stderr, "dnf: transaction failed: %s\n",
                    strerror_xbps(rv));
        xbps_pkgdb_unlock(&xh);
        return rv;
    }

    print_transaction_summary();

    if (!prompt_confirm(assume_yes)) {
        fprintf(stderr, "Aborted.\n");
        xbps_pkgdb_unlock(&xh);
        return 1;
    }

    if (assume_yes)
        pipe_yes_to_stdin();

    rv = xbps_transaction_commit(&xh);
    if (rv != 0)
        fprintf(stderr, "dnf: upgrade failed: %s\n", strerror_xbps(rv));

    xbps_pkgdb_unlock(&xh);
    return rv;
}

struct search_arg {
    const char *pattern;
    int found;
};

static int search_repo_cb(struct xbps_repo *repo, void *arg, bool *done)
{
    struct search_arg *sa = arg;
    xbps_object_iterator_t iter;
    xbps_object_t obj;

    (void)done;

    if (!repo->idx)
        return 0;

    iter = xbps_dictionary_iterator(repo->idx);
    if (!iter)
        return 0;

    while ((obj = xbps_object_iterator_next(iter)) != NULL) {
        const char *key = xbps_dictionary_keysym_cstring_nocopy(obj);
        xbps_dictionary_t pkgd = xbps_dictionary_get_keysym(repo->idx, obj);
        const char *desc = NULL;

        xbps_dictionary_get_cstring_nocopy(pkgd, "short_desc", &desc);

        if (contains_ci(key, sa->pattern) ||
            (desc && contains_ci(desc, sa->pattern))) {
            printf("%-20s %s\n", key, desc ? desc : "");
            sa->found = 1;
        }
    }

    xbps_object_iterator_release(iter);
    return 0;
}

int dnf_search(const char *pattern)
{
    struct search_arg sa = { pattern, 0 };
    int rv;

    rv = xbps_rpool_foreach(&xh, search_repo_cb, &sa);
    if (rv != 0)
        return rv;
    if (!sa.found) {
        fprintf(stderr, "dnf: no matches found for '%s'\n", pattern);
        return 1;
    }
    return 0;
}

struct list_arg {
    const char *pattern;
};

static int list_installed_cb(struct xbps_handle *xhp, xbps_object_t obj,
                             const char *key, void *arg, bool *done)
{
    struct list_arg *la = arg;
    (void)xhp; (void)obj; (void)done;

    if (!la->pattern || contains_ci(key, la->pattern))
        printf("%s\n", key);
    return 0;
}

static int list_available_cb(struct xbps_repo *repo, void *arg, bool *done)
{
    struct list_arg *la = arg;
    xbps_object_iterator_t iter;
    xbps_object_t obj;

    (void)done;

    if (!repo->idx)
        return 0;

    iter = xbps_dictionary_iterator(repo->idx);
    if (!iter)
        return 0;

    while ((obj = xbps_object_iterator_next(iter)) != NULL) {
        const char *key = xbps_dictionary_keysym_cstring_nocopy(obj);

        if (!la->pattern || contains_ci(key, la->pattern))
            printf("%s\n", key);
    }

    xbps_object_iterator_release(iter);
    return 0;
}

int dnf_list(const char *mode, const char *pattern)
{
    struct list_arg la = { pattern };

    if (strcmp(mode, "installed") == 0)
        return xbps_pkgdb_foreach_cb(&xh, list_installed_cb, &la);

    if (strcmp(mode, "available") == 0)
        return xbps_rpool_foreach(&xh, list_available_cb, &la);

    if (strcmp(mode, "all") == 0) {
        int r1 = xbps_pkgdb_foreach_cb(&xh, list_installed_cb, &la);
        int r2 = xbps_rpool_foreach(&xh, list_available_cb, &la);
        return r1 != 0 ? r1 : r2;
    }

    fprintf(stderr, "dnf: unknown list mode '%s'\n", mode);
    return 2;
}

int dnf_info(const char **pkgs, size_t npkgs)
{
    int rv = 0;

    for (size_t i = 0; i < npkgs; i++) {
        xbps_dictionary_t pkgd;
        const char *pkgver = NULL, *desc = NULL, *homepage = NULL;
        const char *license = NULL, *arch = NULL, *maintainer = NULL;
        const char *filename = NULL;
        uint64_t installed_size = 0, filename_size = 0;
        int installed;

        /* Try repo first, then pkgdb */
        pkgd = xbps_rpool_get_pkg(&xh, pkgs[i]);
        if (pkgd) {
            xbps_dictionary_get_cstring_nocopy(pkgd, "pkgver", &pkgver);
            xbps_dictionary_get_cstring_nocopy(pkgd, "short_desc", &desc);
            xbps_dictionary_get_cstring_nocopy(pkgd, "homepage", &homepage);
            xbps_dictionary_get_cstring_nocopy(pkgd, "license", &license);
            xbps_dictionary_get_cstring_nocopy(pkgd, "architecture", &arch);
            xbps_dictionary_get_cstring_nocopy(pkgd, "maintainer", &maintainer);
            xbps_dictionary_get_cstring_nocopy(pkgd, "filename", &filename);
            xbps_dictionary_get_uint64(pkgd, "installed_size", &installed_size);
            xbps_dictionary_get_uint64(pkgd, "filename-size", &filename_size);
        } else {
            pkgd = xbps_pkgdb_get_pkg(&xh, pkgs[i]);
            if (!pkgd) {
                fprintf(stderr, "dnf: package '%s' not found\n", pkgs[i]);
                rv = 1;
                continue;
            }
            xbps_dictionary_get_cstring_nocopy(pkgd, "pkgver", &pkgver);
            xbps_dictionary_get_cstring_nocopy(pkgd, "short_desc", &desc);
            xbps_dictionary_get_cstring_nocopy(pkgd, "homepage", &homepage);
            xbps_dictionary_get_cstring_nocopy(pkgd, "license", &license);
            xbps_dictionary_get_cstring_nocopy(pkgd, "architecture", &arch);
            xbps_dictionary_get_cstring_nocopy(pkgd, "maintainer", &maintainer);
            xbps_dictionary_get_uint64(pkgd, "installed_size", &installed_size);
        }

        installed = xbps_pkg_is_installed(&xh, pkgs[i]);

        printf("Name         : %s\n", pkgver ? pkgver : pkgs[i]);
        printf("Description  : %s\n", desc ? desc : "");
        if (homepage && homepage[0])
            printf("Homepage     : %s\n", homepage);
        if (license && license[0])
            printf("License      : %s\n", license);
        if (arch && arch[0])
            printf("Architecture : %s\n", arch);
        if (maintainer && maintainer[0])
            printf("Maintainer   : %s\n", maintainer);
        printf("Installed    : %s\n", installed == 1 ? "yes" : "no");
        if (installed_size > 0) {
            char buf[32];
            if (xbps_humanize_number(buf, (int64_t)installed_size) == 0)
                printf("Size         : %s\n", buf);
        }
        if (filename && filename[0])
            printf("Filename     : %s\n", filename);
        printf("\n");

        if (pkgd && !xbps_rpool_get_pkg(&xh, pkgs[i]))
            xbps_object_release(pkgd);
    }

    return rv;
}

int dnf_autoremove(int assume_yes)
{
    int rv;

    rv = xbps_pkgdb_lock(&xh);
    if (rv != 0) {
        fprintf(stderr, "dnf: failed to lock pkgdb: %s\n", strerror_xbps(rv));
        return rv;
    }

    rv = xbps_transaction_autoremove_pkgs(&xh);
    if (rv != 0) {
        if (rv == ENOENT)
            fprintf(stderr, "dnf: no orphaned packages found.\n");
        else
            fprintf(stderr, "dnf: failed to prepare autoremove: %s\n",
                    strerror_xbps(rv));
        xbps_pkgdb_unlock(&xh);
        return rv;
    }

    rv = xbps_transaction_prepare(&xh);
    if (rv != 0) {
        if (rv == EEXIST)
            fprintf(stderr, "dnf: no orphaned packages found.\n");
        else
            fprintf(stderr, "dnf: transaction failed: %s\n",
                    strerror_xbps(rv));
        xbps_pkgdb_unlock(&xh);
        return rv;
    }

    print_transaction_summary();

    if (!prompt_confirm(assume_yes)) {
        fprintf(stderr, "Aborted.\n");
        xbps_pkgdb_unlock(&xh);
        return 1;
    }

    if (assume_yes)
        pipe_yes_to_stdin();

    rv = xbps_transaction_commit(&xh);
    if (rv != 0)
        fprintf(stderr, "dnf: autoremove failed: %s\n", strerror_xbps(rv));

    xbps_pkgdb_unlock(&xh);
    return rv;
}

struct check_update_arg {
    const char **pkgs;
    size_t npkgs;
    int found;
};

static int check_update_cb(struct xbps_handle *xhp, xbps_object_t obj,
                            const char *key, void *arg, bool *done)
{
    struct check_update_arg *cua = arg;
    xbps_dictionary_t pkgd = (xbps_dictionary_t)obj;
    const char *inst_pkgver = NULL, *repo_pkgver = NULL;
    char pkgname[64];
    int rv;

    (void)xhp; (void)done;

    /* If specific packages requested, filter */
    if (cua->npkgs > 0) {
        int match = 0;
        for (size_t i = 0; i < cua->npkgs; i++) {
            if (strcmp(key, cua->pkgs[i]) == 0) {
                match = 1;
                break;
            }
        }
        if (!match)
            return 0;
    }

    xbps_dictionary_get_cstring_nocopy(pkgd, "pkgver", &inst_pkgver);
    if (!inst_pkgver)
        return 0;

    /* Extract package name from pkgver */
    pkg_name_from_pkgver(pkgname, sizeof(pkgname), inst_pkgver);

    /* Check if newer version available in repos */
    xbps_dictionary_t repo_pkgd = xbps_rpool_get_pkg(xhp, pkgname);
    if (!repo_pkgd)
        return 0;

    xbps_dictionary_get_cstring_nocopy(repo_pkgd, "pkgver", &repo_pkgver);
    if (!repo_pkgver)
        return 0;

    /* Compare versions */
    rv = xbps_cmpver(repo_pkgver, inst_pkgver);
    if (rv > 0) {
        printf("%-30s %s -> %s\n", pkgname, inst_pkgver, repo_pkgver);
        cua->found = 1;
    }

    xbps_object_release(repo_pkgd);
    return 0;
}

int dnf_check_update(const char **pkgs, size_t npkgs)
{
    struct check_update_arg cua = { pkgs, npkgs, 0 };
    int rv;

    rv = xbps_pkgdb_foreach_cb(&xh, check_update_cb, &cua);
    if (rv != 0)
        return rv;

    if (!cua.found) {
        fprintf(stderr, "dnf: all packages are up-to-date.\n");
        return 0; /* dnf returns 0 even when no updates */
    }

    return 100; /* dnf returns 100 when updates are available */
}

int dnf_clean(const char *target)
{
    char cachedir[1024];
    DIR *dir;
    struct dirent *entry;
    int count = 0;

    if (strcmp(target, "all") != 0 && strcmp(target, "packages") != 0) {
        fprintf(stderr,
                "dnf: clean '%s' is not supported; supported: all, packages\n",
                target);
        return 2;
    }

    /* Build cache directory path */
    snprintf(cachedir, sizeof(cachedir), "%s/%s",
             xh.rootdir[0] ? xh.rootdir : "", XBPS_CACHE_PATH);
    /* Remove leading slash if rootdir is empty */
    if (cachedir[0] == '/' && cachedir[1] == '/')
        memmove(cachedir, cachedir + 1, strlen(cachedir));

    dir = opendir(cachedir);
    if (!dir) {
        fprintf(stderr, "dnf: cannot open cache directory '%s'\n", cachedir);
        return 1;
    }

    while ((entry = readdir(dir)) != NULL) {
        char filepath[1024];

        /* Skip . and .. */
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        /* Only remove .xbps files */
        if (!strstr(entry->d_name, ".xbps"))
            continue;

        snprintf(filepath, sizeof(filepath), "%s/%s", cachedir, entry->d_name);
        if (unlink(filepath) == 0)
            count++;
    }

    closedir(dir);

    printf("Removed %d cached package(s).\n", count);
    return 0;
}

struct provides_arg {
    const char *file;
    int found;
};

static int provides_repo_cb(struct xbps_repo *repo, void *arg, bool *done)
{
    struct provides_arg *pa = arg;
    xbps_object_iterator_t iter;
    xbps_object_t obj;

    (void)done;

    if (!repo->idx)
        return 0;

    iter = xbps_dictionary_iterator(repo->idx);
    if (!iter)
        return 0;

    while ((obj = xbps_object_iterator_next(iter)) != NULL) {
        const char *key = xbps_dictionary_keysym_cstring_nocopy(obj);
        xbps_dictionary_t pkgd = xbps_dictionary_get_keysym(repo->idx, obj);
        xbps_array_t files;
        const char *pkgver = NULL;

        /* Check files list */
        files = xbps_dictionary_get(pkgd, "files");
        if (files) {
            for (unsigned int i = 0; i < xbps_array_count(files); i++) {
                const char *fname = NULL;
                xbps_array_get_cstring_nocopy(files, i, &fname);
                if (fname && strcmp(fname, pa->file) == 0) {
                    xbps_dictionary_get_cstring_nocopy(pkgd, "pkgver", &pkgver);
                    printf("%s : %s\n", key, pkgver ? pkgver : "");
                    pa->found = 1;
                    break;
                }
            }
        }

        /* Also check conf_files list */
        files = xbps_dictionary_get(pkgd, "conf_files");
        if (files) {
            for (unsigned int i = 0; i < xbps_array_count(files); i++) {
                const char *fname = NULL;
                xbps_array_get_cstring_nocopy(files, i, &fname);
                if (fname && strcmp(fname, pa->file) == 0) {
                    xbps_dictionary_get_cstring_nocopy(pkgd, "pkgver", &pkgver);
                    printf("%s : %s\n", key, pkgver ? pkgver : "");
                    pa->found = 1;
                    break;
                }
            }
        }
    }

    xbps_object_iterator_release(iter);
    return 0;
}

struct provides_installed_arg {
    const char *file;
    int found;
};

static int provides_installed_cb(struct xbps_handle *xhp, xbps_object_t obj,
                                 const char *key, void *arg, bool *done)
{
    struct provides_installed_arg *pia = arg;
    xbps_dictionary_t pkgd = (xbps_dictionary_t)obj;
    xbps_dictionary_t files_dict;

    (void)xhp; (void)done;

    /* Get files dictionary for this package */
    files_dict = xbps_pkgdb_get_pkg_files(xhp, key);
    if (!files_dict)
        return 0;

    xbps_array_t files = xbps_dictionary_get(files_dict, "files");
    if (files) {
        for (unsigned int i = 0; i < xbps_array_count(files); i++) {
            xbps_dictionary_t file_dict = xbps_array_get(files, i);
            if (file_dict) {
                const char *fname = NULL;
                xbps_dictionary_get_cstring_nocopy(file_dict, "file", &fname);
                if (fname && strcmp(fname, pia->file) == 0) {
                    const char *pkgver = NULL;
                    xbps_dictionary_get_cstring_nocopy(pkgd, "pkgver", &pkgver);
                    printf("%s : %s\n", key, pkgver ? pkgver : "");
                    pia->found = 1;
                    break;
                }
            }
        }
    }

    xbps_object_release(files_dict);
    return 0;
}

int dnf_provides(const char *file)
{
    struct provides_arg pa = { file, 0 };
    struct provides_installed_arg pia = { file, 0 };
    int rv;

    /* Search in repositories */
    rv = xbps_rpool_foreach(&xh, provides_repo_cb, &pa);
    if (rv != 0)
        return rv;

    /* Search installed packages */
    rv = xbps_pkgdb_foreach_cb(&xh, provides_installed_cb, &pia);
    if (rv != 0)
        return rv;

    if (!pa.found && !pia.found) {
        fprintf(stderr, "dnf: no package found for file '%s'\n", file);
        return 1;
    }

    return 0;
}

int dnf_deps(const char *pkg)
{
    xbps_dictionary_t pkgd;
    xbps_array_t deps;
    const char *pkgver = NULL;

    /* Try repo first, then pkgdb */
    pkgd = xbps_rpool_get_pkg(&xh, pkg);
    if (!pkgd)
        pkgd = xbps_pkgdb_get_pkg(&xh, pkg);

    if (!pkgd) {
        fprintf(stderr, "dnf: package '%s' not found\n", pkg);
        return 1;
    }

    xbps_dictionary_get_cstring_nocopy(pkgd, "pkgver", &pkgver);
    printf("Dependencies for %s:\n", pkgver ? pkgver : pkg);

    deps = xbps_dictionary_get(pkgd, "run_depends");
    if (!deps || xbps_array_count(deps) == 0) {
        printf("  (none)\n");
        if (!xbps_rpool_get_pkg(&xh, pkg))
            xbps_object_release(pkgd);
        return 0;
    }

    for (unsigned int i = 0; i < xbps_array_count(deps); i++) {
        const char *depstr = NULL;
        xbps_array_get_cstring_nocopy(deps, i, &depstr);
        if (depstr)
            printf("  %s\n", depstr);
    }

    if (!xbps_rpool_get_pkg(&xh, pkg))
        xbps_object_release(pkgd);
    return 0;
}
