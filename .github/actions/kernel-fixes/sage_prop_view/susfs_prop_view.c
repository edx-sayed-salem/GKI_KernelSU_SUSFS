// SPDX-License-Identifier: GPL-2.0
/*
 * SAGE prop_view: give an app process its own Android property-area view.
 *
 * bionic maps each property area (/dev/__properties__/<ctx>, 128 KB) MAP_SHARED PROT_READ in
 * zygote, before fork, so every app inherits the same mappings.  To give an app a different
 * view, the kernel replaces, for that process only, each property-area VMA with a mapping of
 * a fabricated copy at the same address.  Root, shell and every non-app process keep the real
 * area.
 *
 * Trigger: __sys_setresuid (the transition a zygote child makes when it becomes an app).  The
 * syscall path is not allowed to walk VMAs or take the mmap lock for write, so it only queues
 * task_work; the callback runs in process context before the syscall returns to userspace.
 *
 * No registry, no supercall, no /proc interface: the property areas are a fixed set of paths,
 * so the copy for a context is looked up by convention --
 *
 *     real:  /dev/__properties__/<ctx>
 *     copy:  <SAGE_PROP_VIEW_DIR>/<ctx>        (same basename, same 128 KB length)
 *
 * If the copy does not exist (or is the wrong length) the VMA is left untouched, so a device
 * with no copies staged behaves exactly as before.
 *
 * Credentials: the app's SELinux domain and DAC credentials may not be allowed to open a file
 * under /data, so the open and the mmap are done under kernel credentials.  The mapped pages
 * are read-only and the app never resolves this path itself.
 */

#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/mm.h>
#include <linux/mm_types.h>
#include <linux/mman.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/namei.h>
#include <linux/cred.h>
#include <linux/init_task.h>
#include <linux/task_work.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/dcache.h>
#include <linux/stat.h>
#include <linux/limits.h>

#include <linux/sage_prop_view.h>

#define SAGE_PROP_VIEW_DIR "/data/misc/sage/propview"
#define SAGE_PROP_VIEW_AREAS_PARENT "__properties__"
#define SAGE_PROP_VIEW_COPY_PARENT "propview"
#define SAGE_PROP_VIEW_HITS 128

struct sage_pv_hit {
	unsigned long start;
	unsigned long len;
	char name[NAME_MAX + 1];
};

struct sage_pv_work {
	struct callback_head cb;
};

/* The property areas live directly under /dev/__properties__; so do the copies under our
 * directory.  Comparing the parent dentry's name avoids building paths on every VMA.  The
 * VMA holds a reference to vm_file, so the dentry cannot go away underneath us; a rename of
 * these directories is not something that happens at runtime. */
static bool sage_pv_parent_is(const struct dentry *d, const char *name)
{
	return d && d->d_parent && strcmp(d->d_parent->d_name.name, name) == 0;
}

static void sage_pv_task_work(struct callback_head *cb)
{
	struct sage_pv_work *w = container_of(cb, struct sage_pv_work, cb);
	struct mm_struct *mm = current->mm;
	struct sage_pv_hit *hits;
	const struct cred *root;
	char *path;
	struct vm_area_struct *vma;
	int n = 0, i;

	kfree(w);
	pr_info_ratelimited("sage_prop_view: cb uid=%u mm=%d\n", current_uid().val, mm ? 1 : 0);
	if (!mm)
		return;
	/* The syscall entry runs before the creds change, so the app uid is only visible here,
	 * after the kernel applied it and before returning to userspace. */
	if (current_uid().val < 10000)
		return;

	/* Heap, not stack: 16 hit slots with a NAME_MAX name each plus PATH_MAX would blow
	 * the 2 KB kernel stack-frame limit under -Wframe-larger-than. */
	hits = kzalloc(sizeof(*hits) * SAGE_PROP_VIEW_HITS, GFP_KERNEL);
	path = kmalloc(PATH_MAX, GFP_KERNEL);
	if (!hits || !path) {
		kfree(hits);
		kfree(path);
		return;
	}

	mmap_read_lock(mm);
	VMA_ITERATOR(vmi, mm, 0);
	for_each_vma(vmi, vma) {
		struct dentry *d;
		size_t len;

		if (!vma->vm_file)
			continue;
		d = vma->vm_file->f_path.dentry;
		if (!d)
			continue;
		/* Already ours: do not remap a copy onto itself on a repeated setresuid. */
		if (sage_pv_parent_is(d, SAGE_PROP_VIEW_COPY_PARENT))
			continue;
		if (!sage_pv_parent_is(d, SAGE_PROP_VIEW_AREAS_PARENT))
			continue;
		if (strncmp(d->d_name.name, "u:object_r:", 11) != 0)
			continue;

		len = strlen(d->d_name.name);
		if (len == 0 || len > NAME_MAX)
			continue;
		if (n >= SAGE_PROP_VIEW_HITS)
			break;

		hits[n].start = vma->vm_start;
		hits[n].len = vma->vm_end - vma->vm_start;
		memcpy(hits[n].name, d->d_name.name, len);
		hits[n].name[len] = '\0';
		n++;
	}
	mmap_read_unlock(mm);
	pr_info_ratelimited("sage_prop_view: collected %d area(s)\n", n);

	/* init creds for the open and the mmap. get_task_cred() is a refcount bump on init_task's
	 * creds, so it cannot fail (prepare_kernel_cred allocates, and returned NULL here). */
	root = get_task_cred(&init_task);

	for (i = 0; i < n; i++) {
		struct file *f;
		const struct cred *saved;
		unsigned long addr;

		snprintf(path, PATH_MAX, SAGE_PROP_VIEW_DIR "/%s", hits[i].name);

		saved = override_creds(root);
		f = filp_open(path, O_RDONLY | O_LARGEFILE, 0);
		revert_creds(saved);

		if (IS_ERR(f)) {
			pr_warn_ratelimited("sage_prop_view: open %s failed %ld\n",
					    path, (long)PTR_ERR(f));
			continue;
		}
		pr_info_ratelimited("sage_prop_view: hit %s len=%lu\n", path, hits[i].len);
		if ((unsigned long)i_size_read(file_inode(f)) != hits[i].len) {
			pr_warn_ratelimited("sage_prop_view: %s is %lld bytes, VMA is %lu\n",
					    path, (long long)i_size_read(file_inode(f)),
					    hits[i].len);
			fput(f);
			continue;
		}

		saved = override_creds(root);
		addr = vm_mmap(f, hits[i].start, hits[i].len, PROT_READ,
			       MAP_SHARED | MAP_FIXED, 0);
		revert_creds(saved);

		if (addr != hits[i].start)
			pr_warn_ratelimited("sage_prop_view: vm_mmap %s at %#lx failed %ld\n",
					    path, hits[i].start, (long)addr);
		fput(f);
	}

	put_cred(root);
	kfree(path);
	kfree(hits);
}

void sage_prop_view_queue(void)
{
	struct sage_pv_work *w;

	if (!current->mm)
		return;

	pr_info_ratelimited("sage_prop_view: queue uid=%u\n", current_uid().val);
	w = kzalloc(sizeof(*w), GFP_KERNEL);
	if (!w)
		return;
	init_task_work(&w->cb, sage_pv_task_work);
	if (task_work_add(current, &w->cb, TWA_RESUME))
		kfree(w);
}
