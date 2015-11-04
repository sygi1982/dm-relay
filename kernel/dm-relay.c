/*
 * Simple relay control plugin
 * based on dm-delay
 *
 * grzegorz.sygieda@gmail.com
 *
 * This file is released under the GPL.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/blkdev.h>
#include <linux/bio.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/version.h>

#include <linux/device-mapper.h>

#define DM_MSG_PREFIX "relay"
#define DM_DEV_NAME  "dm-relay"

struct relay_c {
	struct timer_list relay_timer;
	struct work_struct relay_work;
	wait_queue_head_t relay_queue;

	struct dm_dev *dev_target;
	struct dm_target * ti;
	char endpoint_name[64];
	atomic_t may_delay;
	unsigned sleep_delay;
	unsigned wake_delay;
	bool active;
	struct kobject *sysfs_kobj;

	struct mutex lock;
};

static struct workqueue_struct *krelayd_wq;

static void handle_relay_timer(unsigned long data)
{
	struct relay_c *dc = (struct relay_c *)data;

	queue_work(krelayd_wq, &dc->relay_work);
}

static void queue_relay_job(struct relay_c *dc, unsigned long expires, bool reinit)
{
	mutex_lock(&dc->lock);

	if (reinit || !timer_pending(&dc->relay_timer)) {
		mod_timer(&dc->relay_timer, expires);
		//printk(KERN_ERR "queue_relay_job %lu", expires);
	}

	mutex_unlock(&dc->lock);
}

static void relay_disable_device(struct relay_c *dc)
{
	char *envp[2];
	int retval;

	envp[0] = "RELAY_SWITCH=OFF";
	envp[1] = NULL;

	retval = kobject_uevent_env(dc->sysfs_kobj, KOBJ_CHANGE, envp);
	printk(KERN_ERR "kobject_uevent_env %d", retval);
}

static void relay_enable_device(struct relay_c *dc)
{
	char *envp[2];
	int retval;

	envp[0] = "RELAY_SWITCH=ON";
	envp[1] = NULL;

	retval = kobject_uevent_env(dc->sysfs_kobj, KOBJ_CHANGE, envp);
	printk(KERN_ERR "kobject_uevent_env %d", retval);
}

static void relay_job(struct work_struct *work)
{
	struct relay_c *dc;

	dc = container_of(work, struct relay_c, relay_work);

	mutex_lock(&dc->lock);
	// switch on/off physical drive depending on actual state
	if (dc->active) {
		if (dc->dev_target)
			dm_put_device(dc->ti, dc->dev_target);
		dc->dev_target = NULL;
		relay_disable_device(dc);
		dc->active = false;
		printk(KERN_ERR "relay_job ACTIVE -> IDLE");
	} else {
		dc->active = true;
		printk(KERN_ERR "relay_job IDLE -> ACTIVE waking jobs ...");
		wake_up_all(&dc->relay_queue);
		printk(KERN_ERR "relay_job IDLE -> ACTIVE waking jobs done");
	}
	mutex_unlock(&dc->lock);
}

static ssize_t relay_switch_ctrl_show(struct device *this,
			struct device_attribute *attr,
			const char *buf)
{
	return 0;
}

static ssize_t relay_switch_ctrl_store(struct device *this,
			struct device_attribute *attr,
			const char *buf,
			size_t count)
{
	return 0;
}

static DEVICE_ATTR(switch_ctrl, S_IRUGO|S_IWUGO, relay_switch_ctrl_show,
						relay_switch_ctrl_store);

static struct attribute *relay_attrs[] = {
	&dev_attr_switch_ctrl.attr,
	NULL,
};

static struct attribute_group relay_attrs_group = {
	.attrs = relay_attrs,
};

/*
 * Mapping parameters:
 *    <device> <sleep_delay> <wake_delay>
 *
 * Delays are specified in milliseconds.
 */
static int relay_ctr(struct dm_target *ti, unsigned int argc, char **argv)
{
	struct relay_c *dc;
	int retval;

	printk(KERN_ERR "relay_ctr() %s", argv[0]);
	if (argc != 3) {
		ti->error = "requires exactly 3 arguments";
		return -EINVAL;
	}

	dc = kzalloc(sizeof(*dc), GFP_KERNEL);
	if (!dc) {
		ti->error = "Cannot allocate context";
		return -ENOMEM;
	}

	dc->ti = ti;

	// Assuming the device is present/switched on
	if (dm_get_device(ti, argv[0],
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,34)
			0, ti->len,
#endif
			dm_table_get_mode(ti->table), &dc->dev_target)) {
		ti->error = "Device lookup failed";
		goto bad;
	}
	strcpy(dc->endpoint_name, argv[0]);

	if (sscanf(argv[1], "%u", &dc->sleep_delay) != 1) {
		ti->error = "Invalid sleep delay";
		goto bad_dev;
	}

	if (sscanf(argv[2], "%u", &dc->wake_delay) != 1) {
		ti->error = "Invalid wake delay";
		goto bad_dev;
	}

	setup_timer(&dc->relay_timer, handle_relay_timer, (unsigned long)dc);

	INIT_WORK(&dc->relay_work, relay_job);
	init_waitqueue_head(&dc->relay_queue);
	mutex_init(&dc->lock);
	atomic_set(&dc->may_delay, 1);

	dc->sysfs_kobj = kobject_create_and_add(DM_DEV_NAME, kernel_kobj);

	if (dc->sysfs_kobj) {
		printk(KERN_ERR "relay_ctr creating sysfs ... ");
		dc->sysfs_kobj->kset = kset_create_and_add(DM_DEV_NAME, NULL, dc->sysfs_kobj);

		retval = sysfs_create_group(dc->sysfs_kobj, &relay_attrs_group);
		if (retval)
			kobject_put(dc->sysfs_kobj);
	}

	dc->active = true;

	ti->num_flush_requests = 1;
	ti->private = dc;
	return 0;

