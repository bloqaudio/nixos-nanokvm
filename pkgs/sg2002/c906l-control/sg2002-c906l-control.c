// SPDX-License-Identifier: GPL-2.0-only
/*
 * Attach-only control endpoint for an SG2002 C906L started by the FSBL.
 *
 * The character device transports exactly one little-endian 64-bit mailbox
 * word per write/read transaction.  Bulk data belongs in shared DDR and will
 * use remoteproc/rpmsg once a safe reset/quiesce lifecycle is available.
 */

#include <linux/atomic.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/jiffies.h>
#include <linux/mailbox_client.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/poll.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

#define SG2002_C906L_MAGIC	0x4d564b4eU
#define SG2002_C906L_ABI_MAJOR	1U
#define SG2002_C906L_STATUS_SIZE	64U
#define SG2002_C906L_CLOSE_MS	100U

struct sg2002_c906l_status {
	__le32 magic;
	__le16 abi_major;
	__le16 abi_minor;
	__le32 struct_size;
	__le32 state;
	__le32 generation;
	__le32 flags;
	__le64 heartbeat;
	__le64 capabilities;
	__le64 last_request;
	__le64 last_response;
	u8 reserved[8];
} __packed;

static_assert(sizeof(struct sg2002_c906l_status) == SG2002_C906L_STATUS_SIZE);

struct sg2002_c906l {
	struct device *dev;
	struct mbox_client client;
	struct mbox_chan *channel;
	struct miscdevice misc;
	void __iomem *status;
	spinlock_t state_lock;
	struct mutex io_lock;
	wait_queue_head_t response_wait;
	atomic_t opened;
	u64 response;
	bool request_pending;
	bool response_ready;
};

static void sg2002_c906l_receive(struct mbox_client *client, void *message)
{
	struct sg2002_c906l *ctl =
		container_of(client, struct sg2002_c906l, client);
	unsigned long flags;
	u64 response;

	memcpy(&response, message, sizeof(response));
	spin_lock_irqsave(&ctl->state_lock, flags);
	if (ctl->request_pending) {
		ctl->response = response;
		ctl->request_pending = false;
		ctl->response_ready = true;
	}
	spin_unlock_irqrestore(&ctl->state_lock, flags);
	wake_up_interruptible(&ctl->response_wait);
}

static int sg2002_c906l_open(struct inode *inode, struct file *file)
{
	struct miscdevice *misc = file->private_data;
	struct sg2002_c906l *ctl =
		container_of(misc, struct sg2002_c906l, misc);
	unsigned long flags;

	if (atomic_cmpxchg(&ctl->opened, 0, 1))
		return -EBUSY;

	spin_lock_irqsave(&ctl->state_lock, flags);
	ctl->request_pending = false;
	ctl->response_ready = false;
	spin_unlock_irqrestore(&ctl->state_lock, flags);
	file->private_data = ctl;
	return nonseekable_open(inode, file);
}

static int sg2002_c906l_release(struct inode *inode, struct file *file)
{
	struct sg2002_c906l *ctl = file->private_data;
	unsigned long flags;

	(void)inode;
	if (READ_ONCE(ctl->request_pending))
		wait_event_timeout(ctl->response_wait,
				   !READ_ONCE(ctl->request_pending),
				   msecs_to_jiffies(SG2002_C906L_CLOSE_MS));

	spin_lock_irqsave(&ctl->state_lock, flags);
	ctl->request_pending = false;
	ctl->response_ready = false;
	spin_unlock_irqrestore(&ctl->state_lock, flags);
	atomic_set(&ctl->opened, 0);
	return 0;
}

