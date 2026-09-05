# dnf - A dnf-style front-end for Void Linux's xbps

A command-line tool that provides familiar [dnf](https://github.com/rpm-software-management/dnf) (Fedora/RHEL) commands on Void Linux, using [libxbps](https://github.com/void-linux/xbps) directly for package operations.

## Overview

If you're coming from Fedora/RHEL and miss the `dnf` command syntax, this tool lets you use the same commands on Void Linux. It uses libxbps directly (no spawning xbps CLI tools) for fast, integrated package management.

### Key Features

- **Familiar syntax**: Use `dnf install`, `dnf remove`, `dnf upgrade`, etc.
- **Direct libxbps**: Uses libxbps API directly for install/remove/upgrade/search/list/info
- **Smart installation**: Skips already-installed packages with friendly messages
- **Transaction summary**: Shows package table before committing changes
- **Helpful hints**: Provides guidance for unsupported dnf options
- **Root policy**: Enforces root privileges for write operations

## Installation

### From Source

```bash
# Install dependencies (xbps development headers)
xbps-install -S libxbps-devel

# Build
make

# Install system-wide (requires root)
sudo make install
```

The binary is installed to `/usr/sbin/dnf` by default.

### Uninstall

```bash
sudo make uninstall
```

## Usage

```bash
dnf [options] COMMAND [ARGS...]
```

### Commands

| Command | Description | Status |
|---------|-------------|--------|
| `install PKG...` | Install packages | Implemented |
| `reinstall PKG...` | Reinstall packages | Implemented |
| `remove PKG...` | Remove packages | Implemented |
| `upgrade [PKG...]` | Upgrade all or specific packages | Implemented |
| `update [PKG...]` | Alias for upgrade | Implemented |
| `makecache` | Refresh repository index | Implemented |
| `search STR` | Search available packages | Implemented |
| `info PKG...` | Show package information | Implemented |
| `list [installed\|available\|all] [PAT]` | List packages | Implemented |
| `autoremove` | Remove orphaned packages | Implemented |
| `check-update [PKG...]` | Check for updates (no changes) | Implemented |
| `clean all\|packages` | Remove cached packages | Implemented |
| `provides FILE` | Find package owning file | Implemented |
| `deps PKG` | Show package dependencies | Implemented |
| `help` | Show help | Implemented |
| `version` | Show version | Implemented |

### Options

| Option | Description |
|--------|-------------|
| `-y`, `--assumeyes` | Assume yes to all prompts |
| `-v`, `--verbose` | Verbose output |
| `-q`, `--quiet` | Quiet output |
| `--refresh` | Sync repository index first (with install) |
| `--downloadonly` | Download only, do not install |
| `--installroot DIR` | Use DIR as root directory |
| `-h`, `--help` | Show help |
| `-V`, `--version` | Show version |

## Examples

```bash
# Install a package
sudo dnf install vim

# Install multiple packages with verbose output
sudo dnf -v install vim git htop

# Upgrade all packages
sudo dnf upgrade

# Search for packages
dnf search editor

# Show package info
dnf info vim

# List installed packages matching a pattern
dnf list installed vim

# Remove a package
sudo dnf remove vim

# Sync repository index
sudo dnf makecache
```

## Behavior

### Transaction Summary

Before committing write operations (install/remove/upgrade), dnf displays a transaction summary:

```
Name                 Action     Version            New version        Download size
----                 ------     -------            -----------        -------------
vim                  install    -                  9.2.0506_1         2098KB
vim-common           install    -                  9.2.0506_1         9110KB

Do you want to continue? [Y/n]
```

### Already-Installed Packages

When attempting to install a package that is already installed, dnf will:
1. Check with libxbps if the package is installed
2. Print a skip message: `dnf: 'xxx' is already installed.`
3. Continue with any remaining packages
4. Exit successfully (exit code 0)

### Root Privileges

Write commands (install, reinstall, remove, upgrade) require root privileges. If run as a regular user, dnf will display a helpful message showing how to re-run the command with sudo.

### Unsupported Options

Many dnf-specific options have no xbps equivalent. When used, dnf will display a helpful hint explaining why the option isn't supported and suggesting alternatives where possible.

## Testing

Run the test suite:

```bash
make test
```

The tests verify:
- Error handling for unsupported commands/options
- Root privilege enforcement
- Already-installed package skip behavior
- Read-only command functionality (search, list, info)

## Architecture

The program uses libxbps directly for all package operations:

```
User Command → dnf.c (parse) → xbps_backend.c (libxbps API) → Output
```

### Key Source Files

- `dnf.c` - Main program (command parsing, argument handling)
- `xbps_backend.h` - Backend API interface
- `xbps_backend.c` - libxbps implementation (init, install, remove, search, etc.)
- `Makefile` - Build system
- `test.sh` - Test suite

## Dependencies

- **libxbps**: XBPS package manager library
- **C compiler**: gcc or clang
- **POSIX system**: Linux

## Version

Current version: 0.3.0

## License

See source code for license information.

## Contributing

Contributions are welcome! Please ensure:
1. Code compiles with `-Wall -Wextra -Wpedantic`
2. All tests pass (`make test`)
3. New features include appropriate tests

## Roadmap

All commands are now implemented using libxbps directly. Future improvements:
- Progress bars for downloads
- Transaction dry-run mode
- Better conflict resolution messages
