// SPDX-License-Identifier: GPL-2.0
/*
 * fakesensor - driver for the Pico 2 "fake I2C sensor" with a data-ready IRQ
 *
 * Data path:
 *   DRDY (level high) -> fs_irq_hard()  [hard IRQ: timestamp only]
 *                     -> fs_irq_thread() [IRQ thread: I2C burst read, may sleep]
 *                     -> kfifo -> wake_up() -> read()/poll() on /dev/fakesensor
 *
 * IRQF_ONESHOT keeps the line masked from the hard handler until the thread
 * has read DATA_H (which drops DRDY on the device), so a level IRQ cannot storm.
 */
#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/regmap.h>
#include <linux/interrupt.h>
#include <linux/miscdevice.h>
#include <linux/kfifo.h>
#include <linux/wait.h>
#include <linux/poll.h>
#include <linux/fs.h>
#include <linux/mutex.h>
#include <linux/ktime.h>
#include <linux/of.h>

#include "fakesensor.h"

#define FS_REG_WHOAMI		0x00
#define FS_REG_CTRL		0x01
#define   FS_CTRL_EN		BIT(0)
#define   FS_CTRL_INT_EN	BIT(1)
#define FS_REG_ODR		0x02
#define FS_REG_STATUS		0x03
#define   FS_STATUS_DRDY	BIT(0)
#define   FS_STATUS_OVR		BIT(1)
#define FS_REG_COUNT		0x04
#define FS_REG_DATA_L		0x05
#define FS_REG_DATA_H		0x06

#define FS_WHOAMI_VAL		0x5A
#define FS_FIFO_LEN		64	/* samples, power of 2 */

struct fs_dev {
	struct device		*dev;
	struct regmap		*regmap;
	int			irq;

	u64			irq_ts;	/* written in hard IRQ, read in thread */

	DECLARE_KFIFO(fifo, struct fs_sample, FS_FIFO_LEN);
	struct mutex		read_lock;	/* one reader at a time drains the fifo */
	wait_queue_head_t	wq;
	struct miscdevice	misc;

	/* statistics (approximate, lockless) */
	unsigned long		hardirqs;
	unsigned long		threads;
	unsigned long		spurious;
	unsigned long		fifo_drops;
	unsigned long		dev_overruns;
};

static const struct regmap_config fs_regmap_cfg = {
	.reg_bits	= 8,
	.val_bits	= 8,
	.max_register	= FS_REG_DATA_H,
};

/* ---------------------------------------------------------------- IRQ */

/* Hard IRQ context: IF=0, cannot sleep, cannot touch I2C. Timestamp only. */
static irqreturn_t fs_irq_hard(int irq, void *data)
{
	struct fs_dev *fs = data;

	fs->irq_ts = ktime_get_ns();
	fs->hardirqs++;
	return IRQ_WAKE_THREAD;
}

/* IRQ thread (irq/N-fakesensor, SCHED_FIFO): may sleep, so I2C is fine. */
static irqreturn_t fs_irq_thread(int irq, void *data)
{
	struct fs_dev *fs = data;
	struct fs_sample s = { };
	u8 buf[4];	/* STATUS, COUNT, DATA_L, DATA_H in one burst */
	int ret;

	fs->threads++;

	ret = regmap_bulk_read(fs->regmap, FS_REG_STATUS, buf, sizeof(buf));
	if (ret) {
		dev_err_ratelimited(fs->dev, "burst read failed: %d\n", ret);
		return IRQ_NONE;
	}

	if (!(buf[0] & FS_STATUS_DRDY)) {
		fs->spurious++;
		return IRQ_NONE;	/* lets the spurious detector notice storms */
	}

	if (buf[0] & FS_STATUS_OVR)
		fs->dev_overruns++;

	s.status = buf[0];
	s.seq    = buf[1];
	s.val    = (s16)(buf[2] | (buf[3] << 8));
	s.ts_ns  = fs->irq_ts;	/* safe: ONESHOT masks the line until we return */

	/* single producer (this thread) + single consumer (read_lock): no spinlock */
	if (!kfifo_put(&fs->fifo, s))
		fs->fifo_drops++;

	wake_up_interruptible(&fs->wq);
	return IRQ_HANDLED;
}

