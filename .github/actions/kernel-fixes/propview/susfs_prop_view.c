// SPDX-License-Identifier: GPL-2.0
/*
 * prop_view: give an app process its own Android property-area view.
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
 *     copy:  <PROPVIEW_DIR>/<ctx>        (same basename, same 128 KB length)
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

#include <linux/propview.h>

/* Neutral copy directory, shared with the userspace stager that builds it (the Zygisk cloak's
 * PV_DIR). Keep the two in sync; a kernel built without the matching module finds no copies and
 * leaves prop_view inert. */
#define PROPVIEW_DIR "/data/misc/propview"
#define PROPVIEW_AREAS_PARENT "__properties__"
#define PROPVIEW_COPY_PARENT "propview"
#define PROPVIEW_HITS 128

/* Copies are named by a hash of the live area's basename, not by the SELinux context name. An app
 * can read its own map line in /proc/self/maps, and some context names are tells themselves
 * (qemu_sf_lcd_density_prop, vendor_qemu_prop). The stager (sage-propview-omit -hash-names) and the
 * open-redirect module must use the same algorithm: FNV-1a 32-bit, lowercase %08x. */
static u32 pv_hash(const char *s, size_t n)
{
	u32 h = 2166136261u;

	while (n--) {
		h ^= (u8)*s++;
		h *= 16777619u;
	}
	return h;
}

struct pv_hit {
	unsigned long start;
	unsigned long len;
	char name[NAME_MAX + 1];
};

struct pv_work {
	struct callback_head cb;
};

/* The property areas live directly under /dev/__properties__; so do the copies under our
 * directory.  Comparing the parent dentry's name avoids building paths on every VMA.  The
 * VMA holds a reference to vm_file, so the dentry cannot go away underneath us; a rename of
 * these directories is not something that happens at runtime. */
static bool pv_parent_is(const struct dentry *d, const char *name)
{
	return d && d->d_parent && strcmp(d->d_parent->d_name.name, name) == 0;
}

static void pv_task_work(struct callback_head *cb)
{
	struct pv_work *w = container_of(cb, struct pv_work, cb);
	struct mm_struct *mm = current->mm;
	struct pv_hit *hits;
	const struct cred *root;
	char *path;
	struct vm_area_struct *vma;
	int n = 0, i;

	kfree(w);
	if (!mm)
		return;
	/* The syscall entry runs before the creds change, so the app uid is only visible here,
	 * after the kernel applied it and before returning to userspace. */
	if (current_uid().val < 10000)
		return;

	/* Heap, not stack: 16 hit slots with a NAME_MAX name each plus PATH_MAX would blow
	 * the 2 KB kernel stack-frame limit under -Wframe-larger-than. */
	hits = kzalloc(sizeof(*hits) * PROPVIEW_HITS, GFP_KERNEL);
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
		if (pv_parent_is(d, PROPVIEW_COPY_PARENT))
			continue;
		if (!pv_parent_is(d, PROPVIEW_AREAS_PARENT))
			continue;
		if (strncmp(d->d_name.name, "u:object_r:", 11) != 0)
			continue;

		len = strlen(d->d_name.name);
		if (len == 0 || len > NAME_MAX)
			continue;
		if (n >= PROPVIEW_HITS)
			break;

		hits[n].start = vma->vm_start;
		hits[n].len = vma->vm_end - vma->vm_start;
		memcpy(hits[n].name, d->d_name.name, len);
		hits[n].name[len] = '\0';
		n++;
	}
	mmap_read_unlock(mm);

	/* init creds for the open and the mmap. get_task_cred() is a refcount bump on init_task's
	 * creds, so it cannot fail (prepare_kernel_cred allocates, and returned NULL here). */
	root = get_task_cred(&init_task);

	for (i = 0; i < n; i++) {
		struct file *f;
		const struct cred *saved;
		unsigned long addr;

		snprintf(path, PATH_MAX, PROPVIEW_DIR "/%08x",
			 pv_hash(hits[i].name, strlen(hits[i].name)));

		saved = override_creds(root);
		f = filp_open(path, O_RDONLY | O_LARGEFILE, 0);
		revert_creds(saved);

		if (IS_ERR(f)) {
			pr_warn_ratelimited("propview: open %s failed %ld\n",
					    path, (long)PTR_ERR(f));
			continue;
		}
		if ((unsigned long)i_size_read(file_inode(f)) != hits[i].len) {
			pr_warn_ratelimited("propview: %s is %lld bytes, VMA is %lu\n",
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
			pr_warn_ratelimited("propview: vm_mmap %s at %#lx failed %ld\n",
					    path, hits[i].start, (long)addr);
		fput(f);
	}

	put_cred(root);
	kfree(path);
	kfree(hits);
}

void propview_queue(void)
{
	struct pv_work *w;

	if (!current->mm)
		return;

	w = kzalloc(sizeof(*w), GFP_KERNEL);
	if (!w)
		return;
	init_task_work(&w->cb, pv_task_work);
	if (task_work_add(current, &w->cb, TWA_RESUME))
		kfree(w);
}

/*
 * Late mappings: bionic maps each property area on first use, and an app typically touches several
 * areas only after specialization. The setresuid task_work cannot see those, so the app would keep
 * reading the live area for them. ksys_mmap_pgoff calls this right after a successful mmap with the
 * file the app mapped; if it is a live property area with a staged copy, remap that exact range
 * onto the copy (MAP_FIXED), which is the same substitution the task_work performs.
 *
 * Runs in the app's own syscall context before returning to userspace, so there is no window in
 * which the app can read the live area. Non-app uids are skipped: the copy is only ever mapped for
 * uid >= 10000 (services must keep the real values -- ro.boot.qemu gates the USB gadget path).
 */
void propview_after_mmap(struct file *file, unsigned long addr, unsigned long len)
{
	const struct cred *root, *saved;
	struct file *copy;
	unsigned long got;
	const char *name;
	struct dentry *d;
	char *path;

	if (!file || !len || (long)addr < 0)
		return;
	if (current_uid().val < 10000)
		return;
	d = file->f_path.dentry;
	if (!pv_parent_is(d, PROPVIEW_AREAS_PARENT))
		return;
	name = d->d_name.name;
	if (strncmp(name, "u:object_r:", 11) != 0 || strlen(name) > NAME_MAX)
		return;

	path = kmalloc(PATH_MAX, GFP_KERNEL);
	if (!path)
		return;
	snprintf(path, PATH_MAX, PROPVIEW_DIR "/%08x", pv_hash(name, strlen(name)));

	root = get_task_cred(&init_task);
	saved = override_creds(root);
	copy = filp_open(path, O_RDONLY | O_LARGEFILE, 0);
	revert_creds(saved);

	/* No staged copy is the normal case for most contexts: leave the mapping alone. */
	if (IS_ERR(copy))
		goto out_cred;
	if ((unsigned long)i_size_read(file_inode(copy)) != len) {
		pr_warn_ratelimited("propview: %s is %lld bytes, VMA is %lu\n",
				    path, (long long)i_size_read(file_inode(copy)), len);
		fput(copy);
		goto out_cred;
	}

	saved = override_creds(root);
	got = vm_mmap(copy, addr, len, PROT_READ, MAP_SHARED | MAP_FIXED, 0);
	revert_creds(saved);
	if (got != addr)
		pr_warn_ratelimited("propview: late vm_mmap %s at %#lx failed %ld\n",
				    path, addr, (long)got);
	fput(copy);

out_cred:
	put_cred(root);
	kfree(path);
}
