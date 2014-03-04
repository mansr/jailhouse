/*
 * Jailhouse, a Linux-based partitioning hypervisor
 *
 * Copyright (c) Siemens AG, 2013
 *
 * Authors:
 *  Jan Kiszka <jan.kiszka@siemens.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/cpu.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/firmware.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/smp.h>
#include <linux/uaccess.h>
#include <linux/reboot.h>
#include <linux/vmalloc.h>
#include <linux/io.h>
#include <asm/smp.h>
#include <asm/cacheflush.h>

#include "jailhouse.h"
#include <jailhouse/header.h>
#include <jailhouse/hypercall.h>

/* For compatibility with older kernel versions */
#include <linux/version.h>

#if LINUX_VERSION_CODE < KERNEL_VERSION(3,11,0)
#define DEVICE_ATTR_RO(_name) \
	struct device_attribute dev_attr_##_name = __ATTR_RO(_name)
#endif /* < 3.11 */
/* End of compatibility section - remove as version become obsolete */

#define JAILHOUSE_FW_NAME	"jailhouse.bin"

struct cell {
	struct kobject kobj;
	struct list_head entry;
	unsigned int id;
};

MODULE_DESCRIPTION("Loader for Jailhouse partitioning hypervisor");
MODULE_LICENSE("GPL");
MODULE_FIRMWARE(JAILHOUSE_FW_NAME);

static struct device *jailhouse_dev;
static DEFINE_MUTEX(lock);
static bool enabled;
static void *hypervisor_mem;
static unsigned long hv_core_percpu_size;
static cpumask_t offlined_cpus;
static atomic_t call_done;
static int error_code;
static LIST_HEAD(cells);
static struct kobject *cells_dir;

static inline unsigned int
cell_cpumask_next(int n, const struct jailhouse_cell_desc *config)
{
	const unsigned long *cpu_mask = jailhouse_cell_cpu_set(config);

	return find_next_bit(cpu_mask, config->cpu_set_size * 8, n + 1);
}

#define for_each_cell_cpu(cpu, config)				\
	for ((cpu) = -1;					\
	     (cpu) = cell_cpumask_next((cpu), (config)),	\
	     (cpu) < (config)->cpu_set_size * 8;)

static void cell_kobj_release(struct kobject *kobj)
{
	struct cell *cell = container_of(kobj, struct cell, kobj);

	kfree(cell);
}

static struct kobj_type cell_type = {
	.release = cell_kobj_release,
};

static struct cell *create_cell(const struct jailhouse_cell_desc *cell_desc)
{
	struct cell *cell;
	int err;

	cell = kzalloc(sizeof(*cell), GFP_KERNEL);
	if (!cell)
		return ERR_PTR(-ENOMEM);

	err = kobject_init_and_add(&cell->kobj, &cell_type, cells_dir, "%s",
				   cell_desc->name);
	if (err) {
		kfree(cell);
		return ERR_PTR(err);
	}

	return cell;
}

static void register_cell(struct cell *cell)
{
	list_add_tail(&cell->entry, &cells);
	kobject_uevent(&cell->kobj, KOBJ_ADD);
}

static struct cell *find_cell(struct jailhouse_cell_desc *cell_desc)
{
	struct cell *cell;

	list_for_each_entry(cell, &cells, entry)
		if (strcmp(kobject_name(&cell->kobj), cell_desc->name) == 0)
			return cell;
	return NULL;
}

static void delete_cell(struct cell *cell)
{
	list_del(&cell->entry);
	kobject_put(&cell->kobj);
}

static void *jailhouse_ioremap(phys_addr_t phys, unsigned long virt,
			       unsigned long size)
{
	struct vm_struct *vma;

	if (virt)
		vma = __get_vm_area(size, VM_IOREMAP, virt,
				    virt + size + PAGE_SIZE);
	else
		vma = __get_vm_area(size, VM_IOREMAP, VMALLOC_START,
				    VMALLOC_END);
	if (!vma)
		return NULL;
	vma->phys_addr = phys;

	if (ioremap_page_range((unsigned long)vma->addr,
			       (unsigned long)vma->addr + size, phys,
			       PAGE_KERNEL_EXEC)) {
		vunmap(vma->addr);
		return NULL;
	}

	return vma->addr;
}

static void enter_hypervisor(void *info)
{
	struct jailhouse_header *header = info;
	int err;

	/* either returns 0 or the same error code across all CPUs */
	err = header->entry(smp_processor_id());
	if (err)
		error_code = err;

	atomic_inc(&call_done);
}