/* ------------------------------------------------------- char device */

static ssize_t fs_read(struct file *file, char __user *ubuf, size_t len, loff_t *ppos)
{
	struct fs_dev *fs = container_of(file->private_data, struct fs_dev, misc);
	unsigned int copied;
	int ret;

	if (len < sizeof(struct fs_sample))
		return -EINVAL;

	if (mutex_lock_interruptible(&fs->read_lock))
		return -ERESTARTSYS;

	while (kfifo_is_empty(&fs->fifo)) {
		mutex_unlock(&fs->read_lock);
		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;
		/* TASK_INTERRUPTIBLE sleep until the IRQ thread wakes us */
		ret = wait_event_interruptible(fs->wq, !kfifo_is_empty(&fs->fifo));
		if (ret)
			return ret;	/* -ERESTARTSYS on a signal */
		if (mutex_lock_interruptible(&fs->read_lock))
			return -ERESTARTSYS;
	}

	ret = kfifo_to_user(&fs->fifo, ubuf, len, &copied);
	mutex_unlock(&fs->read_lock);

	return ret ? ret : copied;
}

static __poll_t fs_poll(struct file *file, poll_table *pt)
{
	struct fs_dev *fs = container_of(file->private_data, struct fs_dev, misc);

	poll_wait(file, &fs->wq, pt);
	return kfifo_is_empty(&fs->fifo) ? 0 : EPOLLIN | EPOLLRDNORM;
}

/*
 * EXERCISE: if the device is unbound while a file is open, fs is freed by
 * devm while read()/poll() can still use it (use-after-free). Reproduce it
 * with KASAN, then fix it (kref on fs_dev, get on open / put on release,
 * plus a "gone" flag that wakes and fails sleeping readers).
 */
static const struct file_operations fs_fops = {
	.owner	= THIS_MODULE,
	.read	= fs_read,
	.poll	= fs_poll,
	.llseek	= noop_llseek,
};

/* -------------------------------------------------------------- sysfs */

static ssize_t odr_ms_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct fs_dev *fs = dev_get_drvdata(dev);
	unsigned int val;
	int ret;

	ret = regmap_read(fs->regmap, FS_REG_ODR, &val);
	if (ret)
		return ret;
	return sysfs_emit(buf, "%u\n", val * 10);
}

static ssize_t odr_ms_store(struct device *dev, struct device_attribute *attr,
			    const char *buf, size_t count)
{
	struct fs_dev *fs = dev_get_drvdata(dev);
	unsigned int ms;
	int ret;

	ret = kstrtouint(buf, 0, &ms);
	if (ret)
		return ret;
	if (ms < 10 || ms > 2550)
		return -EINVAL;

	ret = regmap_write(fs->regmap, FS_REG_ODR, ms / 10);
	return ret ? ret : count;
}
static DEVICE_ATTR_RW(odr_ms);

static ssize_t stats_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct fs_dev *fs = dev_get_drvdata(dev);

	return sysfs_emit(buf,
		"hardirqs %lu\nthreads %lu\nspurious %lu\nfifo_drops %lu\ndev_overruns %lu\nqueued %u\n",
		READ_ONCE(fs->hardirqs), READ_ONCE(fs->threads),
		READ_ONCE(fs->spurious), READ_ONCE(fs->fifo_drops),
		READ_ONCE(fs->dev_overruns), kfifo_len(&fs->fifo));
}
static DEVICE_ATTR_RO(stats);

