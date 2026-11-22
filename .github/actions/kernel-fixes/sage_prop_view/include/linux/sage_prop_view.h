/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_SAGE_PROP_VIEW_H
#define _LINUX_SAGE_PROP_VIEW_H

/*
 * Queue a check of this process's property-area VMAs.  Called from __sys_setresuid inside the
 * CONFIG_KSU_SUSFS block; the actual work runs from task_work before the syscall returns to
 * userspace (see fs/susfs_prop_view.c).
 */
void sage_prop_view_queue(void);

#endif /* _LINUX_SAGE_PROP_VIEW_H */
