// SPDX-License-Identifier: GPL-2.0
/*
 * susfs_priority.c - SUSFS 2.4.0 Priority UID filter: hide selected app UIDs from
 * getpriority, setpriority, ioprio_get, and ioprio_set syscalls when called by
 * untrusted processes (current_uid() >= 10000).
 *
 * Implements CMD_SUSFS_SET_PRIORITY_UID_FILTER (0x60030) and susfs_parse_priority_app_strings()
 * according to the SUSFS 2.4.0 specification and FinaSU contract.
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/kthread.h>
#include <linux/wait.h>
#include <linux/rcupdate.h>
#include <linux/bitmap.h>
#include <linux/cred.h>
#include <linux/uaccess.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/resource.h>
#include <linux/ioprio.h>
#include <linux/syscalls.h>
#include <linux/sched.h>
#include <linux/ptrace.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>

#include "susfs.h"
#include "susfs_abi.h"
#include "susfs_log.h"
#include "susfs_fp_hook.h"
#include "symbol_resolver.h"
#include "ksu_allow_gate.h"

#define SUSFS_COMPAT_NR_GETPRIORITY	96
#define SUSFS_COMPAT_NR_SETPRIORITY	97
#define SUSFS_COMPAT_NR_IOPRIO_SET	314
#define SUSFS_COMPAT_NR_IOPRIO_GET	315

#ifndef __NR_ioprio_set
#define __NR_ioprio_set			30
#endif
#ifndef __NR_ioprio_get
#define __NR_ioprio_get			31
#endif
#ifndef __NR_setpriority
#define __NR_setpriority		140
#endif
#ifndef __NR_getpriority
#define __NR_getpriority		141
#endif

struct susfs_priority_uid_filter_bitmap {
	struct rcu_head		rcu;
	unsigned long		bits[SUSFS_PRIORITY_UID_FILTER_BITMAP_LEN];
};

static struct susfs_priority_uid_filter_bitmap __rcu *priority_uid_filter_bitmap;
static DEFINE_MUTEX(susfs_mutex_lock_priority_uid_filter);
static char *susfs_priority_app_strings = NULL;
static atomic_t susfs_priority_app_strings_running = ATOMIC_INIT(0);
static DECLARE_WAIT_QUEUE_HEAD(susfs_priority_worker_wait);
static bool susfs_priority_stopping;
static int priority_hooks_armed;

/* Resolved dynamically from kernel/KernelSU if available */
static void (*fn_setup_selinux)(const char *domain, struct cred *cred);
static bool (*fn_is_current_ksu_domain)(void);

static bool susfs_is_valid_app_name(const char *app_name)
{
	const char *p = app_name;
	size_t len = strlen(app_name);

	if (len == 0 || len > 255)
		return false;

	if (!strcmp(app_name, ".") || !strcmp(app_name, ".."))
		return false;

	for (; *p != '\0'; p++) {
		if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
		    (*p >= '0' && *p <= '9') || *p == '.' || *p == '_')
			continue;
		return false;
	}

	return true;
}

/* Must NOT check CFI or inline: calls runtime-resolved kernel pointers fn_setup_selinux / fn_is_current_ksu_domain. */
static __nocfi noinline struct cred *susfs_switch_to_ksu_domain(void)
{
	struct cred *orig_cred = NULL;
	struct cred *cred = NULL;

	orig_cred = prepare_creds();
	if (!orig_cred) {
		SUSFS_LOGE("failed to prepare creds!\n");
		return NULL;
	}

	cred = prepare_creds();
	if (!cred) {
		SUSFS_LOGE("failed to prepare creds!\n");
		abort_creds(orig_cred);
		return NULL;
	}

	if (fn_setup_selinux) {
		fn_setup_selinux("u:r:ksu:s0", cred);
	}

	commit_creds(cred);

	if (fn_is_current_ksu_domain && !fn_is_current_ksu_domain()) {
		SUSFS_LOGE("domain is not ksu, restoring the original creds\n");
		commit_creds(orig_cred);
		return NULL;
	}

	return orig_cred;
}

static void susfs_restore_creds(struct cred *orig_cred)
{
	if (!orig_cred)
		return;
	commit_creds(orig_cred);
}

