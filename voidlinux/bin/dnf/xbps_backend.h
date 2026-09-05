/*
 * xbps_backend.h - libxbps backend interface for dnf wrapper.
 * Provides direct libxbps implementations for package operations.
 */
#ifndef XBPS_BACKEND_H
#define XBPS_BACKEND_H

#include <stddef.h>

/* Initialize libxbps handle. Must be called before any other dnf_xbps_* function.
 * Saves stdin fd to restore after xbps_end().
 * Returns 0 on success, errno on failure. */
int  dnf_xbps_init(const char *rootdir, int verbose);

/* Release libxbps resources and restore stdin. */
void dnf_xbps_end(void);

/* Sync repository index. */
int  dnf_makecache(void);

/* Install packages. force=1 for reinstall. assume_yes=1 to skip prompt.
 * Skips already-installed packages unless force is set. */
int  dnf_install(const char **pkgs, size_t npkgs, int force, int assume_yes,
                 int downloadonly);

/* Remove packages. assume_yes=1 to skip prompt. */
int  dnf_remove(const char **pkgs, size_t npkgs, int assume_yes);

/* Upgrade packages. If npkgs==0, upgrade all. assume_yes=1 to skip prompt. */
int  dnf_upgrade(const char **pkgs, size_t npkgs, int assume_yes,
                 int downloadonly);

/* Search repository packages matching pattern (case-insensitive).
 * Prints matching packages to stdout. */
int  dnf_search(const char *pattern);

/* List packages. mode: "installed", "available", or "all".
 * pattern: optional filter (NULL for all). */
int  dnf_list(const char *mode, const char *pattern);

/* Show info for given packages. */
int  dnf_info(const char **pkgs, size_t npkgs);

/* Remove orphaned packages. assume_yes=1 to skip prompt. */
int  dnf_autoremove(int assume_yes);

/* Check for available updates. Does not modify system.
 * Prints packages with available updates. Returns 0 if updates available,
 * 1 if all up-to-date. */
int  dnf_check_update(const char **pkgs, size_t npkgs);

/* Clean cached packages. target: "all" or "packages". */
int  dnf_clean(const char *target);

/* Find package owning a file. */
int  dnf_provides(const char *file);

/* Show dependencies for a package. */
int  dnf_deps(const char *pkg);

#endif /* XBPS_BACKEND_H */
