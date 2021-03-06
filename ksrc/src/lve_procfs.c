#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/fs.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/mutex.h>
#include <linux/time.h>

#include <asm/uaccess.h>

#include "lve_internal.h"
#include "lve_debug.h"
#include "light_ve.h"
#include "resource.h"
#include "lve_kmod_c.h"
#include "lve_os_compat.h"

struct lve_proc {
	/* MUST first to seq_release work correct */
	struct seq_file		lp_m;
	struct lvp_ve_private  *lp_lvp;
};

#define proc_lve(p) (container_of((p), struct lve_proc, lp_m))

/************************ list *********************************/

static void *
list_start(struct seq_file *m, loff_t *pos)
{
	struct list_head *p;
	struct light_ve *ret = NULL;
	loff_t l = *pos;
	struct lve_proc *lp = proc_lve(m);
	struct lvp_ve_private *lvp = lp->lp_lvp;

	if (!l--)
		return SEQ_START_TOKEN;

	read_lock_irq(&lvp->lvp_lock);
	list_for_each(p, &lvp->lvp_lve_list) {
		if (!l--) {
			ret = list_entry(p, struct light_ve, lve_link);
			light_ve_get(ret);
			break;
		}
	}
	read_unlock_irq(&lvp->lvp_lock);

	return ret;
}

#define lve_if_not_last(list, point) \
	((point)->next == list ? NULL :  \
	list_first_entry(point, struct light_ve, lve_link))

static void *
list_next(struct seq_file *m, void *v, loff_t *pos)
{
	struct light_ve *old = v;
	struct light_ve *ret = NULL, *entry;
	struct lve_proc *lp = proc_lve(m);
	struct lvp_ve_private *lvp = lp->lp_lvp;

	read_lock_irq(&lvp->lvp_lock);
	if (v == SEQ_START_TOKEN) {
		ret = lve_if_not_last(&lvp->lvp_lve_list, &lvp->lvp_lve_list);
	} else {
		if (unlikely(old->lve_unlinked)) {
			LVE_DBG("Lve %d is deleted but still in lve_list\n", old->lve_id);
			list_for_each_entry(entry, &lvp->lvp_lve_list, lve_link) {
				if (entry->lve_id > old->lve_id) {
					LVE_DBG("Next lve is %d \n", entry->lve_id);
					ret = entry;
					break;
				}
			}
		} else {
			ret = lve_if_not_last(&lvp->lvp_lve_list, &old->lve_link);
		}
	}
	(*pos)++;

	if (ret)
		light_ve_get(ret);
	read_unlock_irq(&lvp->lvp_lock);
	if (v != SEQ_START_TOKEN)
		light_ve_put(old);

	return ret;
}

static void
list_stop(struct seq_file *m, void *v)
{
	struct light_ve *old = v;

	if (old && v != SEQ_START_TOKEN)
		light_ve_put(old);
}

static void lve_limits_hdr(struct seq_file *m)
{
		seq_printf(m, "\tlCPU\tlCPUW\tnCPU");
		seq_printf(m, "\tlEP\tlNPROC");
		seq_printf(m, "\tlMEM\tlMEMPHY");
		seq_printf(m, "\tlIO\tlIOPS");
		seq_printf(m, "\tlNETO\tlNETI");
}
static void lve_limits_print(struct seq_file *m, lve_limits_t limits)
{
	seq_printf(m, "\t%u\t%u\t%u",
		  limits[LIM_CPU],
		  limits[LIM_CPU_WEIGHT],
		  limits[LIM_CPUS]);

	seq_printf(m, "\t%u\t%u",
		  limits[LIM_ENTER],
		  limits[LIM_NPROC]);

	seq_printf(m, "\t%u\t%u",
		  limits[LIM_MEMORY],
		  limits[LIM_MEMORY_PHY]);

	seq_printf(m, "\t%u\t%u",
		   limits[LIM_IO],
		   limits[LIM_IOPS]);
}