static int susfs_resolve_priority_app_uid(const char *app_name, uid_t *uid)
{
	struct file *fp;
	char *buf = NULL;
	char *line, *tmp, *p;
	char *pkg_name, *uid_str;
	loff_t pos = 0;
	size_t size;
	unsigned long val;
	int ret = -ENOENT;

	if (!app_name || !*app_name || !uid)
		return -EINVAL;

	if (!susfs_is_valid_app_name(app_name))
		return -EINVAL;

	fp = filp_open("/data/system/packages.list", O_RDONLY | O_NOATIME, 0);
	if (IS_ERR(fp)) {
		ret = PTR_ERR(fp);
		SUSFS_LOGE("failed to open /data/system/packages.list: %d\n", ret);
		return ret;
	}

	size = i_size_read(file_inode(fp));
	if (size <= 0 || size > (4 * 1024 * 1024)) {
		filp_close(fp, NULL);
		return -EFBIG;
	}

	buf = vmalloc(size + 1);
	if (!buf) {
		filp_close(fp, NULL);
		return -ENOMEM;
	}
	buf[size] = '\0';

	ret = kernel_read(fp, buf, size, &pos);
	filp_close(fp, NULL);
	if (ret != (int)size) {
		vfree(buf);
		return -EIO;
	}

	line = buf;
	while (line && *line) {
		p = strchr(line, '\n');
		if (p) {
			*p = '\0';
			tmp = p + 1;
		} else {
			tmp = NULL;
		}

		if (*line == '\0' || *line == '\r') {
			line = tmp;
			continue;
		}

		pkg_name = strsep(&line, " \t");
		if (!pkg_name || *pkg_name == '\0') {
			line = tmp;
			continue;
		}

		uid_str = strsep(&line, " \t");
		if (!uid_str || *uid_str == '\0') {
			line = tmp;
			continue;
		}

		if (!strcmp(pkg_name, app_name)) {
			if (kstrtoul(uid_str, 10, &val)) {
				vfree(buf);
				return -EINVAL;
			}

			*uid = (uid_t)val % 100000;
			vfree(buf);

			if (*uid < SUSFS_PRIORITY_UID_FILTER_MIN_UID ||
			    *uid > SUSFS_PRIORITY_UID_FILTER_MAX_UID)
				return -EINVAL;

			return 0;
		}

		line = tmp;
	}

	vfree(buf);
	return -ENOENT;
}

static int susfs_parse_priority_app_names(char *app_str, unsigned long *bits, bool skip_failed)
{
	char *token = NULL;
	char *saveptr = app_str;
	uid_t uid = 0;
	int ret;

	while ((token = strsep(&saveptr, ",")) != NULL) {
		token = strim(token);
		if (*token == '\0')
			continue;

		if (!susfs_is_valid_app_name(token)) {
			SUSFS_LOGE("invalid app name '%s'\n", token);
			return -EINVAL;
		}

		ret = susfs_resolve_priority_app_uid(token, &uid);
		if (ret) {
			if (skip_failed)
				continue;
			return ret;
		}

		if (uid < SUSFS_PRIORITY_UID_FILTER_MIN_UID || uid > SUSFS_PRIORITY_UID_FILTER_MAX_UID) {
			SUSFS_LOGE("the uid %u of the app '%s' is out of range [%d, %d]\n",
				   uid, token, SUSFS_PRIORITY_UID_FILTER_MIN_UID,
				   SUSFS_PRIORITY_UID_FILTER_MAX_UID);
			if (!skip_failed)
				return -EINVAL;
			continue;
		}

		set_bit(uid - SUSFS_PRIORITY_UID_FILTER_MIN_UID, bits);
		SUSFS_LOGI("app_name '%s', uid '%u' added to priority filter\n", token, uid);
	}

	return 0;
}