static int jailhouse_enable(struct jailhouse_system __user *arg)
{
	const struct firmware *hypervisor;
	struct jailhouse_system config_header;
	struct jailhouse_memory *hv_mem = &config_header.hypervisor_memory;
	struct jailhouse_header *header;
	unsigned long config_size;
	struct cell *root_cell;
	int err;

	if (copy_from_user(&config_header, arg, sizeof(config_header)))
		return -EFAULT;
	config_header.system.name[JAILHOUSE_CELL_NAME_MAXLEN] = 0;

	if (mutex_lock_interruptible(&lock) != 0)
		return -EINTR;

	err = -EBUSY;
	if (enabled || !try_module_get(THIS_MODULE))
		goto error_unlock;

	root_cell = create_cell(&config_header.system);
	err = -ENOMEM;
	if (!root_cell)
		goto error_put_module;

	err = request_firmware(&hypervisor, JAILHOUSE_FW_NAME, jailhouse_dev);
	if (err) {
		pr_err("jailhouse: Missing hypervisor image %s\n",
		       JAILHOUSE_FW_NAME);
		goto error_free_cell;
	}

	header = (struct jailhouse_header *)hypervisor->data;

	err = -EINVAL;
	if (memcmp(header->signature, JAILHOUSE_SIGNATURE,
		   sizeof(header->signature)) != 0)
		goto error_release_fw;

	hv_core_percpu_size = PAGE_ALIGN(header->core_size) +
		num_possible_cpus() * header->percpu_size;
	config_size = jailhouse_system_config_size(&config_header);
	if (hv_mem->size <= hv_core_percpu_size + config_size)
		goto error_release_fw;

	hypervisor_mem = jailhouse_ioremap(hv_mem->phys_start, JAILHOUSE_BASE,
					   hv_mem->size);
	if (!hypervisor_mem) {
		pr_err("jailhouse: Unable to map RAM reserved for hypervisor "
		       "at %08lx\n", (unsigned long)hv_mem->phys_start);
		goto error_release_fw;
	}

	memcpy(hypervisor_mem, hypervisor->data, hypervisor->size);
	memset(hypervisor_mem + hypervisor->size, 0,
	       hv_mem->size - hypervisor->size);

	header = (struct jailhouse_header *)hypervisor_mem;
	header->size = hv_mem->size;
	header->page_offset =
		(unsigned long)hypervisor_mem - hv_mem->phys_start;
	header->possible_cpus = num_possible_cpus();

	if (copy_from_user(hypervisor_mem + hv_core_percpu_size, arg,
			   config_size)) {
		err = -EFAULT;
		goto error_unmap;
	}

	error_code = 0;

	preempt_disable();

	header->online_cpus = num_online_cpus();

	atomic_set(&call_done, 0);
	on_each_cpu(enter_hypervisor, header, 0);
	while (atomic_read(&call_done) != num_online_cpus())
		cpu_relax();

	preempt_enable();

	if (error_code) {
		err = error_code;
		goto error_unmap;
	}

	release_firmware(hypervisor);

	enabled = true;
	root_cell->id = 0;
	register_cell(root_cell);

	mutex_unlock(&lock);

	pr_info("The Jailhouse is opening.\n");

	return 0;

error_unmap:
	vunmap(hypervisor_mem);

error_release_fw:
	release_firmware(hypervisor);

error_free_cell:
	kobject_put(&root_cell->kobj);

error_put_module:
	module_put(THIS_MODULE);

error_unlock:
	mutex_unlock(&lock);
	return err;
}

static void leave_hypervisor(void *info)
{
	unsigned long size;
	void *page;
	int err;

	/* Touch each hypervisor page we may need during the switch so that
	 * the active mm definitely contains all mappings. At least x86 does
	 * not support taking any faults while switching worlds. */
	for (page = hypervisor_mem, size = hv_core_percpu_size; size > 0;
	     size -= PAGE_SIZE, page += PAGE_SIZE)
		readl(page);

	/* either returns 0 or the same error code across all CPUs */
	err = jailhouse_call0(JAILHOUSE_HC_DISABLE);
	if (err)
		error_code = err;

	atomic_inc(&call_done);
}

static int jailhouse_disable(void)
{
	struct cell *cell, *tmp;
	unsigned int cpu;
	int err;

	if (mutex_lock_interruptible(&lock) != 0)
		return -EINTR;

	if (!enabled) {
		err = -EINVAL;
		goto unlock_out;
	}

	error_code = 0;

	preempt_disable();

	atomic_set(&call_done, 0);
	on_each_cpu(leave_hypervisor, NULL, 0);
	while (atomic_read(&call_done) != num_online_cpus())
		cpu_relax();

	preempt_enable();

	err = error_code;
	if (err)
		goto unlock_out;

	vunmap(hypervisor_mem);

	for_each_cpu_mask(cpu, offlined_cpus) {
		if (cpu_up(cpu) != 0)
			pr_err("Jailhouse: failed to bring CPU %d back "
			       "online\n", cpu);
		cpu_clear(cpu, offlined_cpus);
	}

	list_for_each_entry_safe(cell, tmp, &cells, entry)
		delete_cell(cell);
	enabled = false;
	module_put(THIS_MODULE);

	pr_info("The Jailhouse was closed.\n");

unlock_out:
	mutex_unlock(&lock);

	return err;
}