static void lve_stat_show(struct seq_file *m, int head, struct light_ve *lve)
{
	struct lve_usage usage;

	if (head != 0) {
		seq_printf(m, "10:LVE");

		lve_limits_hdr(m);
		/* */
		seq_printf(m, "\tEP");
		seq_printf(m, "\tCPU\tMEM\tIO");
		seq_printf(m, "\tfMEM\tfEP");
		seq_printf(m, "\tMEMPHY\tfMEMPHY\tNPROC\tfNPROC");
		seq_printf(m, "\tIOPS");
		seq_printf(m, "\tNETO\tNETI");
		seq_printf(m, "\n");

		seq_printf(m, "%u,%u\t", lve->lve_lvp->lvp_id, 0);
		lve_limits_print(m, lve->lve_lvp->lvp_def_limits);

		seq_printf(m, "\t"LPU64"\t"LPU64, 0ULL, 0ULL);

		/** Usage */
		seq_printf(m, "\t"LPU64, 0ULL);
		seq_printf(m, "\t"LPU64"\t"LPU64"\t"LPU64, 0ULL, 0ULL, 0ULL);
		seq_printf(m, "\t"LPU64"\t"LPU64, 0ULL, 0ULL);
		seq_printf(m, "\t"LPU64"\t"LPU64, 0ULL, 0ULL);
		seq_printf(m, "\t"LPU64"\t"LPU64, 0ULL, 0ULL);
		seq_printf(m, "\t"LPU64, 0ULL);
		seq_printf(m, "\t"LPU64"\t"LPU64, 0ULL, 0ULL);

		seq_printf(m, "\n");
	}

	lve_resource_usage(lve, &usage);

	/* XXX read long always atomic */
	seq_printf(m, "%u,%u\t", lve->lve_lvp->lvp_id,
		   lve->lve_id);

	lve_limits_print(m, lve->lve_limits);

	seq_printf(m, "\t"LPU64"\t"LPU64,
		(uint64_t)atomic64_read(&lve->lve_net.ln_stats.out_limit),
		(uint64_t)atomic64_read(&lve->lve_net.ln_stats.in_limit));

	/** Usage */
	seq_printf(m, "\t"LPU64,
		  usage.data[RES_ENTER].data);
	seq_printf(m, "\t"LPU64"\t"LPU64"\t"LPU64,
		  usage.data[RES_CPU].data,
		  usage.data[RES_MEM].data,
		  usage.data[RES_IO].data);

	seq_printf(m, "\t"LPU64"\t"LPU64,
		   usage.data[RES_MEM].fail,
		   usage.data[RES_ENTER].fail);

	seq_printf(m, "\t"LPU64"\t"LPU64,
		   usage.data[RES_MEM_PHY].data,
		   usage.data[RES_MEM_PHY].fail);

	seq_printf(m, "\t"LPU64"\t"LPU64,
		   usage.data[RES_NPROC].data,
		   usage.data[RES_NPROC].fail);

	seq_printf(m, "\t"LPU64,
		   usage.data[RES_IOPS].data);

	seq_printf(m, "\t"LPU64"\t"LPU64,
		(uint64_t)atomic64_read(&lve->lve_net.ln_stats.out_total),
		(uint64_t)atomic64_read(&lve->lve_net.ln_stats.in_total));

	seq_printf(m, "\n");
}

static void lve_net_stat_show(struct seq_file *m, struct light_ve *lve)
{
	seq_printf(m, "traf: ");
	lve_net_traf_show(m, lve);
	seq_printf(m, "\n");
	lve_net_port_show(m, lve);
}

static int list_show(struct seq_file *m, void *v)
{
	struct light_ve *lve = v;
	struct lve_proc *lp = proc_lve(m);
	struct lvp_ve_private *lvp = lp->lp_lvp;

	if (v == SEQ_START_TOKEN) {
		/* print selflimit */
		lve = lvp->lvp_default;
	}

	lve_stat_show(m, (v == SEQ_START_TOKEN), lve);

	return 0;
}

static const struct seq_operations lve_list_op = {
	.start	= list_start,
	.next	= list_next,
	.stop	= list_stop,
	.show	= list_show
};

static int lve_list_open(struct inode *inode, struct file *file)
{
	int ret;
	struct lve_proc *p;

	p = kmalloc(GFP_KERNEL, sizeof(*p));
	if (!p)
		return -ENOMEM;

	p->lp_lvp = PDE_DATA(inode);
	file->private_data = p;

	ret = seq_open(file, &lve_list_op);
	if (ret)
		return ret;

	return 0;
}