void susfs_set_priority_uid_filter(void __user **user_info)
{
	struct st_susfs_priority_uid_filter *info = NULL;
	struct susfs_priority_uid_filter_bitmap *new_bitmap = NULL;
	struct susfs_priority_uid_filter_bitmap *old_bitmap = NULL;
	char *new_app_strings = NULL;
	struct cred *orig_cred = NULL;
	size_t app_str_len = 0;
	int err = 0;

	info = kvmalloc(sizeof(*info), GFP_KERNEL | __GFP_ZERO);
	if (!info) {
		err = -ENOMEM;
		goto out_copy_to_user;
	}

	if (copy_from_user(info, (struct st_susfs_priority_uid_filter __user *)*user_info, sizeof(*info))) {
		err = -EFAULT;
		goto out_copy_to_user;
	}

	if (!memchr(info->app_strings, '\0', sizeof(info->app_strings))) {
		SUSFS_LOGE("the app names string is not null terminated\n");
		err = -EINVAL;
		goto out_copy_to_user;
	}

	if (*info->app_strings == '\0') {
		SUSFS_LOGE("the app names string cannot be empty\n");
		err = -EINVAL;
		goto out_copy_to_user;
	}

	app_str_len = strlen(info->app_strings);
	new_app_strings = kvmalloc(app_str_len + 1, GFP_KERNEL | __GFP_ZERO);
	if (!new_app_strings) {
		err = -ENOMEM;
		goto out_copy_to_user;
	}
	memcpy(new_app_strings, info->app_strings, app_str_len + 1);

	new_bitmap = kzalloc(sizeof(*new_bitmap), GFP_KERNEL);
	if (!new_bitmap) {
		err = -ENOMEM;
		goto out_free_new_app_strings;
	}

	orig_cred = susfs_switch_to_ksu_domain();
	if (!orig_cred) {
		err = -EINVAL;
		goto out_free_new_bitmap;
	}

	err = susfs_parse_priority_app_names(info->app_strings, new_bitmap->bits, true);
	susfs_restore_creds(orig_cred);
	orig_cred = NULL;

	if (err)
		goto out_free_new_bitmap;

	mutex_lock(&susfs_mutex_lock_priority_uid_filter);
	old_bitmap = rcu_dereference_protected(priority_uid_filter_bitmap,
					       lockdep_is_held(&susfs_mutex_lock_priority_uid_filter));
	rcu_assign_pointer(priority_uid_filter_bitmap, new_bitmap);
	new_bitmap = NULL;
	kvfree(susfs_priority_app_strings);
	susfs_priority_app_strings = new_app_strings;
	new_app_strings = NULL;
	mutex_unlock(&susfs_mutex_lock_priority_uid_filter);

	synchronize_rcu();
	if (old_bitmap)
		kfree_rcu(old_bitmap, rcu);

	SUSFS_LOGI("the priority uid filter is set\n");
	err = 0;

out_free_new_bitmap:
	if (new_bitmap)
		kfree(new_bitmap);
out_free_new_app_strings:
	if (new_app_strings)
		kvfree(new_app_strings);
out_copy_to_user:
	if (copy_to_user(&((struct st_susfs_priority_uid_filter __user *)*user_info)->err, &err, sizeof(err)))
		err = -EFAULT;
	SUSFS_LOGI("CMD_SUSFS_SET_PRIORITY_UID_FILTER -> ret: %d\n", err);
	if (info)
		kvfree(info);
}

static int susfs_priority_app_strings_worker(void *data)
{
	struct susfs_priority_uid_filter_bitmap *new_bitmap = NULL;
	struct susfs_priority_uid_filter_bitmap *old_bitmap = NULL;
	struct cred *orig_cred = NULL;
	char *app_strings = NULL;
	size_t app_str_len = 0;
	int ret = 0;

	orig_cred = susfs_switch_to_ksu_domain();
	if (!orig_cred)
		goto out;

	mutex_lock(&susfs_mutex_lock_priority_uid_filter);
	if (susfs_priority_app_strings) {
		app_str_len = strlen(susfs_priority_app_strings);
		app_strings = kvmalloc(app_str_len + 1, GFP_KERNEL | __GFP_ZERO);
		if (app_strings)
			memcpy(app_strings, susfs_priority_app_strings, app_str_len + 1);
	}
	mutex_unlock(&susfs_mutex_lock_priority_uid_filter);

	if (!app_strings)
		goto out_restore_creds;

	new_bitmap = kzalloc(sizeof(*new_bitmap), GFP_KERNEL);
	if (!new_bitmap)
		goto out_restore_creds;

	ret = susfs_parse_priority_app_names(app_strings, new_bitmap->bits, true);
	if (ret)
		goto out_restore_creds;

	mutex_lock(&susfs_mutex_lock_priority_uid_filter);
	old_bitmap = rcu_dereference_protected(priority_uid_filter_bitmap,
					       lockdep_is_held(&susfs_mutex_lock_priority_uid_filter));
	rcu_assign_pointer(priority_uid_filter_bitmap, new_bitmap);
	new_bitmap = NULL;
	mutex_unlock(&susfs_mutex_lock_priority_uid_filter);

	synchronize_rcu();
	if (old_bitmap)
		kfree_rcu(old_bitmap, rcu);

	SUSFS_LOGI("the uids of the priority uid filter are resolved again\n");

out_restore_creds:
	susfs_restore_creds(orig_cred);
	orig_cred = NULL;
out:
	if (new_bitmap)
		kfree(new_bitmap);
	if (app_strings)
		kvfree(app_strings);
	atomic_set(&susfs_priority_app_strings_running, 0);
	wake_up_all(&susfs_priority_worker_wait);

	return ret;
}

