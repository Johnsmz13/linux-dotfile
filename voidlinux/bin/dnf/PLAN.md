# Plan: Migrate to Direct libxbps Usage (Part 2)

## Status: In Progress

### Completed (v0.2.0)

The following commands have been implemented using libxbps directly:

| Command | Status | API Used |
|---------|--------|----------|
| `install` | Done | `xbps_transaction_install_pkg()` |
| `reinstall` | Done | `xbps_transaction_install_pkg(force=true)` |
| `remove` | Done | `xbps_transaction_remove_pkg()` |
| `upgrade`/`update` | Done | `xbps_transaction_update_pkg()` / `xbps_transaction_update_packages()` |
| `search` | Done | `xbps_rpool_foreach()` |
| `list` | Done | `xbps_pkgdb_foreach_cb()` / `xbps_rpool_foreach()` |
| `info` | Done | `xbps_rpool_get_pkg()` / `xbps_pkgdb_get_pkg()` |
| `makecache` | Done | `xbps_rpool_sync()` |

### Remaining

| Command | Status | Notes |
|---------|--------|-------|
| `autoremove` | Planned | `xbps_transaction_autoremove_pkgs()` |
| `check-update` | Planned | Query only, no transaction |
| `clean` | Planned | Cache directory cleanup |
| `provides` | Planned | Iterate repos, check file lists |
| `deps` | Planned | Read `run_depends` from package dict |

## Architecture (Implemented)

```
User Command → dnf.c (parse) → xbps_backend.c (libxbps API) → Output
```

### Files

- `dnf.c` - Command parsing and argument handling
- `xbps_backend.h` - Backend API interface
- `xbps_backend.c` - libxbps implementation
- `Makefile` - Build system (links with -lxbps)
- `test.sh` - Test suite

## API Reference

### Initialization

```c
int  dnf_xbps_init(const char *rootdir, int verbose);
void dnf_xbps_end(void);
```

- `rootdir`: Optional root directory (NULL for default "/")
- `verbose`: Enable verbose output
- Saves/restores stdin fd (xbps_end() closes it)

### Write Operations

```c
int dnf_install(const char **pkgs, size_t npkgs, int force, int assume_yes, int downloadonly);
int dnf_remove(const char **pkgs, size_t npkgs, int assume_yes);
int dnf_upgrade(const char **pkgs, size_t npkgs, int assume_yes, int downloadonly);
```

Transaction flow:
1. `xbps_pkgdb_lock()`
2. Queue packages with `xbps_transaction_*_pkg()`
3. `xbps_transaction_prepare()`
4. Display transaction summary
5. Prompt for confirmation (or pipe "y" if assume_yes)
6. `xbps_transaction_commit()`
7. `xbps_pkgdb_unlock()`

### Read Operations

```c
int dnf_search(const char *pattern);
int dnf_list(const char *mode, const char *pattern);
int dnf_info(const char **pkgs, size_t npkgs);
int dnf_makecache(void);
```

### Key libxbps Functions Used

```c
// Package state
int xbps_pkg_is_installed(struct xbps_handle *xhp, const char *pkg);

// Transaction
int xbps_transaction_install_pkg(struct xbps_handle *xhp, const char *pkg, bool force);
int xbps_transaction_remove_pkg(struct xbps_handle *xhp, const char *pkgname, bool recursive);
int xbps_transaction_update_pkg(struct xbps_handle *xhp, const char *pkgname, bool force);
int xbps_transaction_update_packages(struct xbps_handle *xhp);
int xbps_transaction_prepare(struct xbps_handle *xhp);
int xbps_transaction_commit(struct xbps_handle *xhp);

// Database lock
int  xbps_pkgdb_lock(struct xbps_handle *xhp);
void xbps_pkgdb_unlock(struct xbps_handle *xhp);

// Repository
int xbps_rpool_sync(struct xbps_handle *xhp, const char *uri);
int xbps_rpool_foreach(struct xbps_handle *xhp,
    int (*fn)(struct xbps_repo *, void *, bool *), void *arg);

// Package database
int xbps_pkgdb_foreach_cb(struct xbps_handle *xhp,
    int (*fn)(struct xbps_handle *, xbps_object_t, const char *, void *, bool *),
    void *arg);

// Package lookup
xbps_dictionary_t xbps_rpool_get_pkg(struct xbps_handle *xhp, const char *pkg);
xbps_dictionary_t xbps_pkgdb_get_pkg(struct xbps_handle *xhp, const char *pkg);
```

### Callbacks

State callback for progress output:
```c
static int state_cb(const struct xbps_state_cb_data *xscd, void *arg);
```

Handles states: INSTALL, INSTALL_DONE, UPDATE, UPDATE_DONE, REMOVE, REMOVE_DONE, DOWNLOAD, TRANS_DOWNLOAD, TRANS_RUN, TRANS_CONFIGURE, TRANS_FAIL, REPOSYNC.

### Error Handling

Map errno to user-friendly messages:
- EEXIST: Already installed/up-to-date
- ENOENT: Package not found
- ENOSPC: Not enough disk space
- ENODEV: Missing dependencies
- EAGAIN: Package conflicts
- EPERM: Permission denied
- ENOTSUP: No repositories available
- ENXIO: Dependency resolution failed
- EBUSY: xbps needs updating first

### `-y` (assume yes) Implementation

After `xbps_transaction_prepare()`, if assume_yes is set:
1. Create a pipe
2. Write "y\n" to pipe
3. Redirect stdin to read from pipe
4. Call `xbps_transaction_commit()`

This makes xbps auto-accept the confirmation prompt.

## Implementation Details

### stdin Preservation

xbps_end() closes fd 0 (stdin). Solution:
```c
saved_stdin = dup(STDIN_FILENO);
// ... xbps_init/end ...
dup2(saved_stdin, STDIN_FILENO);
close(saved_stdin);
```

### Transaction Summary

After `xbps_transaction_prepare()`, iterate `xh.transd` packages array:
- Display table with Name/Action/Version/New version/Download size
- Use `xbps_transaction_pkg_type()` for action string
- Use `xbps_humanize_number()` for size formatting

## Future Enhancements

1. Implement remaining commands (autoremove, check-update, clean, provides, deps)
2. Progress bars for downloads
3. Transaction dry-run mode
4. Better conflict resolution messages
5. Configuration file support