static const struct file_operations lve_list_fops = {
	.owner		= THIS_MODULE,
	.open           = lve_list_open,
	.read           = seq_read,
	.llseek         = seq_lseek,
	.release        = seq_release,
};
/**************** list end ***************************/

static int usage_show(struct seq_file *m, void *v)
{
	struct light_ve *lve = v;
	struct lve_usage usage;
	struct lve_proc *lp = proc_lve(m);
	struct lvp_ve_private *lvp = lp->lp_lvp;

	if (v == SEQ_START_TOKEN) {
		seq_printf(m, "LAST RESET: %lu\n", lvp->lvp_last_reset);
		seq_printf(m, "LVE\tfMEM\tfEP\tCPU\n");
		return 0;
	}

	lve_resource_usage(lve, &usage);
	seq_printf(m, "%u,%u\t%lu\t%ld\t"LPU64"\n",
		  lve->lve_lvp->lvp_id,
		  lve->lve_id,
		  (unsigned long)0,
		  lve->lve_stats.st_err_enters,
		  usage.data[RES_CPU].data);

	return 0;
}

static const struct seq_operations lve_usage_op = {
	.start	= list_start,
	.next	= list_next,
	.stop	= list_stop,
	.show	= usage_show
};

#define LVE_CLEAR_CMD  "clear"
static ssize_t lve_usage_write(struct file *file, const char __user *data, 
			    size_t count, loff_t *off)
{
	char d[sizeof LVE_CLEAR_CMD + 1];
	struct light_ve *lve;
	struct lve_proc *lp = file->private_data;
	struct lvp_ve_private *lvp = lp->lp_lvp;
	int rc;

	if (!data || count < (sizeof(LVE_CLEAR_CMD) - 1) || *off)
		return -EINVAL;

	memset(d, 0, sizeof d);

	rc = copy_from_user(d, data, (sizeof(LVE_CLEAR_CMD) - 1));
	if (rc)
		return rc;

	if (strcmp(d, LVE_CLEAR_CMD))
		return -ENOSYS;

	read_lock_irq(&lvp->lvp_lock);
	list_for_each_entry(lve, &lvp->lvp_lve_list, lve_link) {
		os_resource_usage_clear(lve_private(lve));
		spin_lock(&lve->lve_stats.enter_lock);
		lve->lve_stats.st_err_enters = 0;
		spin_unlock(&lve->lve_stats.enter_lock);
		/** XXX mem fault reset*/
	}
	read_unlock_irq(&lvp->lvp_lock);

	lvp->lvp_last_reset = get_seconds();

	return count;
}


static int lve_usage_open(struct inode *inode, struct file *file)
{
	int ret;
	struct lve_proc *p;

	p = kmalloc(GFP_KERNEL, sizeof(*p));
	if (!p)
		return -ENOMEM;

	p->lp_lvp = PDE_DATA(inode);
	file->private_data = p;

	ret = seq_open(file, &lve_usage_op);
	if (ret)
		return ret;

	return 0;
}

static const struct file_operations lve_usage_fops = {
	.owner		= THIS_MODULE,
	.open           = lve_usage_open,
	.read           = seq_read,
	.write		= lve_usage_write,
	.llseek         = seq_lseek,
	.release        = seq_release,
};

static const char *facilities[] = {
	[LVE_DEBUG_FAC_DBG]  = "debug",
	[LVE_DEBUG_FAC_WARN] = "warning",
	[LVE_DEBUG_FAC_ERR]  = "error"
};

static ssize_t lve_debug_write(struct file *file, const char __user *data,
				size_t count, loff_t *off)
{
	char str[50], *pstr = str;
	int rc, i;
	unsigned newmask = 0, op = 0;

	if (!data || count > (sizeof(str) - 1) || *off)
		return -EINVAL;

	rc = copy_from_user(str, data, count);
	if (rc)
		return -EFAULT;
	str[count] = '\0';

	/* add subsystems to the list */
	if (str[0] == '+') {
		op = +1;
		pstr++;
	}
	/* remove subsystems from the list */
	if (str[0] == '-') {
		op = -1;
		pstr++;
	}

	while (*pstr) {
		char *lstr;

		while (*pstr == ' ' || *pstr == '\t' || *pstr == '\n')
			pstr++;

		if (!*pstr)
			break;

		lstr = pstr;
		while (*lstr != ' ' && *lstr != '\t' && *lstr != '\n' && *lstr)
			lstr++;

		for (i = 0; i < ARRAY_SIZE(facilities); i++) {
			if (strlen(facilities[i]) == (lstr - pstr) &&
			    !strncmp(facilities[i], pstr, lstr - pstr)) {
				newmask |= (1 << i);
				break;
			}
		}

		if (i == ARRAY_SIZE(facilities))
			return -EINVAL;

		pstr = lstr;
	}

	switch (op) {
	case -1:
		atomic_clear_mask(newmask, &lve_debug_mask);
		break;
	case  0:
		atomic_set(&lve_debug_mask, newmask);
		break;
	case +1:
		atomic_set_mask(newmask, &lve_debug_mask);
		break;
	default:
		BUG();
	}

	return count;
}