void susfs_parse_priority_app_strings(void)
{
	if (READ_ONCE(susfs_priority_stopping))
		return;
	if (atomic_cmpxchg(&susfs_priority_app_strings_running, 0, 1))
		return;
	if (READ_ONCE(susfs_priority_stopping)) {
		atomic_set(&susfs_priority_app_strings_running, 0);
		wake_up_all(&susfs_priority_worker_wait);
		return;
	}

	if (IS_ERR(kthread_run(susfs_priority_app_strings_worker, NULL, "susfs_gpsp_appstr"))) {
		atomic_set(&susfs_priority_app_strings_running, 0);
		wake_up_all(&susfs_priority_worker_wait);
		SUSFS_LOGE("failed to create thread susfs_priority_app_strings\n");
	}
}
EXPORT_SYMBOL_GPL(susfs_parse_priority_app_strings);

bool susfs_is_priority_uid_filtered(uid_t uid)
{
	struct susfs_priority_uid_filter_bitmap *bitmap;
	uid_t app_id = uid % 100000;
	bool filtered = false;

	if (app_id < SUSFS_PRIORITY_UID_FILTER_MIN_UID ||
	    app_id > SUSFS_PRIORITY_UID_FILTER_MAX_UID)
		return false;

	rcu_read_lock();
	bitmap = rcu_dereference(priority_uid_filter_bitmap);
	if (likely(bitmap)) {
		if (test_bit(app_id - SUSFS_PRIORITY_UID_FILTER_MIN_UID, bitmap->bits))
			filtered = true;
	}
	rcu_read_unlock();

	return filtered;
}
EXPORT_SYMBOL_GPL(susfs_is_priority_uid_filtered);

/* Syscall wrappers via sys_call_table interception (susfs_fp_hook) */
static inline bool susfs_priority_should_hide(int which, int who)
{
	if (which == 2 /* PRIO_USER / IOPRIO_WHO_USER */ && current_uid().val >= 10000) {
		uid_t target_uid = who ? (uid_t)who : current_uid().val;
		return susfs_is_priority_uid_filtered(target_uid);
	}
	return false;
}

static susfs_syscall_fn_t priority_orig_getpriority;
static susfs_syscall_fn_t priority_orig_setpriority;
static susfs_syscall_fn_t priority_orig_ioprio_get;
static susfs_syscall_fn_t priority_orig_ioprio_set;

static susfs_syscall_fn_t priority_orig_compat_getpriority;
static susfs_syscall_fn_t priority_orig_compat_setpriority;
static susfs_syscall_fn_t priority_orig_compat_ioprio_get;
static susfs_syscall_fn_t priority_orig_compat_ioprio_set;

static __nocfi long priority_wrap_getpriority(const struct pt_regs *regs)
{
	susfs_syscall_fn_t orig = READ_ONCE(priority_orig_getpriority);

	if (!orig)
		return -ENOSYS;
	if (susfs_priority_should_hide((int)regs->regs[0], (int)regs->regs[1]))
		return -ESRCH;
	return orig(regs);
}

static __nocfi long priority_wrap_setpriority(const struct pt_regs *regs)
{
	susfs_syscall_fn_t orig = READ_ONCE(priority_orig_setpriority);

	if (!orig)
		return -ENOSYS;
	if (susfs_priority_should_hide((int)regs->regs[0], (int)regs->regs[1]))
		return -ESRCH;
	return orig(regs);
}

static __nocfi long priority_wrap_ioprio_get(const struct pt_regs *regs)
{
	susfs_syscall_fn_t orig = READ_ONCE(priority_orig_ioprio_get);

	if (!orig)
		return -ENOSYS;
	if (susfs_priority_should_hide((int)regs->regs[0], (int)regs->regs[1]))
		return -ESRCH;
	return orig(regs);
}