static ssize_t sg2002_c906l_write(struct file *file, const char __user *buffer,
				  size_t length, loff_t *offset)
{
	struct sg2002_c906l *ctl = file->private_data;
	unsigned long flags;
	u64 request;
	int ret;

	if (length != sizeof(request))
		return -EMSGSIZE;
	if (copy_from_user(&request, buffer, sizeof(request)))
		return -EFAULT;

	mutex_lock(&ctl->io_lock);
	spin_lock_irqsave(&ctl->state_lock, flags);
	if (ctl->request_pending || ctl->response_ready) {
		spin_unlock_irqrestore(&ctl->state_lock, flags);
		mutex_unlock(&ctl->io_lock);
		return -EBUSY;
	}
	ctl->request_pending = true;
	spin_unlock_irqrestore(&ctl->state_lock, flags);

	ret = mbox_send_message(ctl->channel, &request);
	if (ret < 0) {
		spin_lock_irqsave(&ctl->state_lock, flags);
		ctl->request_pending = false;
		spin_unlock_irqrestore(&ctl->state_lock, flags);
		wake_up_interruptible(&ctl->response_wait);
	}
	mutex_unlock(&ctl->io_lock);
	return ret < 0 ? ret : sizeof(request);
}

static ssize_t sg2002_c906l_read(struct file *file, char __user *buffer,
				 size_t length, loff_t *offset)
{
	struct sg2002_c906l *ctl = file->private_data;
	unsigned long flags;
	u64 response;
	int ret;

	if (length < sizeof(response))
		return -EMSGSIZE;
	if ((file->f_flags & O_NONBLOCK) && !READ_ONCE(ctl->response_ready))
		return -EAGAIN;

	ret = wait_event_interruptible(ctl->response_wait,
				       READ_ONCE(ctl->response_ready));
	if (ret)
		return ret;

	mutex_lock(&ctl->io_lock);
	spin_lock_irqsave(&ctl->state_lock, flags);
	if (!ctl->response_ready) {
		spin_unlock_irqrestore(&ctl->state_lock, flags);
		mutex_unlock(&ctl->io_lock);
		return -EAGAIN;
	}
	response = ctl->response;
	ctl->response_ready = false;
	spin_unlock_irqrestore(&ctl->state_lock, flags);
	mutex_unlock(&ctl->io_lock);

	if (copy_to_user(buffer, &response, sizeof(response)))
		return -EFAULT;
	return sizeof(response);
}

static __poll_t sg2002_c906l_poll(struct file *file, poll_table *wait)
{
	struct sg2002_c906l *ctl = file->private_data;
	__poll_t mask = 0;

	poll_wait(file, &ctl->response_wait, wait);
	if (READ_ONCE(ctl->response_ready))
		mask |= EPOLLIN | EPOLLRDNORM;
	if (!READ_ONCE(ctl->request_pending) && !READ_ONCE(ctl->response_ready))
		mask |= EPOLLOUT | EPOLLWRNORM;
	return mask;
}

static const struct file_operations sg2002_c906l_fops = {
	.owner = THIS_MODULE,
	.open = sg2002_c906l_open,
	.release = sg2002_c906l_release,
	.read = sg2002_c906l_read,
	.write = sg2002_c906l_write,
	.poll = sg2002_c906l_poll,
};

static ssize_t status_show(struct device *dev, struct device_attribute *attr,
			   char *buffer)
{
	struct sg2002_c906l *ctl = dev_get_drvdata(dev);
	struct sg2002_c906l_status status;

	(void)attr;
	memcpy_fromio(&status, ctl->status, sizeof(status));
	if (le32_to_cpu(status.magic) != SG2002_C906L_MAGIC ||
	    le32_to_cpu(status.struct_size) < sizeof(status))
		return sysfs_emit(buffer, "unavailable\n");
	if (le16_to_cpu(status.abi_major) != SG2002_C906L_ABI_MAJOR)
		return sysfs_emit(buffer, "unsupported abi=%u.%u\n",
			le16_to_cpu(status.abi_major),
			le16_to_cpu(status.abi_minor));

	return sysfs_emit(buffer,
		"abi=%u.%u state=%u generation=%u flags=%#x heartbeat=%llu "
		"capabilities=%#llx last_request=%#llx last_response=%#llx\n",
		le16_to_cpu(status.abi_major), le16_to_cpu(status.abi_minor),
		le32_to_cpu(status.state), le32_to_cpu(status.generation),
		le32_to_cpu(status.flags), le64_to_cpu(status.heartbeat),
		le64_to_cpu(status.capabilities),
		le64_to_cpu(status.last_request),
		le64_to_cpu(status.last_response));
}
static DEVICE_ATTR_RO(status);