static struct attribute *fs_attrs[] = {
	&dev_attr_odr_ms.attr,
	&dev_attr_stats.attr,
	NULL
};
ATTRIBUTE_GROUPS(fs);

/* -------------------------------------------------------- probe/remove */

static int fs_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct fs_dev *fs;
	unsigned int id;
	int ret;

	fs = devm_kzalloc(dev, sizeof(*fs), GFP_KERNEL);
	if (!fs)
		return -ENOMEM;

	fs->dev = dev;
	INIT_KFIFO(fs->fifo);
	mutex_init(&fs->read_lock);
	init_waitqueue_head(&fs->wq);
	i2c_set_clientdata(client, fs);

	fs->regmap = devm_regmap_init_i2c(client, &fs_regmap_cfg);
	if (IS_ERR(fs->regmap))
		return dev_err_probe(dev, PTR_ERR(fs->regmap), "regmap init\n");

	ret = regmap_read(fs->regmap, FS_REG_WHOAMI, &id);
	if (ret)
		return dev_err_probe(dev, ret, "no response at 0x%02x\n", client->addr);
	if (id != FS_WHOAMI_VAL)
		return dev_err_probe(dev, -ENODEV, "WHO_AM_I 0x%02x, expected 0x%02x\n",
				     id, FS_WHOAMI_VAL);

	/* start quiet: sampling off, then flush any stale DRDY */
	regmap_write(fs->regmap, FS_REG_CTRL, 0);

	fs->irq = client->irq;	/* parsed from DT "interrupts" by the I2C core */
	if (fs->irq <= 0)
		return dev_err_probe(dev, -EINVAL, "no IRQ in device tree\n");

	/*
	 * No trigger flags here: the type (level-high) comes from DT.
	 * IRQF_ONESHOT is mandatory for a level line serviced in a thread.
	 */
	ret = devm_request_threaded_irq(dev, fs->irq, fs_irq_hard, fs_irq_thread,
					IRQF_ONESHOT, "fakesensor", fs);
	if (ret)
		return dev_err_probe(dev, ret, "request IRQ %d\n", fs->irq);

	fs->misc.minor = MISC_DYNAMIC_MINOR;
	fs->misc.name  = "fakesensor";
	fs->misc.fops  = &fs_fops;
	fs->misc.parent = dev;
	ret = misc_register(&fs->misc);
	if (ret)
		return dev_err_probe(dev, ret, "misc_register\n");

	ret = regmap_write(fs->regmap, FS_REG_CTRL, FS_CTRL_EN | FS_CTRL_INT_EN);
	if (ret) {
		misc_deregister(&fs->misc);
		return dev_err_probe(dev, ret, "enable\n");
	}

	dev_info(dev, "fakesensor ready, irq %d, /dev/%s\n", fs->irq, fs->misc.name);
	return 0;
}

static void fs_remove(struct i2c_client *client)
{
	struct fs_dev *fs = i2c_get_clientdata(client);

	regmap_write(fs->regmap, FS_REG_CTRL, 0);	/* stop DRDY first */
	misc_deregister(&fs->misc);
	/* devm then frees the IRQ (waits for the thread), then fs */
}

static const struct of_device_id fs_of_match[] = {
	{ .compatible = "subin,fakesensor" },
	{ }
};
MODULE_DEVICE_TABLE(of, fs_of_match);

static const struct i2c_device_id fs_id[] = {
	{ "fakesensor" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, fs_id);

static struct i2c_driver fs_driver = {
	.driver = {
		.name		= "fakesensor",
		.of_match_table	= fs_of_match,
		.dev_groups	= fs_groups,
	},
	.probe		= fs_probe,
	.remove		= fs_remove,
	.id_table	= fs_id,
};
module_i2c_driver(fs_driver);

MODULE_DESCRIPTION("Pico 2 fake I2C sensor with data-ready IRQ");
MODULE_AUTHOR("Subin Gangadharan");
MODULE_LICENSE("GPL");
