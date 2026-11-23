#!/usr/bin/env python3
"""Make the kernel treat the emulator SDK's prebuilt vendor modules -- and SAGE's tagged OOT\nmodules -- as platform modules.

Why: the system image ships goldfish_*, v4l2loopback, virtio-media and vkms as prebuilt
(and unsigned) modules. They can never carry the "intree" modinfo tag, so every boot raises
TAINT_OOT_MODULE and /proc/sys/kernel/tainted reads 4096 on a device that otherwise looks
stock. Exempt only that known set: anything else out-of-tree still taints, so the bit keeps
its meaning for unknown modules.

Applied to kernel/module/main.c in module_augment_kernel_taints(). Idempotent; asserts on a
missing anchor so an upstream change fails the build instead of silently doing nothing.
"""
import sys

HELPER = '''/*
 * SAGE: the emulator SDK image ships its vendor modules prebuilt and unsigned, so they can
 * never carry the "intree" tag. Treat that known set as part of the platform instead of
 * tainting the kernel; anything else out-of-tree still raises TAINT_OOT_MODULE, so the bit
 * keeps its meaning for unknown modules.
 *
 * SAGE's own out-of-tree modules opt in with MODULE_INFO(sage_trusted, "1"). This is an explicit
 * marker, not a security control -- root can load anything and can forge the tag -- but it lets an
 * experimental LKM be tried without another kernel rebuild, and it stays auditable by grepping the
 * module's .modinfo. A module that does not claim it still taints.
 */
static bool sage_vendor_module(const char *name, struct load_info *info)
{
\tstatic const char *const names[] = {
\t\t"goldfish_address_space", "goldfish_pipe", "goldfish_sync",
\t\t"v4l2loopback", "virtio_media", "vkms",
\t};
\tint i;

\tfor (i = 0; i < ARRAY_SIZE(names); i++)
\t\tif (!strcmp(name, names[i]))
\t\t\treturn true;
\tif (get_modinfo(info, "sage_trusted"))
\t\treturn true;
\treturn false;
}

'''

ANCHOR = 'static void module_augment_kernel_taints(struct module *mod, struct load_info *info)'
OLD_COND = '\tif (!get_modinfo(info, "intree")) {'
NEW_COND = '\tif (!get_modinfo(info, "intree") && !sage_vendor_module(mod->name, info)) {'


def main(path: str) -> int:
    src = open(path).read()

    if 'sage_vendor_module' in src:
        print('already patched: %s' % path)
        return 0

    if src.count(ANCHOR) != 1:
        print('ERROR: anchor not found exactly once in %s' % path, file=sys.stderr)
        return 1
    if src.count(OLD_COND) != 1:
        print('ERROR: condition not found exactly once in %s' % path, file=sys.stderr)
        return 1

    src = src.replace(ANCHOR, HELPER + ANCHOR, 1)
    src = src.replace(OLD_COND, NEW_COND, 1)
    open(path, 'w').write(src)
    print('patched: %s' % path)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1]))
