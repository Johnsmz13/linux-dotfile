#!/bin/sh
# Unit tests for the dnf -> xbps wrapper.
# Tests error handling, help/version, root policy, and basic behavior.

BIN=./dnf
pass=0
fail=0
skip=0

# expect_exit <description> <expected-exit> <args...>
expect_exit() {
    desc="$1"; want="$2"; shift 2
    "$BIN" "$@" >/dev/null 2>&1
    rc=$?
    if [ "$rc" -eq "$want" ]; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        echo "FAIL: $desc (expected exit $want, got $rc)"
    fi
}

# expect_output <description> <expected-substring> <args...>
expect_output() {
    desc="$1"; pattern="$2"; shift 2
    out="$("$BIN" "$@" 2>&1)"
    if echo "$out" | grep -q "$pattern"; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        echo "FAIL: $desc"
        echo "  args:     $*"
        echo "  expected output containing: $pattern"
        echo "  got:      $out"
    fi
}

# ---- strict errors: unsupported commands/options ----

expect_exit "no arguments"        2
expect_exit "unknown command"     2 frobnicate
expect_exit "group"               2 group install base
expect_exit "groupinstall"        2 groupinstall base
expect_exit "history"             2 history
expect_exit "distro-sync"         2 distro-sync
expect_exit "downgrade"           2 downgrade vim
expect_exit "swap"                2 swap
expect_exit "repoquery"           2 repoquery --installed
expect_exit "deplist"             2 deplist vim
expect_exit "search no pattern"   2 search
expect_exit "search two patterns" 2 search a b
expect_exit "list too many args"  2 list installed vim extra
expect_exit "install --best"      2 install --best vim
expect_exit "install --allowerasing" 2 install --allowerasing vim
expect_exit "install --nodocs"    2 install --nodocs vim
expect_exit "install --setopt"    2 install --setopt=skip_broken=1 vim
expect_exit "install --releasever" 2 install --releasever=39 vim
expect_exit "install --cacheonly" 2 install --cacheonly vim
expect_exit "install --disablerepo" 2 install --disablerepo=updates vim
expect_exit "unknown option"      2 install --frobnicate vim

# These need root to get past root check, but error on missing args
# or are not yet implemented - test as non-root for "needs root" exit 1
if [ "$(id -u)" -ne 0 ]; then
    expect_exit "install no pkg (needs root)"   1 install
    expect_exit "reinstall no pkg (needs root)" 1 reinstall
    expect_exit "remove no pkg (needs root)"    1 remove
    expect_exit "info no pkg"                   2 info
    expect_exit "provides no file"              2 provides
    expect_exit "deps no pkg"                   2 deps
    expect_exit "clean no target (needs root)"  1 clean
    expect_exit "clean metadata (needs root)"   1 clean metadata
else
    expect_exit "install no pkg"   2 install
    expect_exit "reinstall no pkg" 2 reinstall
    expect_exit "remove no pkg"    2 remove
    expect_exit "info no pkg"      2 info
    expect_exit "provides no file" 2 provides
    expect_exit "deps no pkg"      2 deps
    expect_exit "clean no target"  2 clean
    expect_exit "clean metadata"   2 clean metadata
fi

# ---- help/version ----

expect_exit "help"                0 --help
expect_exit "help command"        0 help
expect_exit "version"             0 --version
expect_exit "version command"     0 version

expect_output "help shows usage"  "Usage: dnf" --help
expect_output "version shows ver" "dnf $("$BIN" --version 2>&1 | awk '{print $2}')" --version

# ---- root policy (only meaningful as a non-root user) ----

if [ "$(id -u)" -ne 0 ]; then
    for cmd in "install vim" "reinstall vim" "remove vim" "upgrade" "update"; do
        # shellcheck disable=SC2086
        expect_exit "write command needs root: $cmd" 1 $cmd
    done
    # These are write commands that need root
    expect_exit "autoremove needs root"   1 autoremove
    expect_exit "clean all needs root"    1 clean all
    # These are read-only commands (implemented)
    expect_exit "check-update"        0 check-update
    expect_exit "provides not found"  1 provides /nonexistent/file
    expect_exit "deps"                0 deps bash
else
    # When running as root
    expect_exit "autoremove"        0 autoremove
    expect_exit "clean all"         0 clean all
    expect_exit "check-update"      0 check-update
    expect_exit "provides not found" 1 provides /nonexistent/file
    expect_exit "deps"              0 deps bash
fi

# ---- installed package skip behavior (requires root) ----

if [ "$(id -u)" -eq 0 ] && command -v xbps-query >/dev/null 2>&1; then
    if xbps-query bash >/dev/null 2>&1; then
        out="$("$BIN" install bash 2>&1)"
        rc=$?
        if [ "$rc" -eq 0 ]; then
            pass=$((pass + 1))
        else
            fail=$((fail + 1))
            echo "FAIL: install already-installed package should exit 0"
            echo "  got exit: $rc"
        fi
        if echo "$out" | grep -q "already installed"; then
            pass=$((pass + 1))
        else
            fail=$((fail + 1))
            echo "FAIL: install already-installed package should show skip message"
            echo "  got: $out"
        fi
    else
        skip=$((skip + 2))
    fi
else
    skip=$((skip + 2))
fi

# ---- read-only commands (no root required) ----

if command -v xbps-query >/dev/null 2>&1; then
    # search
    out="$("$BIN" search bash 2>&1)"
    rc=$?
    if [ "$rc" -eq 0 ] && echo "$out" | grep -q "bash"; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        echo "FAIL: search bash should find results (rc=$rc)"
    fi

    # list installed
    out="$("$BIN" list installed 2>&1)"
    rc=$?
    if [ "$rc" -eq 0 ] && echo "$out" | grep -q "bash"; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        echo "FAIL: list installed should show bash (rc=$rc)"
    fi

    # info
    out="$("$BIN" info bash 2>&1)"
    rc=$?
    if [ "$rc" -eq 0 ] && echo "$out" | grep -q "Name"; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        echo "FAIL: info bash should show package info (rc=$rc)"
    fi

    # list with filter
    out="$("$BIN" list installed bash 2>&1)"
    rc=$?
    if [ "$rc" -eq 0 ] && echo "$out" | grep -q "bash"; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        echo "FAIL: list installed bash should filter results (rc=$rc)"
    fi
else
    skip=$((skip + 4))
fi

# ---- summary ----

echo ""
echo "passed: $pass"
echo "failed: $fail"
echo "skipped: $skip"
[ "$fail" -eq 0 ]