static int load_image(struct jailhouse_cell_desc *config,
		      struct jailhouse_preload_image __user *uimage)
{
	struct jailhouse_preload_image image;
	const struct jailhouse_memory *mem;
	unsigned int regions;
	u64 image_offset;
	void *image_mem;
	int err = 0;

	if (copy_from_user(&image, uimage, sizeof(image)))
		return -EFAULT;

	mem = jailhouse_cell_mem_regions(config);
	for (regions = config->num_memory_regions; regions > 0; regions--) {
		image_offset = image.target_address - mem->virt_start;
		if (image.target_address >= mem->virt_start &&
		    image_offset < mem->size) {
			if (image.size > mem->size - image_offset)
				return -EINVAL;
			break;
		}
		mem++;
	}
	if (regions == 0)
		return -EINVAL;

	image_mem = jailhouse_ioremap(mem->phys_start + image_offset, 0,
				      image.size);
	if (!image_mem) {
		pr_err("jailhouse: Unable to map cell RAM at %08llx "
		       "for image loading\n",
		       (unsigned long long)(mem->phys_start + image_offset));
		return -EBUSY;
	}

	if (copy_from_user(image_mem,
			   (void *)(unsigned long)image.source_address,
			   image.size))
		err = -EFAULT;

	vunmap(image_mem);

	return err;
}

static int jailhouse_cell_create(struct jailhouse_new_cell __user *arg)
{
	struct jailhouse_preload_image __user *image = arg->image;
	struct jailhouse_new_cell cell_params;
	struct jailhouse_cell_desc *config;
	unsigned int cpu, n;
	struct cell *cell;
	int id, err;

	if (copy_from_user(&cell_params, arg, sizeof(cell_params)))
		return -EFAULT;

	config = kmalloc(cell_params.config_size, GFP_KERNEL | GFP_DMA);
	if (!config)
		return -ENOMEM;

	if (copy_from_user(config,
			   (void *)(unsigned long)cell_params.config_address,
			   cell_params.config_size)) {
		err = -EFAULT;
		goto kfree_config_out;
	}
	config->name[JAILHOUSE_CELL_NAME_MAXLEN] = 0;

	if (mutex_lock_interruptible(&lock) != 0) {
		err = -EINTR;
		goto kfree_config_out;
	}

	if (!enabled) {
		err = -EINVAL;
		goto unlock_out;
	}

	if (find_cell(config) != NULL) {
		err = -EEXIST;
		goto unlock_out;
	}

	for (n = cell_params.num_preload_images; n > 0; n--, image++) {
		err = load_image(config, image);
		if (err)
			goto unlock_out;
	}

	cell = create_cell(config);
	if (IS_ERR(cell)) {
		err = PTR_ERR(cell);
		goto unlock_out;
	}

	for_each_cell_cpu(cpu, config)
		if (cpu_online(cpu)) {
			err = cpu_down(cpu);
			if (err)
				goto error_cpu_online;
			cpu_set(cpu, offlined_cpus);
		}

	id = jailhouse_call1(JAILHOUSE_HC_CELL_CREATE, __pa(config));
	if (id < 0) {
		err = id;
		goto error_cpu_online;
	}

	cell->id = id;
	register_cell(cell);

	pr_info("Created Jailhouse cell \"%s\"\n", config->name);

unlock_out:
	mutex_unlock(&lock);

kfree_config_out:
	kfree(config);

	return err;

error_cpu_online:
	for_each_cell_cpu(cpu, config)
		if (!cpu_online(cpu) && cpu_up(cpu) == 0)
			cpu_clear(cpu, offlined_cpus);
	kobject_put(&cell->kobj);
	goto unlock_out;
}