static __nocfi long priority_wrap_ioprio_set(const struct pt_regs *regs)
{
	susfs_syscall_fn_t orig = READ_ONCE(priority_orig_ioprio_set);

	if (!orig)
		return -ENOSYS;
	if (susfs_priority_should_hide((int)regs->regs[0], (int)regs->regs[1]))
		return -ESRCH;
	return orig(regs);
}

static __nocfi long priority_wrap_compat_getpriority(const struct pt_regs *regs)
{
	susfs_syscall_fn_t orig = READ_ONCE(priority_orig_compat_getpriority);

	if (!orig)
		return -ENOSYS;
	if (susfs_priority_should_hide((int)regs->regs[0], (int)regs->regs[1]))
		return -ESRCH;
	return orig(regs);
}

static __nocfi long priority_wrap_compat_setpriority(const struct pt_regs *regs)
{
	susfs_syscall_fn_t orig = READ_ONCE(priority_orig_compat_setpriority);

	if (!orig)
		return -ENOSYS;
	if (susfs_priority_should_hide((int)regs->regs[0], (int)regs->regs[1]))
		return -ESRCH;
	return orig(regs);
}

static __nocfi long priority_wrap_compat_ioprio_get(const struct pt_regs *regs)
{
	susfs_syscall_fn_t orig = READ_ONCE(priority_orig_compat_ioprio_get);

	if (!orig)
		return -ENOSYS;
	if (susfs_priority_should_hide((int)regs->regs[0], (int)regs->regs[1]))
		return -ESRCH;
	return orig(regs);
}

static __nocfi long priority_wrap_compat_ioprio_set(const struct pt_regs *regs)
{
	susfs_syscall_fn_t orig = READ_ONCE(priority_orig_compat_ioprio_set);

	if (!orig)
		return -ENOSYS;
	if (susfs_priority_should_hide((int)regs->regs[0], (int)regs->regs[1]))
		return -ESRCH;
	return orig(regs);
}

static struct susfs_fp_hook priority_hooks[] = {
	{
		.nr		= __NR_getpriority,
		.name		= "priority/getpriority",
		.sym		= "__arm64_sys_getpriority",
		.wrapper	= priority_wrap_getpriority,
		.orig_slot	= &priority_orig_getpriority,
	},
	{
		.nr		= __NR_setpriority,
		.name		= "priority/setpriority",
		.sym		= "__arm64_sys_setpriority",
		.wrapper	= priority_wrap_setpriority,
		.orig_slot	= &priority_orig_setpriority,
	},
	{
		.nr		= __NR_ioprio_get,
		.name		= "priority/ioprio_get",
		.sym		= "__arm64_sys_ioprio_get",
		.wrapper	= priority_wrap_ioprio_get,
		.orig_slot	= &priority_orig_ioprio_get,
	},
	{
		.nr		= __NR_ioprio_set,
		.name		= "priority/ioprio_set",
		.sym		= "__arm64_sys_ioprio_set",
		.wrapper	= priority_wrap_ioprio_set,
		.orig_slot	= &priority_orig_ioprio_set,
	},
	{
		.nr		= -1,
		.nr_compat	= SUSFS_COMPAT_NR_GETPRIORITY,
		.name		= "priority/compat getpriority",
		.sym		= "__arm64_compat_sys_getpriority",
		.wrapper	= priority_wrap_compat_getpriority,
		.orig_slot_compat = &priority_orig_compat_getpriority,
	},
	{
		.nr		= -1,
		.nr_compat	= SUSFS_COMPAT_NR_SETPRIORITY,
		.name		= "priority/compat setpriority",
		.sym		= "__arm64_compat_sys_setpriority",
		.wrapper	= priority_wrap_compat_setpriority,
		.orig_slot_compat = &priority_orig_compat_setpriority,
	},
	{
		.nr		= -1,
		.nr_compat	= SUSFS_COMPAT_NR_IOPRIO_GET,
		.name		= "priority/compat ioprio_get",
		.sym		= "__arm64_compat_sys_ioprio_get",
		.wrapper	= priority_wrap_compat_ioprio_get,
		.orig_slot_compat = &priority_orig_compat_ioprio_get,
	},
	{
		.nr		= -1,
		.nr_compat	= SUSFS_COMPAT_NR_IOPRIO_SET,
		.name		= "priority/compat ioprio_set",
		.sym		= "__arm64_compat_sys_ioprio_set",
		.wrapper	= priority_wrap_compat_ioprio_set,
		.orig_slot_compat = &priority_orig_compat_ioprio_set,
	},
};

