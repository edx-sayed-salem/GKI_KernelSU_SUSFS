#!/usr/bin/env python3
"""Install SAGE prop_view into the synced kernel tree.

Why: bionic maps the property areas MAP_SHARED in zygote, so every app inherits the same view.
This adds a small kernel-side swap (fs/susfs_prop_view.c) that gives an app process its own
copy of each area at the same address, plus the one-line trigger in __sys_setresuid's existing
CONFIG_KSU_SUSFS block.

Shape (CRC-neutral, as planned): new static code in its own file, no EXPORT_SYMBOL, no change
to any exported header, no CONFIG change, and the only shared file touched is kernel/sys.c,
inside the block SUSFS already owns.

Idempotent, and asserts on every anchor so an upstream change fails the build instead of
silently shipping a kernel with no prop_view.
"""
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "sage_prop_view")

INC_ANCHOR = "#include <linux/syscalls.h>"
CALL_ANCHOR = "(void)ksu_handle_setresuid(ruid, euid, suid);"
MAKE_LINE = "obj-$(CONFIG_KSU_SUSFS) += susfs_prop_view.o"


def fail(msg):
    print("ERROR: " + msg, file=sys.stderr)
    return 1


def main(root):
    fs = os.path.join(root, "fs")
    inc = os.path.join(root, "include", "linux")
    sysc = os.path.join(root, "kernel", "sys.c")
    for p in (fs, inc, sysc):
        if not os.path.exists(p):
            return fail("%s missing" % p)

    shutil.copy(os.path.join(SRC, "susfs_prop_view.c"),
                os.path.join(fs, "susfs_prop_view.c"))
    shutil.copy(os.path.join(SRC, "include", "linux", "sage_prop_view.h"),
                os.path.join(inc, "sage_prop_view.h"))

    mk = os.path.join(fs, "Makefile")
    s = open(mk).read()
    if "susfs_prop_view.o" in s:
        print("fs/Makefile already lists prop_view")
    else:
        if not s.endswith("\n"):
            s += "\n"
        s += "\n# SAGE prop_view (app-scoped property-area copy)\n" + MAKE_LINE + "\n"
        open(mk, "w").write(s)
        print("patched fs/Makefile")

    s = open(sysc).read()
    if "sage_prop_view_queue" in s:
        print("kernel/sys.c already calls prop_view")
        return 0

    if s.count(CALL_ANCHOR) != 1:
        return fail("setresuid anchor not found exactly once in kernel/sys.c")
    if s.count(INC_ANCHOR) != 1:
        return fail("include anchor not found exactly once in kernel/sys.c")

    s = s.replace(INC_ANCHOR, INC_ANCHOR + "\n#include <linux/sage_prop_view.h>", 1)
    s = s.replace(CALL_ANCHOR, CALL_ANCHOR + "\n\tsage_prop_view_queue();", 1)
    open(sysc, "w").write(s)
    print("patched kernel/sys.c")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else "."))