static int sg2002_c906l_probe(struct platform_device *pdev)
{
	struct device_node *memory_node;
	struct sg2002_c906l *ctl;
	struct resource memory;
	int ret;

	ctl = devm_kzalloc(&pdev->dev, sizeof(*ctl), GFP_KERNEL);
	if (!ctl)
		return -ENOMEM;
	ctl->dev = &pdev->dev;
	spin_lock_init(&ctl->state_lock);
	mutex_init(&ctl->io_lock);
	init_waitqueue_head(&ctl->response_wait);
	atomic_set(&ctl->opened, 0);

	memory_node = of_parse_phandle(pdev->dev.of_node, "memory-region", 0);
	if (!memory_node)
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "missing memory-region\n");
	ret = of_address_to_resource(memory_node, 0, &memory);
	of_node_put(memory_node);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "invalid memory-region\n");
	if (resource_size(&memory) < sizeof(struct sg2002_c906l_status))
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "memory-region is too small\n");
	ctl->status = devm_ioremap(&pdev->dev, memory.start, resource_size(&memory));
	if (!ctl->status)
		return dev_err_probe(&pdev->dev, -ENOMEM,
				     "failed to map shared status\n");

	ctl->client.dev = &pdev->dev;
	ctl->client.rx_callback = sg2002_c906l_receive;
	ctl->client.tx_block = false;
	ctl->client.knows_txdone = false;
	ctl->channel = mbox_request_channel_byname(&ctl->client, "control");
	if (IS_ERR(ctl->channel))
		return dev_err_probe(&pdev->dev, PTR_ERR(ctl->channel),
				     "failed to request control mailbox\n");

	ctl->misc.minor = MISC_DYNAMIC_MINOR;
	ctl->misc.name = "sg2002-c906l-control";
	ctl->misc.fops = &sg2002_c906l_fops;
	ctl->misc.parent = &pdev->dev;
	platform_set_drvdata(pdev, ctl);

	ret = misc_register(&ctl->misc);
	if (ret) {
		mbox_free_channel(ctl->channel);
		return dev_err_probe(&pdev->dev, ret,
				     "failed to register control device\n");
	}
	ret = device_create_file(&pdev->dev, &dev_attr_status);
	if (ret) {
		misc_deregister(&ctl->misc);
		mbox_free_channel(ctl->channel);
		return ret;
	}

	return 0;
}

static void sg2002_c906l_remove(struct platform_device *pdev)
{
	struct sg2002_c906l *ctl = platform_get_drvdata(pdev);

	device_remove_file(&pdev->dev, &dev_attr_status);
	misc_deregister(&ctl->misc);
	mbox_free_channel(ctl->channel);
}

static const struct of_device_id sg2002_c906l_of_match[] = {
	{ .compatible = "sophgo,sg2002-c906l-control" },
	{ }
};
MODULE_DEVICE_TABLE(of, sg2002_c906l_of_match);

static struct platform_driver sg2002_c906l_driver = {
	.probe = sg2002_c906l_probe,
	.remove = sg2002_c906l_remove,
	.driver = {
		.name = "sg2002-c906l-control",
		.of_match_table = sg2002_c906l_of_match,
		/* Static firmware endpoint: never unbind it underneath an open fd. */
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(sg2002_c906l_driver);

MODULE_DESCRIPTION("SG2002 C906L mailbox control endpoint");
MODULE_AUTHOR("nixos-nanokvm contributors");
MODULE_LICENSE("GPL");