bool susfs_priority_active(void)
{
	return READ_ONCE(priority_hooks_armed) > 0;
}

/* Diagnostic /proc node */
static int priority_proc_show(struct seq_file *m, void *v)
{
	struct susfs_priority_uid_filter_bitmap *bitmap;
	int i, count = 0;

	seq_printf(m, "priority_uid_filter: %s\n",
		   susfs_priority_active() ? "active" : "inactive");

	mutex_lock(&susfs_mutex_lock_priority_uid_filter);
	seq_printf(m, "stored_packages: %s\n",
		   susfs_priority_app_strings ? susfs_priority_app_strings : "(none)");
	mutex_unlock(&susfs_mutex_lock_priority_uid_filter);

	rcu_read_lock();
	bitmap = rcu_dereference(priority_uid_filter_bitmap);
	if (bitmap) {
		for (i = 0; i < SUSFS_PRIORITY_UID_FILTER_UID_NUM; i++) {
			if (test_bit(i, bitmap->bits))
				count++;
		}
	}
	rcu_read_unlock();

	seq_printf(m, "filtered_uids_count: %d\n", count);
	return 0;
}

static int priority_proc_open(struct inode *inode, struct file *file)
{
	if (!susfs_control_visible())
		return -ENOENT;
	return single_open(file, priority_proc_show, NULL);
}

static const struct proc_ops priority_proc_ops = {
	.proc_open    = priority_proc_open,
	.proc_read    = seq_read,
	.proc_lseek   = seq_lseek,
	.proc_release = single_release,
};

static struct proc_dir_entry *priority_proc_entry;

int susfs_priority_init(void)
{
	int i, err = 0;

	WRITE_ONCE(susfs_priority_stopping, false);
	priority_hooks_armed = 0;

	fn_setup_selinux = (void *)find_kernel_symbol_exact_any("setup_selinux");
	fn_is_current_ksu_domain = (void *)find_kernel_symbol_exact_any("susfs_is_current_ksu_domain");

	for (i = 0; i < ARRAY_SIZE(priority_hooks); i++) {
		err = susfs_fp_install(&priority_hooks[i]);
		if (err) {
			pr_warn("priority: failed to install hook %s: %d\n",
				priority_hooks[i].name, err);
			continue;
		}
		priority_hooks_armed++;
	}

	if (!priority_hooks_armed) {
		pr_warn("priority: none of the %u hooks installed - priority uid filter disabled\n",
			(unsigned int)ARRAY_SIZE(priority_hooks));
		return -EOPNOTSUPP;
	}

	if (susfs_control_node_allowed()) {
		priority_proc_entry = proc_create("susfs_priority", 0777, NULL, &priority_proc_ops);
		if (!priority_proc_entry)
			pr_warn("priority: proc_create(susfs_priority) failed\n");
	}

	SUSFS_LOGI("priority uid filter initialized (%d/%u hooks installed)\n",
		   priority_hooks_armed, (unsigned int)ARRAY_SIZE(priority_hooks));
	return 0;
}

void susfs_priority_exit(void)
{
	struct susfs_priority_uid_filter_bitmap *bitmap;
	int i;

	WRITE_ONCE(susfs_priority_stopping, true);
	wait_event(susfs_priority_worker_wait, !atomic_read(&susfs_priority_app_strings_running));

	if (priority_proc_entry) {
		proc_remove(priority_proc_entry);
		priority_proc_entry = NULL;
	}

	for (i = ARRAY_SIZE(priority_hooks) - 1; i >= 0; i--) {
		if (priority_hooks[i].installed)
			susfs_fp_remove(&priority_hooks[i]);
	}

	if (priority_hooks_armed) {
		susfs_fp_drain();
		priority_hooks_armed = 0;
	}

	mutex_lock(&susfs_mutex_lock_priority_uid_filter);
	bitmap = rcu_dereference_protected(priority_uid_filter_bitmap,
					   lockdep_is_held(&susfs_mutex_lock_priority_uid_filter));
	rcu_assign_pointer(priority_uid_filter_bitmap, NULL);
	kvfree(susfs_priority_app_strings);
	susfs_priority_app_strings = NULL;
	mutex_unlock(&susfs_mutex_lock_priority_uid_filter);

	synchronize_rcu();
	if (bitmap)
		kfree_rcu(bitmap, rcu);

	SUSFS_LOGI("priority uid filter removed\n");
}
