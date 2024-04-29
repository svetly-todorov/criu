#include <linux/filter.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <ptrace.h>

#include "common/config.h"
#include "imgset.h"
#include "kcmp.h"
#include "pstree.h"
#include <compel/ptrace.h>
#include "proc_parse.h"
#include "restorer.h"
#include "sud.h"
#include "servicefd.h"
#include "util.h"
#include "rst-malloc.h"

#include "protobuf.h"
#include "images/sud.pb-c.h"

#undef LOG_PREFIX
#define LOG_PREFIX "sys-dispatch: "

static struct rb_root sud_tid_rb_root = RB_ROOT;
static struct sys_dispatch_entry *sud_tid_entry_root;

/* Autogenerated protobuf message structure */
static SysDispatchEntry *sud_img_entry;

struct sys_dispatch_entry *sud_lookup(pid_t tid_real, bool create, bool mandatory)
{
	struct sys_dispatch_entry *entry = NULL;

	struct rb_node *node = sud_tid_rb_root.rb_node;
	struct rb_node **new = &sud_tid_rb_root.rb_node;
	struct rb_node *parent = NULL;

	while (node) {
		struct sys_dispatch_entry *this = rb_entry(node, struct sys_dispatch_entry, node);

		parent = *new;
		if (tid_real < this->tid_real)
			node = node->rb_left, new = &((*new)->rb_left);
		else if (tid_real > this->tid_real)
			node = node->rb_right, new = &((*new)->rb_right);
		else
			return this;
	}

	if (create) {
		entry = xzalloc(sizeof(*entry));
		if (!entry)
			return NULL;
		rb_init_node(&entry->node);
		entry->tid_real = tid_real;

		entry->next = sud_tid_entry_root, sud_tid_entry_root = entry;
		rb_link_and_balance(&sud_tid_rb_root, &entry->node, parent, new);
	} else {
		if (mandatory)
			pr_err("Can't find entry on tid_real %d\n", tid_real);
	}

	return entry;
}

/* This must run after ptrace freeze but *before* parasite code. */
int sud_collect_entry(pid_t tid_real)
{
    struct sys_dispatch_entry *entry;
    sud_config_t config;

	entry = sud_lookup(tid_real, true, false);
	if (!entry) {
		pr_err("Can't create entry on tid_real %d\n", tid_real);
		return -1;
	}

    if (ptrace_get_sud(tid_real, &config)) {
        pr_err("Failed to get SUD settings for %d", tid_real);
        return -1;
    }

    entry->mode = config.mode;
    entry->selector = config.selector;
    entry->offset = config.offset;
    entry->len = config.len;

	pr_debug("Collected tid_real %d, SUD mode %lx\n", tid_real, config.mode);
	return 0;
}

int dump_sud_per_core(pid_t tid_real, ThreadCoreEntry *tc)
{
	struct sys_dispatch_entry *entry = sud_find_entry(tid_real);
	if (!entry) {
		pr_err("Can't dump thread core on tid_real %d\n", tid_real);
		return -1;
	}

	if (entry->mode == SYS_DISPATCH_ON) {
		tc->has_sud_mode = true;
		tc->sud_mode = entry->mode;
		tc->has_sud_setting = true;
		tc->sud_setting = entry->img_setting_pos;
	}

	return 0;
}

/* Traverse the nodes from collect_entry, and write data to protobuf */
int dump_sud(void)
{
	SysDispatchEntry se = SYS_DISPATCH_ENTRY__INIT;
	SysDispatchSetting *settings;
	struct sys_dispatch_entry *entry;
	size_t img_setting_pos = 0, nr_settings = 0, i;
	struct rb_node *node;
	int ret;

	/* Get the number of entries in the collect ring buffer */
	for (node = rb_first(&sud_tid_rb_root); node; node = rb_next(node)) {
		entry = rb_entry(node, struct sys_dispatch_entry, node);
		nr_settings += 1;
	}

	/* Allocate space for dumping the thread settings */
	se.n_settings = nr_settings;
	if (nr_settings) {
		/* Allocate the list of pointers to settings */
		se.settings = xmalloc(sizeof(*se.settings) * nr_settings);
		if (!se.settings)
			return -1;
		/* Allocate the settings themselves */
		settings = xmalloc(sizeof(*settings) * nr_settings);
		if (!settings) {
			xfree(se.settings);
			return -1;
		}
		/*
		 * Fill the list of pointers with setting addresses,
		 * initializing the protobuf structs on the way
		 */
		for (i = 0; i < nr_settings; ++i) {
			sys_dispatch_setting__init(&settings[i]);
			se.settings[i] = &settings[i];
		}
	}

	/* Traverse again, this time writing the settings to img format */
	for (node = rb_first(&sud_tid_rb_root); node; node = rb_next(node)) {
		entry = rb_entry(node, struct sys_dispatch_entry, node);

		if (entry->mode == SYS_DISPATCH_ON) {
			se.settings[img_setting_pos]->selector = entry->selector;
			se.settings[img_setting_pos]->offset = entry->offset;
			se.settings[img_setting_pos]->len = entry->len;
			entry->img_setting_pos = img_setting_pos++;
		}
	}

	ret = pb_write_one(img_from_set(glob_imgset, CR_FD_SYS_DISPATCH), &se, PB_SYS_DISPATCH);

	/* Once saved, we don't need to keep the SUD entries allocated */
	xfree(se.settings);
	xfree(settings);

	return ret;
}