static int debug_show(struct seq_file *m, void *v)
{
	unsigned mask, i;
	mask = atomic_read(&lve_debug_mask);

	for (i = 0; i < ARRAY_SIZE(facilities); i++, mask >>= 1) {
		if (mask & 1) {
			seq_puts(m, facilities[i]);
			if (mask > 1)
				seq_putc(m, ' ');
		}
	}

	seq_putc(m, '\n');

	return 0;
}


static int lve_debug_open(struct inode *inode, struct file *file)
{
	int ret;

	ret = single_open(file, debug_show, NULL);
	if (ret)
		return ret;

	return 0;
}

static const struct file_operations lve_debug_fops = {
	.owner		= THIS_MODULE,
	.open		= lve_debug_open,
	.read		= seq_read,
	.write		= lve_debug_write,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static ssize_t lve_fail_write(struct file *file, const char __user *data,
				size_t count, loff_t *off)
{
	char str[50], *ptr;
	int rc;
	unsigned long new_value;

	if (!data || count > (sizeof(str) - 1) || *off)
		return -EINVAL;

	rc = copy_from_user(str, data, count);
	if (rc)
		return -EFAULT;
	str[count] = '\0';

	new_value = simple_strtoul(str, &ptr, 0);
	if (ptr == str)
		return -EINVAL;

	fail_value = new_value;

	return count;
}

static int fail_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%lx\n", fail_value);
	return 0;
}

static int lve_fail_open(struct inode *inode, struct file *file)
{
	int ret;

	ret = single_open(file, fail_show, NULL);
	if (ret)
		return ret;

	return 0;
}

static const struct file_operations lve_fail_fops = {
	.owner		= THIS_MODULE,
	.open		= lve_fail_open,
	.read		= seq_read,
	.write		= lve_fail_write,
	.llseek		= seq_lseek,
	.release	= single_release,
};

#ifdef HAVE_EXEC_NOTIFIER

static ssize_t lve_enter_write(struct file *file, const char __user *data,
				size_t count, loff_t *off)
{
	char str[50];
	int rc;
	struct lvp_ve_private *lvp = PDE_DATA(file->f_dentry->d_inode);

	if (!data || count > (sizeof(str) - 1) || *off)
		return -EINVAL;

	rc = copy_from_user(str, data, count);
	if (rc)
		return -EFAULT;
	str[count] = '\0';

	if (str[0] == '+') {
		rc = lve_exec_add_file(lvp, &str[1]);
		if (rc)
			return rc;
	} else if (str[0] == '-') {
		rc = lve_exec_del_file(lvp, &str[1]);
		if (rc)
			return rc;
	} else {
		return -EINVAL;
	}

	return count;
}

static int lve_enter_show(struct seq_file *m, void *v)
{
	struct lve_exec_entry *e;
	char pathname[256], *rpath;
	struct lvp_ve_private *lvp = m->private;

	read_lock(&lvp->lvp_exec_lock);
	list_for_each_entry(e, &lvp->lvp_exec_entries, list) {
		rpath = d_path(&e->path, pathname, sizeof(pathname));
		seq_printf(m, "%s\n", rpath);
	}
	read_unlock(&lvp->lvp_exec_lock);

	return 0;
}

static int lve_enter_open(struct inode *inode, struct file *file)
{
	return single_open(file, lve_enter_show, PDE_DATA(inode));
}