bad_dev:
	if(dc->dev_target)
		dm_put_device(ti, dc->dev_target);
bad:
	kfree(dc);
	return -EINVAL;
}

static void relay_dtr(struct dm_target *ti)
{
	struct relay_c *dc = ti->private;

	printk(KERN_ERR "relay_dtr()");
	flush_workqueue(krelayd_wq);

	if (dc->sysfs_kobj) {
		printk(KERN_ERR "relay_dtr removing sysfs ... ");
		kset_unregister(dc->sysfs_kobj->kset);
		sysfs_remove_group(dc->sysfs_kobj, &relay_attrs_group);
		kobject_del(dc->sysfs_kobj);
	}

	if(dc->dev_target)
		dm_put_device(ti, dc->dev_target);

	kfree(dc);
}

static void relay_presuspend(struct dm_target *ti)
{
	struct relay_c *dc = ti->private;

	atomic_set(&dc->may_delay, 0);
	del_timer_sync(&dc->relay_timer);
}

static void relay_resume(struct dm_target *ti)
{
	struct relay_c *dc = ti->private;

	atomic_set(&dc->may_delay, 1);
}

static void flush_bios(struct bio *bio)
{
	struct bio *n;

	while (bio) {
		n = bio->bi_next;
		bio->bi_next = NULL;
		generic_make_request(bio);
		bio = n;
	}
}

#define _DELAY(d) (jiffies + (d * HZ / 1000))

static int relay_map(struct dm_target *ti, struct bio *bio,
		     union map_info *map_context)
{
	struct relay_c *dc = ti->private;
	unsigned long delay;

	if (atomic_read(&dc->may_delay)) {
		if (dc->active == true) {
			delay = _DELAY(dc->sleep_delay);
			queue_relay_job(dc, delay, true);
			printk(KERN_ERR "relay_map ACTIVE queing sleep relay job %d", dc->sleep_delay);
		} else {
			delay = _DELAY(dc->wake_delay);
			relay_enable_device(dc);
			queue_relay_job(dc, delay, false);
			printk(KERN_ERR "relay_map IDLE queing wake relay job %d, waiting ....", dc->wake_delay);
			wait_event_interruptible(dc->relay_queue, dc->active);
			printk(KERN_ERR "relay_map IDLE queing wake relay job done");
			if (dm_get_device(ti, dc->endpoint_name,
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,34)
					0, ti->len,
#endif
					dm_table_get_mode(ti->table), &dc->dev_target)) {
				ti->error = "Device lookup failed";
				printk(KERN_ERR "relay_map dm_get_device failed");
				dc->active = false;
				return -EIO;
			}
		}
	}

	if (dc->dev_target) {
		bio->bi_bdev = dc->dev_target->bdev;
		bio->bi_sector = (bio->bi_sector - ti->begin);

		flush_bios(bio);

		return 0;
	} else {
		return -EIO;
	}
}

static int relay_status(struct dm_target *ti, status_type_t type,
			char *result, unsigned maxlen)
{
	struct relay_c *dc = ti->private;
	int sz = 0;

	if (!dc->dev_target) {
		printk(KERN_ERR "relay_status dev_target==NULL");
		return -EINVAL;
	}

	switch (type) {
	case STATUSTYPE_INFO:
		//DMEMIT("%u %u", dc->reads, dc->writes);
		break;

	case STATUSTYPE_TABLE:
		DMEMIT("%s %u %u", dc->dev_target->name,
		       dc->sleep_delay, dc->wake_delay);
		break;
	}

	return 0;
}

static int relay_iterate_devices(struct dm_target *ti,
				 iterate_devices_callout_fn fn, void *data)
{
	struct relay_c *dc = ti->private;
	int ret = 0;

	if (!dc->dev_target) {
		printk(KERN_ERR "relay_iterate_devices dev_target==NULL");
		return -EINVAL;
	}

	ret = fn(ti, dc->dev_target, 0, ti->len, data);

	return ret;
}

static struct target_type relay_target = {
	.name	     = "relay",
	.version     = {1, 0, 0},
	.module      = THIS_MODULE,
	.ctr	     = relay_ctr,
	.dtr	     = relay_dtr,
	.map	     = relay_map,
	.presuspend  = relay_presuspend,
	.resume	     = relay_resume,
	.status	     = relay_status,
	.iterate_devices = relay_iterate_devices,
};

static int __init dm_relay_init(void)
{
	int r = -ENOMEM;

	krelayd_wq = create_workqueue("krelayd");
	if (!krelayd_wq) {
		DMERR("Couldn't start krelayd");
		return r;
	}

	r = dm_register_target(&relay_target);
	if (r < 0) {
		DMERR("register failed %d", r);
		goto bad;
	}

	return 0;

bad:
	destroy_workqueue(krelayd_wq);
	return r;
}

static void __exit dm_relay_exit(void)
{
	dm_unregister_target(&relay_target);
	destroy_workqueue(krelayd_wq);
}

/* Module hooks */
module_init(dm_relay_init);
module_exit(dm_relay_exit);

MODULE_DESCRIPTION(DM_NAME " relay target");
MODULE_AUTHOR("Grzegorz Sygieda <grzegorz.sygieda@gmail.com>");
MODULE_LICENSE("GPL");