static int jailhouse_cell_destroy(const char __user *arg)
{
	struct jailhouse_cell_desc *config;
	struct jailhouse_cell cell_params;
	struct cell *cell;
	unsigned int cpu;
	int err;

	if (copy_from_user(&cell_params, arg, sizeof(cell_params)))
		return -EFAULT;

	config = kmalloc(cell_params.config_size, GFP_KERNEL | GFP_DMA);
	if (!config)
		return -ENOMEM;

	if (copy_from_user(config,
			   (void *)(unsigned long)cell_params.config_address,
			   cell_params.config_size)) {
		err = -EFAULT;
		goto kfree_config_out;
	}
	config->name[JAILHOUSE_CELL_NAME_MAXLEN] = 0;

	if (mutex_lock_interruptible(&lock) != 0) {
		err = -EINTR;
		goto kfree_config_out;
	}

	if (!enabled) {
		err = -EINVAL;
		goto unlock_out;
	}

	cell = find_cell(config);
	if (!cell) {
		err = -ENOENT;
		goto unlock_out;
	}

	err = jailhouse_call1(JAILHOUSE_HC_CELL_DESTROY, cell->id);
	if (err)
		goto unlock_out;

	delete_cell(cell);

	for_each_cell_cpu(cpu, config)
		if (cpu_isset(cpu, offlined_cpus)) {
			if (cpu_up(cpu) != 0)
				pr_err("Jailhouse: failed to bring CPU %d "
				       "back online\n", cpu);
			cpu_clear(cpu, offlined_cpus);
		}

	pr_info("Destroyed Jailhouse cell \"%s\"\n", config->name);

unlock_out:
	mutex_unlock(&lock);

kfree_config_out:
	kfree(config);

	return err;
}

static long jailhouse_ioctl(struct file *file, unsigned int ioctl,
			    unsigned long arg)
{
	long err;

	switch (ioctl) {
	case JAILHOUSE_ENABLE:
		err = jailhouse_enable(
			(struct jailhouse_system __user *)arg);
		break;
	case JAILHOUSE_DISABLE:
		err = jailhouse_disable();
		break;
	case JAILHOUSE_CELL_CREATE:
		err = jailhouse_cell_create(
			(struct jailhouse_new_cell __user *)arg);
		break;
	case JAILHOUSE_CELL_DESTROY:
		err = jailhouse_cell_destroy((const char __user *)arg);
		break;
	default:
		err = -EINVAL;
		break;
	}

	return err;
}

static const struct file_operations jailhouse_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = jailhouse_ioctl,
	.compat_ioctl = jailhouse_ioctl,
	.llseek = noop_llseek,
};

static struct miscdevice jailhouse_misc_dev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "jailhouse",
	.fops = &jailhouse_fops,
};

static int jailhouse_shutdown_notify(struct notifier_block *unused1,
				     unsigned long unused2, void *unused3)
{
	int err;

	err = jailhouse_disable();
	if (err && err != -EINVAL)
		pr_emerg("jailhouse: ordered shutdown failed!\n");

	return NOTIFY_DONE;
}

static struct notifier_block jailhouse_shutdown_nb = {
	.notifier_call = jailhouse_shutdown_notify,
};

static ssize_t enabled_show(struct device *dev, struct device_attribute *attr,
			    char *buffer)
{
	return sprintf(buffer, "%d\n", enabled);
}

static DEVICE_ATTR_RO(enabled);

static struct attribute *jailhouse_sysfs_entries[] = {
	&dev_attr_enabled.attr,
	NULL
};

static struct attribute_group jailhouse_attribute_group = {
	.name = NULL,
	.attrs = jailhouse_sysfs_entries,
};

static int __init jailhouse_init(void)
{
	int err;

	jailhouse_dev = root_device_register("jailhouse");
	if (IS_ERR(jailhouse_dev))
		return PTR_ERR(jailhouse_dev);

	err = sysfs_create_group(&jailhouse_dev->kobj,
				 &jailhouse_attribute_group);
	if (err)
		goto unreg_dev;

	cells_dir = kobject_create_and_add("cells", &jailhouse_dev->kobj);
	if (!cells_dir) {
		err = -ENOMEM;
		goto remove_attrs;
	}

	err = misc_register(&jailhouse_misc_dev);
	if (err)
		goto remove_cells_dir;

	register_reboot_notifier(&jailhouse_shutdown_nb);

	return 0;

remove_cells_dir:
	kobject_put(cells_dir);

remove_attrs:
	sysfs_remove_group(&jailhouse_dev->kobj, &jailhouse_attribute_group);

unreg_dev:
	root_device_unregister(jailhouse_dev);
	return err;
}

static void __exit jailhouse_exit(void)
{
	unregister_reboot_notifier(&jailhouse_shutdown_nb);
	misc_deregister(&jailhouse_misc_dev);
	kobject_put(cells_dir);
	sysfs_remove_group(&jailhouse_dev->kobj, &jailhouse_attribute_group);
	root_device_unregister(jailhouse_dev);
}

module_init(jailhouse_init);
module_exit(jailhouse_exit);