/*
 * Read SUD settings from the checkpoint image.
 *
 * Since the code here rips from seccomp, all of the settings
 * for a process tree will be stored in one image entry.
 *
 * Compare this to something like file-lock.c:collect_one_file_lock,
 * which reads FileLockEntries sequentially from protobuf and daisy-
 * chains them into file_lock_list.
 */
int sud_read_image(void)
{
	struct cr_img *img;
	int ret;

	img = open_image(CR_FD_SYS_DISPATCH, O_RSTR);
	if (!img)
		return -1;

	ret = pb_read_one_eof(img, &sud_img_entry, PB_SYS_DISPATCH);
	close_image(img);
	if (ret <= 0)
		return 0; /* all threads had SYS_DISPATCH_OFF */

	BUG_ON(!sud_img_entry);

	return 0;
}

/* Fill out task_restore_args with the info it needs to restore SUD */
// int prepare_sud(struct pstree_item *item, struct task_restore_args *ta)
// {
// 	struct thread_restore_args *args_array = (struct thread_restore_args *)(&ta[1]);
// 	size_t i;

// 	for (i = 0; i < item->nr_threads; i++) {
// 		ThreadCoreEntry *thread_core = item->core[i]->thread_core;
// 		struct thread_restore_args *args = &args_array[i];
// 		SysDispatchSetting *ss;

// 		/* This tells CRIU to disable SUD by default */
// 		args->sud_mode = SYS_DISPATCH_OFF;
// 		args->sud_selector = 0;
// 		args->sud_offset = 0;
// 		args->sud_len = 0;

// 		if (thread_core->has_sud_mode)
// 			args->sud_mode = thread_core->sud_mode;

// 		if (args->sud_mode != SYS_DISPATCH_ON)
// 			continue;

// 		if (thread_core->sud_setting >= sud_img_entry->n_settings) {
// 			pr_err("Corrupted sud setting index on tid %d (%u > %zu)\n", item->threads[i].ns[0].virt,
// 			       thread_core->sud_setting, sud_img_entry->n_settings);
// 			return -1;
// 		}

// 		ss = sud_img_entry->settings[thread_core->sud_setting];
// 		args->sud_selector = ss->selector;
// 		args->sud_offset = ss->offset;
// 		args->sud_len = ss->len;
// 	}
// }

/*
 * Restore SUD directly from the image data, to
 * avoid disable/enable shuffling in the restorer blob.
 *
 * I believe this is OK because this is called in
 * cr-restore.c:finalize_restore_detach(), which happens
 * a few lines before close_image_dir(), which closes the
 * image streamer.
 */
int restore_sud_per_core(pid_t tid_real, ThreadCoreEntry *tc)
{
	sud_config_t config;
	SysDispatchSetting *ss;

	if (!tc->has_sud_mode)
		return 0;

	if (tc->sud_mode == SYS_DISPATCH_OFF)
		return 0;

	if (tc->sud_setting >= sud_img_entry->n_settings) {
		pr_err("Corrupted sud setting index on tid %d (%u > %zu)\n", tid_real,
				tc->sud_setting, sud_img_entry->n_settings);
		return -1;
	}

	ss = sud_img_entry->settings[tc->sud_setting];

	config.mode = SYS_DISPATCH_ON;
	config.selector = ss->selector;
	config.offset = ss->offset;
	config.len = ss->len;

	return ptrace_set_sud(tid_real, &config);
}