static const struct file_operations lve_enter_fops = {
	.owner		= THIS_MODULE,
	.open		= lve_enter_open,
	.read		= seq_read,
	.write		= lve_enter_write,
	.llseek		= seq_lseek,
	.release	= single_release,
};

#endif

/*************** stat end ****************************/

static int lve_dir_stats_show(struct seq_file *m, void *v)
{
	lve_stat_show(m, 1, m->private);
	return 0;
}

static int lve_dir_net_stats_show(struct seq_file *m, void *v)
{
	lve_net_stat_show(m, m->private);
	return 0;
}

static int lve_dir_stats_open(struct inode *inode, struct file *file)
{
	return single_open(file, lve_dir_stats_show, PDE_DATA(inode));
}

static int lve_dir_net_stats_open(struct inode *inode, struct file *file)
{
	return single_open(file, lve_dir_net_stats_show, PDE_DATA(inode));
}

static const struct file_operations lve_dir_stats_fops = {
	.owner		= THIS_MODULE,
	.open		= lve_dir_stats_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static const struct file_operations lve_dir_net_stats_fops = {
	.owner		= THIS_MODULE,
	.open		= lve_dir_net_stats_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

int lve_stats_dir_init(struct light_ve *lve)
{
	struct proc_dir_entry *lve_dir;
	char name[30];

#ifndef LVE_PER_VE
	if (lve->lve_lvp->lvp_id != ROOT_LVP)
		return 0;
#endif

	if (lve->lve_id != ROOT_LVE)
		snprintf(name, sizeof(name)-1, "%u", lve->lve_id);
	else
		snprintf(name, sizeof(name)-1, "default");

	lve_dir = proc_mkdir(name, lve->lve_lvp->lvp_stats_root);
	if (lve_dir == NULL)
		return -EINVAL;
	lve->lve_proc_dir = lve_dir;
	proc_create_data( "stat", S_IRUGO, lve_dir, &lve_dir_stats_fops, lve);
	proc_create_data("net_stat", S_IRUGO, lve_dir, &lve_dir_net_stats_fops, lve);

	return 0;
}

void lve_stats_dir_fini(struct light_ve *lve)
{
	char name[30];

	if (lve->lve_proc_dir == NULL)
		return;

	if (lve->lve_id != ROOT_LVE)
		snprintf(name, sizeof(name)-1, "%u", lve->lve_id);
	else
		snprintf(name, sizeof(name)-1, "default");

	remove_proc_entry("stat", lve->lve_proc_dir);
	remove_proc_entry("net_stat", lve->lve_proc_dir);
	remove_proc_entry(name, lve->lve_lvp->lvp_stats_root);
}
/* reseler defaults limits prints*/
/* OpenVZ print just own user default */
static void *
defaults_start(struct seq_file *m, loff_t *pos)
{
	void *ret;

	down_read(&lvp_sem);
	if (*pos)
		return NULL;

	m->private = SEQ_START_TOKEN;
#ifdef LVE_PER_VE
	ret = TASK_VE_PRIVATE(current);
#else
	ret = list_first_entry_or_null(&lvp_list, struct lvp_ve_private,
					lvp_link);
#endif
	return ret;
}

static int defaults_show(struct seq_file *m, void *v)
{
	struct lvp_ve_private *lvp = v;

	printk("show %p\n", lvp);

	if (m->private == SEQ_START_TOKEN) {
		seq_printf(m, "1:LVE");
		/* */
		lve_limits_hdr(m);
		seq_putc(m,'\n');
		m->private = NULL;
	}

	seq_printf(m, "%u\t", lvp->lvp_id);
	lve_limits_print(m, lvp->lvp_def_limits);
	seq_printf(m, "\t%u\t%u", 0, 0);
	seq_putc(m,'\n');

	return 0;
}

static void *
defaults_next(struct seq_file *m, void *v, loff_t *pos)
{
#ifdef LVE_PER_VE
	return NULL;
#else
	struct lvp_ve_private *lvp = v;

	if (lvp && !list_is_last(&lvp->lvp_link, &lvp_list)) {
		++*pos;
		return list_next_entry(lvp, lvp_link);
	} else {
		return NULL;
	}
#endif
}

static void
defaults_stop(struct seq_file *m, void *v)
{
	up_read(&lvp_sem);
}

static const struct seq_operations lve_defaults_op = {
	.start	= defaults_start,
	.next	= defaults_next,
	.stop	= defaults_stop,
	.show	= defaults_show
};

static int lve_defaults_open(struct inode *inode, struct file *file)
{
	int ret;

	ret = seq_open(file, &lve_defaults_op);
	if (ret)
		return ret;

	return 0;
}

static const struct file_operations lve_defaults_fops = {
	.owner		= THIS_MODULE,
	.open		= lve_defaults_open,
	.read		= seq_read,
	.release	= seq_release,
};
/****************************************************/
static int lve_map_open(struct inode *inode, struct file *file)
{
	int ret;

	ret = seq_open(file, &lve_map_op);
	if (ret)
		return ret;

	return 0;
}

static const struct file_operations lve_map_fops = {
	.owner		= THIS_MODULE,
	.open		= lve_map_open,
	.read		= seq_read,
	.release	= seq_release,
};
/****************************************************/

#define RESELLERS_DIR "resellers"

int lvp_proc_init(struct lvp_ve_private *lvp)
{
	struct proc_dir_entry *root, *lve_proc_root;
	char name[100];

	root = lve_procfs_root(lvp);

#ifndef LVE_PER_VE
	/* XXX ugly - just for fast test now */
	if (lvp->lvp_id != ROOT_LVP) {
		snprintf(name, sizeof(name), RESELLERS_DIR"/lvp%d", lvp->lvp_id);
		root = root_lvp->lvp_proc_root;
	} else
#endif
		snprintf(name, sizeof(name), "lve");

	lve_proc_root = proc_mkdir(name, root);
	LVE_DBG("root=%p lve_proc_root=%p\n", root, lve_proc_root);
	if (!lve_proc_root)
		return -ENOMEM;

	lvp->lvp_proc_root = lve_proc_root;
	proc_create_data("list", S_IRUGO, lve_proc_root, &lve_list_fops, lvp);
	proc_create_data("usage", S_IRUGO, lve_proc_root, &lve_usage_fops, lvp);
#ifdef HAVE_EXEC_NOTIFIER
	proc_create_data("enter", S_IRUGO, lve_proc_root, &lve_enter_fops, lvp);
#endif
	lvp->lvp_stats_root = proc_mkdir("per-lve", lve_proc_root);

	if (lvp->lvp_id == ROOT_LVP) {
		proc_create("defaults", S_IRUGO, lve_proc_root, &lve_defaults_fops);
#ifndef LVE_PER_VE
		proc_create("map", S_IRUGO, lve_proc_root, &lve_map_fops);
#endif
		proc_create("debug", S_IRUGO, lve_proc_root, &lve_debug_fops);
		proc_create("fail", S_IRUGO, lve_proc_root, &lve_fail_fops);
		proc_mkdir(RESELLERS_DIR, lve_proc_root);
	}

	return 0;
}

int lvp_proc_fini(struct lvp_ve_private *lvp)
{
	struct proc_dir_entry *root;
	char name[100];

	if (!lvp->lvp_proc_root)
		return 0;

#ifdef HAVE_EXEC_NOTIFIER
	remove_proc_entry("enter", lvp->lvp_proc_root);
#endif
	remove_proc_entry("usage", lvp->lvp_proc_root);
	remove_proc_entry("list", lvp->lvp_proc_root);
	/* */
	remove_proc_entry("per-lve", lvp->lvp_proc_root);

	if (lvp->lvp_id == ROOT_LVP) {
		remove_proc_entry("defaults", lvp->lvp_proc_root);
#ifndef LVE_PER_VE
		remove_proc_entry("map", lvp->lvp_proc_root);
#endif
		remove_proc_entry("fail", lvp->lvp_proc_root);
		remove_proc_entry("debug", lvp->lvp_proc_root);
		remove_proc_entry(RESELLERS_DIR, lvp->lvp_proc_root);
	}

	/* */
	root = lve_procfs_root(lvp);

#ifndef LVE_PER_VE
	/* XXX ugly - just for fast test now */
	if (lvp->lvp_id != ROOT_LVP) {
		snprintf(name, sizeof(name), RESELLERS_DIR"/lvp%d", lvp->lvp_id);
		root = root_lvp->lvp_proc_root;
	} else
#endif
		snprintf(name, sizeof(name), "lve");

	remove_proc_entry(name, root);

	return 0;
}
