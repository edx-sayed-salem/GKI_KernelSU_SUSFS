#!/usr/bin/env python3
"""Fix fanotify_fdinfo() arity in fs/notify/fdinfo.c after the SUSFS patch.

Some SUSFS patches widen show_fdinfo() to a 3-argument callback:

    #if defined(CONFIG_KSU_SUSFS_SUS_MOUNT) || defined(CONFIG_KSU_SUSFS_SUS_KSTAT)
    static void show_fdinfo(struct seq_file *m, struct file *f,
                            void (*show)(struct seq_file *m,
                                         struct fsnotify_mark *mark,
                                         struct file *file))
    ...
            show(m, mark, f);

They adapt the inotify callbacks to that shape but not the fanotify one, so
fanotify_fdinfo() stays 2-argument. That is invisible while CONFIG_FANOTIFY=n,
because the whole block compiles out. With fanotify enabled it is a hard error:

    fs/notify/fdinfo.c:221:20: error: incompatible function pointer types passing
      'void (struct seq_file *, struct fsnotify_mark *)' to parameter of type
      'void (*)(struct seq_file *, struct fsnotify_mark *, struct file *)'

Add the missing `struct file *file` parameter to fanotify_fdinfo() whenever
show_fdinfo() is in the widened form. The parameter is unused by design (the
inotify side is what needs `file` for sus_mount path resolution), and the kernel
does not build with -Wunused-parameter.

When the widened show_fdinfo() sits behind the SUSFS config guard (6.6 and
later), fanotify_fdinfo() must carry the *same* guard: widening it
unconditionally breaks every build that resolves CONFIG_KSU_SUSFS_SUS_MOUNT and
CONFIG_KSU_SUSFS_SUS_KSTAT off (or drops them), because the narrow variant of
show_fdinfo() is the one that compiles and it takes a 2-argument callback:

    fs/notify/fdinfo.c:220:20: error: incompatible function pointer types passing
      'void (struct seq_file *, struct fsnotify_mark *, struct file *)' to
      parameter of type 'void (*)(struct seq_file *, struct fsnotify_mark *)'

So: mirror the guard when present, widen unconditionally only for the older
patch shape that has no guard.

Idempotent, anchored, fails loudly if the file is in neither expected shape.

Usage: susfs_fdinfo_fanotify_arity.py <kernel/common>
"""

import os
import re
import sys
from typing import NoReturn

WIDE_SHOW = re.compile(
    r"static void show_fdinfo\(struct seq_file \*m, struct file \*f,\s*\n"
    r"\s*void \(\*show\)\(struct seq_file \*m,\s*\n"
    r"\s*struct fsnotify_mark \*mark,\s*\n"
    r"\s*struct file \*file\)\)"
)
# The same widened definition, behind the SUSFS config guard the patch uses.
WIDE_SHOW_GUARDED = re.compile(
    r"#if defined\(CONFIG_KSU_SUSFS_SUS_MOUNT\) \|\| "
    r"defined\(CONFIG_KSU_SUSFS_SUS_KSTAT\)\s*\n"
    r"static void show_fdinfo\(struct seq_file \*m, struct file \*f,\s*\n"
    r"\s*void \(\*show\)\(struct seq_file \*m,\s*\n"
    r"\s*struct fsnotify_mark \*mark,\s*\n"
    r"\s*struct file \*file\)\)"
)
FAN_2ARG = re.compile(
    r"static void fanotify_fdinfo\(struct seq_file \*m, struct fsnotify_mark \*mark\)"
)
FAN_3ARG = re.compile(
    r"static void fanotify_fdinfo\(struct seq_file \*m, struct fsnotify_mark \*mark,"
    r"\s*\n?\s*struct file \*file\)"
)
FAN_3ARG_GUARDED = re.compile(
    r"#if defined\(CONFIG_KSU_SUSFS_SUS_MOUNT\) \|\| "
    r"defined\(CONFIG_KSU_SUSFS_SUS_KSTAT\)\s*\n"
    r"static void fanotify_fdinfo\(struct seq_file \*m, struct fsnotify_mark \*mark,"
    r"\s*\n?\s*struct file \*file\)"
)


def die(msg: str) -> NoReturn:
    sys.exit(f"ERROR: {msg}")


def main() -> None:
    if len(sys.argv) != 2:
        die("usage: susfs_fdinfo_fanotify_arity.py <kernel/common>")
    path = os.path.join(sys.argv[1].rstrip("/"), "fs/notify/fdinfo.c")
    if not os.path.exists(path):
        print("  fdinfo.c: not present (nothing to do)")
        return

    src = open(path, encoding="utf-8").read()

    if not WIDE_SHOW.search(src):
        print("  fdinfo.c: show_fdinfo() is not widened - nothing to do")
        return
    # 6.6+ wraps the widened definition in the SUSFS config guard; the arity of
    # fanotify_fdinfo() has to follow the same condition or one of the two
    # config states fails to compile.
    guarded = bool(WIDE_SHOW_GUARDED.search(src))
    if FAN_3ARG.search(src):
        print("  fdinfo.c: fanotify_fdinfo() already takes file")
        return
    if FAN_3ARG_GUARDED.search(src):
        print("  fdinfo.c: fanotify_fdinfo() already follows the SUSFS guard")
        return
    if not FAN_2ARG.search(src):
        # fanotify may be compiled out of this tree entirely.
        print("  fdinfo.c: no 2-argument fanotify_fdinfo() found - nothing to do")
        return

    if guarded:
        replacement = (
            "#if defined(CONFIG_KSU_SUSFS_SUS_MOUNT) || defined(CONFIG_KSU_SUSFS_SUS_KSTAT)\n"
            "static void fanotify_fdinfo(struct seq_file *m, struct fsnotify_mark *mark,\n"
            "\t\t\t     struct file *file)\n"
            "#else\n"
            "static void fanotify_fdinfo(struct seq_file *m, struct fsnotify_mark *mark)\n"
            "#endif // #if defined(CONFIG_KSU_SUSFS_SUS_MOUNT) || defined(CONFIG_KSU_SUSFS_SUS_KSTAT)"
        )
    else:
        replacement = (
            "static void fanotify_fdinfo(struct seq_file *m, struct fsnotify_mark *mark,\n"
            "\t\t\t     struct file *file)"
        )

    src = FAN_2ARG.sub(replacement, src, count=1)
    open(path, "w", encoding="utf-8").write(src)

    check = open(path, encoding="utf-8").read()
    if guarded and not FAN_3ARG_GUARDED.search(check):
        die("fdinfo.c: fanotify_fdinfo() is not behind the SUSFS guard after rewrite")
    if not guarded and not FAN_3ARG.search(check):
        die("fdinfo.c: fanotify_fdinfo() still 2-argument after rewrite")
    print(
        "  fdinfo.c: fanotify_fdinfo() "
        + ("follows the SUSFS guard" if guarded else "widened to match show_fdinfo()")
    )


if __name__ == "__main__":
    main()
