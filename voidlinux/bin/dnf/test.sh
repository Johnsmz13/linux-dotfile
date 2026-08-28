#!/bin/sh
# Unit tests for the dnf -> xbps wrapper.
# Runs against --print (translation only, no system changes), plus
# strict-error and root-policy checks that never spawn xbps.

BIN=./dnf
pass=0
fail=0

# t <description> <expected-print-output> <args...>
t() {
    desc="$1"; expected="$2"; shift 2
    out="$("$BIN" --print "$@" 2>&1)"
    if [ "$out" = "$expected" ]; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        echo "FAIL: $desc"
        echo "  args:     $*"
        echo "  expected: $expected"
        echo "  got:      $out"
    fi
}

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

# Multi-line / quote-heavy expected outputs.
EXP_LIST_FILTERED="xbps-query -l
# (output filtered by pattern: vim)"
EXP_LIST_AVAIL="xbps-query -R -s ''"
EXP_LIST_ALL="xbps-query -l
xbps-query -R -s ''"

# ---- translation tests (--print) ----

t "install"                       "xbps-install vim"                    install vim
t "install -y before command"     "xbps-install -y vim"                 -y install vim
t "install -y after command"      "xbps-install -y vim"                 install -y vim
t "install --refresh"             "xbps-install -S vim"                 install --refresh vim
t "install --downloadonly"        "xbps-install -D vim"                 install --downloadonly vim
t "install -y --refresh"          "xbps-install -S -y vim"              install -y --refresh vim
t "install combined short flags"  "xbps-install -y -v vim"              -yv install vim
t "install --installroot"         "xbps-install -r /mnt vim"            install --installroot /mnt vim
t "install --installroot=dir"     "xbps-install -r /mnt vim"            install --installroot=/mnt vim
t "reinstall"                     "xbps-install -f vim"                 reinstall vim
t "remove"                        "xbps-remove vim"                     remove vim
t "remove -y"                     "xbps-remove -y vim"                  remove -y vim
t "autoremove"                    "xbps-remove -o"                      autoremove
t "upgrade"                       "xbps-install -Su"                    upgrade
t "upgrade package"               "xbps-install -Su vim"                upgrade vim
t "update alias"                  "xbps-install -Su"                    update
t "check-update"                  "xbps-install -Sun"                   check-update
t "makecache"                     "xbps-install -S"                     makecache
t "clean all"                     "xbps-remove -O"                      clean all
t "clean packages"                "xbps-remove -O"                      clean packages
t "search"                        "xbps-query -Rs vim"                  search vim
t "info"                          "xbps-query -RS vim"                  info vim
t "provides"                      "xbps-query -Ro /usr/bin/vim"         provides /usr/bin/vim
t "whatprovides"                  "xbps-query -Ro /usr/bin/vim"         whatprovides /usr/bin/vim
t "deps"                          "xbps-query -Rx vim"                  deps vim
t "list default"                  "xbps-query -l"                       list
t "list installed"                "xbps-query -l"                       list installed
t "list installed filtered"       "$EXP_LIST_FILTERED"                   list installed vim
t "list available"                "$EXP_LIST_AVAIL"                      list available
t "list all"                      "$EXP_LIST_ALL"                        list all
t "quiet swallowed"               "xbps-install vim"                    install -q vim

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
expect_exit "clean metadata"      2 clean metadata
expect_exit "clean no target"     2 clean
expect_exit "search no pattern"   2 search
expect_exit "search two patterns" 2 search a b
expect_exit "provides no file"    2 provides
expect_exit "deps no pkg"         2 deps
expect_exit "install --best"      2 install --best vim
expect_exit "install --allowerasing" 2 install --allowerasing vim
expect_exit "install --nodocs"    2 install --nodocs vim
expect_exit "install --setopt"    2 install --setopt=skip_broken=1 vim
expect_exit "install --releasever" 2 install --releasever=39 vim
expect_exit "install --cacheonly" 2 install --cacheonly vim
expect_exit "install --disablerepo" 2 install --disablerepo=updates vim
expect_exit "unknown option"      2 install --frobnicate vim
expect_exit "list too many args"  2 list installed vim extra

# ---- help/version ----

expect_exit "help"                0 --help
expect_exit "help command"        0 help
expect_exit "version"             0 --version
expect_exit "version command"     0 version

# ---- root policy (only meaningful as a non-root user) ----

if [ "$(id -u)" -ne 0 ]; then
    for cmd in "install vim" "reinstall vim" "remove vim" "autoremove" "upgrade" "update" "clean all"; do
        # shellcheck disable=SC2086
        expect_exit "write command needs root: $cmd" 1 $cmd
    done
fi

# ---- summary ----

echo "passed: $pass"
echo "failed: $fail"
[ "$fail" -eq 0 ]
