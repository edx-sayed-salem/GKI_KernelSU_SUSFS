/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_SAGE_PROP_VIEW_H
#define _LINUX_SAGE_PROP_VIEW_H

/*
 * Queue a check of this process's property-area VMAs.  Called from __sys_setresuid inside the
 * CONFIG_KSU_SUSFS block; the actual work runs from task_work before the syscall returns to
 * userspace (see fs/susfs_prop_view.c).
 */
void sage_prop_view_queue(void);

/*
 * Remap an app's freshly-mapped live property area onto the staged copy.  bionic maps property
 * areas lazily, so many first appear AFTER __sys_setresuid's task_work has already run; without
 * this those areas stay live and their properties remain app-visible.  Called from
 * ksys_mmap_pgoff with the file the app mapped; a missing copy means "leave it alone".
 */
struct file;
void sage_prop_view_after_mmap(struct file *file, unsigned long addr, unsigned long len);

#endif /* _LINUX_SAGE_PROP_VIEW_H */
