// SPDX-License-Identifier: GPL-2.0-only
/*
 * Attach-only remoteproc/RPMsg transport for the SG2002 C906L.
 *
 * The vendor FSBL starts immutable firmware before Linux.  This driver never
 * loads, starts, stops, or resets C906L; it maps the firmware-published
 * resource table and gives Linux remoteproc two fixed vrings and a dedicated
 * coherent RPMsg buffer pool.  AP mailbox channels 1 and 2 are unidirectional
 * doorbells so simultaneous kicks cannot overwrite one shared payload slot.
 */

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/mailbox_client.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/remoteproc.h>
#include <linux/rpmsg.h>

/* Exported by remoteproc_virtio.c but currently kept in its internal header. */
extern irqreturn_t rproc_vq_interrupt(struct rproc *rproc, int vq_id);

#define SG2002_C906L_MAGIC		0x4d564b4eU
#define SG2002_C906L_ABI_MAJOR		1U
#define SG2002_C906L_STATE_RUNNING	2U
#define SG2002_C906L_CAP_RPMSG		BIT_ULL(3)

#define SG2002_SHMEM_ADDRESS		@SHMEM_ADDRESS@
#define SG2002_SHMEM_SIZE		@SHMEM_SIZE@
#define SG2002_RSC_OFFSET		@RESOURCE_TABLE_OFFSET@
#define SG2002_RSC_SIZE			@RESOURCE_TABLE_SIZE@
#define SG2002_VRING0_OFFSET		@VRING0_OFFSET@
#define SG2002_VRING0_SIZE		@VRING0_SIZE@
#define SG2002_VRING1_OFFSET		@VRING1_OFFSET@
#define SG2002_VRING1_SIZE		@VRING1_SIZE@
#define SG2002_BUFFER_OFFSET		@BUFFER_OFFSET@
#define SG2002_BUFFER_SIZE		@BUFFER_SIZE@

#define SG2002_RSC_VDEV			3U
#define SG2002_VIRTIO_ID_RPMSG		7U
#define SG2002_VIRTIO_RPMSG_F_NS	BIT(0)
#define SG2002_VRING_ALIGN		4096U
#define SG2002_VRING_DESCRIPTORS	256U

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

struct sg2002_vring_resource {
	__le32 da;
	__le32 align;
	__le32 num;
	__le32 notifyid;
	__le32 pa;
} __packed;

struct sg2002_resource_snapshot {
	__le32 version;
	__le32 entries;
	__le32 reserved[2];
	__le32 offset;
	__le32 type;
	__le32 id;
	__le32 notifyid;
	__le32 device_features;
	__le32 guest_features;
	__le32 config_len;
	u8 status;
	u8 vring_count;
	u8 vdev_reserved[2];
	struct sg2002_vring_resource vrings[2];
} __packed;

static_assert(sizeof(struct sg2002_c906l_status) == 64);
static_assert(sizeof(struct sg2002_resource_snapshot) == 88);

struct sg2002_c906l_rproc {
	struct device *dev;
	struct rproc *rproc;
	struct mbox_client mbox_client;
	struct mbox_chan *kick_channel;
	struct mbox_chan *notify_channel;
	u8 __iomem *shared;
	phys_addr_t shared_pa;
	size_t shared_size;
	u64 kick_words[2];
	atomic64_t kicks_sent;
	atomic64_t kicks_dropped;
	atomic64_t notifications;
	int mailbox_irq;
	bool shutting_down;
};

static int sg2002_validate_status(struct sg2002_c906l_rproc *priv)
{
	struct sg2002_c906l_status status;

	memcpy_fromio(&status, priv->shared, sizeof(status));
	if (le32_to_cpu(status.magic) != SG2002_C906L_MAGIC ||
	    le16_to_cpu(status.abi_major) != SG2002_C906L_ABI_MAJOR ||
	    le32_to_cpu(status.struct_size) < sizeof(status) ||
	    le32_to_cpu(status.state) != SG2002_C906L_STATE_RUNNING ||
	    !(le64_to_cpu(status.capabilities) & SG2002_C906L_CAP_RPMSG))
		return -EPROBE_DEFER;

	return 0;
}

static int sg2002_validate_resource_table(struct sg2002_c906l_rproc *priv)
{
	struct sg2002_resource_snapshot table;

	memcpy_fromio(&table, priv->shared + SG2002_RSC_OFFSET,
		      sizeof(table));
	if (le32_to_cpu(table.version) != 1 ||
	    le32_to_cpu(table.entries) != 1 ||
	    le32_to_cpu(table.reserved[0]) != 0 ||
	    le32_to_cpu(table.reserved[1]) != 0 ||
	    le32_to_cpu(table.offset) != 20 ||
	    le32_to_cpu(table.type) != SG2002_RSC_VDEV ||
	    le32_to_cpu(table.id) != SG2002_VIRTIO_ID_RPMSG ||
	    !(le32_to_cpu(table.device_features) & SG2002_VIRTIO_RPMSG_F_NS) ||
	    le32_to_cpu(table.config_len) != 0 || table.vring_count != 2 ||
	    table.vdev_reserved[0] != 0 || table.vdev_reserved[1] != 0)
		return -EINVAL;

	if (le32_to_cpu(table.vrings[0].da) !=
		priv->shared_pa + SG2002_VRING0_OFFSET ||
	    le32_to_cpu(table.vrings[1].da) !=
		priv->shared_pa + SG2002_VRING1_OFFSET ||
	    le32_to_cpu(table.vrings[0].align) != SG2002_VRING_ALIGN ||
	    le32_to_cpu(table.vrings[1].align) != SG2002_VRING_ALIGN ||
	    le32_to_cpu(table.vrings[0].num) != SG2002_VRING_DESCRIPTORS ||
	    le32_to_cpu(table.vrings[1].num) != SG2002_VRING_DESCRIPTORS)
		return -EINVAL;

	return 0;
}

static void sg2002_mbox_receive(struct mbox_client *client, void *message)
{
	struct sg2002_c906l_rproc *priv =
		container_of(client, struct sg2002_c906l_rproc, mbox_client);
	u64 word;
	u32 notifyid;

	if (READ_ONCE(priv->shutting_down))
		return;

	memcpy(&word, message, sizeof(word));
	notifyid = (u32)word;
	atomic64_inc(&priv->notifications);
	if (notifyid > 1 || !priv->rproc || priv->rproc->max_notifyid < 0 ||
	    notifyid > priv->rproc->max_notifyid) {
		dev_warn_ratelimited(priv->dev,
			"ignoring invalid C906L virtqueue notification %u\n",
			notifyid);
		return;
	}

	rproc_vq_interrupt(priv->rproc, notifyid);
}

static int sg2002_attach(struct rproc *rproc)
{
	struct sg2002_c906l_rproc *priv = rproc->priv;

	return sg2002_validate_status(priv);
}

static int sg2002_detach(struct rproc *rproc)
{
	struct sg2002_c906l_rproc *priv = rproc->priv;

	/* remoteproc has restored the clean resource table (status == 0). */
	(void)mbox_send_message(priv->kick_channel, &priv->kick_words[0]);
	return 0;
}

static int sg2002_stop_transport(struct rproc *rproc)
{
	struct sg2002_c906l_rproc *priv = rproc->priv;

	/*
	 * The remoteproc core calls .stop while unwinding a failed attach and
	 * from rproc_del() if detach itself failed. Never reset the running core:
	 * only publish an offline vdev and wake its polling transport task.
	 */
	writeb(0, priv->shared + SG2002_RSC_OFFSET +
	       offsetof(struct sg2002_resource_snapshot, status));
	wmb();
	(void)mbox_send_message(priv->kick_channel, &priv->kick_words[0]);
	return 0;
}

static void sg2002_kick(struct rproc *rproc, int vqid)
{
	struct sg2002_c906l_rproc *priv = rproc->priv;
	int ret;

	if (vqid < 0 || vqid > 1) {
		atomic64_inc(&priv->kicks_dropped);
		return;
	}

	ret = mbox_send_message(priv->kick_channel, &priv->kick_words[vqid]);
	if (ret < 0)
		atomic64_inc(&priv->kicks_dropped);
	else
		atomic64_inc(&priv->kicks_sent);
}

static void *sg2002_da_to_va(struct rproc *rproc, u64 da, size_t len,
			     bool *is_iomem)
{
	struct sg2002_c906l_rproc *priv = rproc->priv;
	u64 offset;

	if (!len || da < priv->shared_pa)
		return NULL;
	offset = da - priv->shared_pa;
	if (offset > priv->shared_size || len > priv->shared_size - offset)
		return NULL;
	if (is_iomem)
		*is_iomem = true;

	return (__force void *)(priv->shared + offset);
}

static struct resource_table *
sg2002_get_loaded_resource_table(struct rproc *rproc, size_t *size)
{
	struct sg2002_c906l_rproc *priv = rproc->priv;

	*size = SG2002_RSC_SIZE;
	return (__force struct resource_table *)(priv->shared + SG2002_RSC_OFFSET);
}

static const struct rproc_ops sg2002_rproc_ops = {
	.attach = sg2002_attach,
	.detach = sg2002_detach,
	.stop = sg2002_stop_transport,
	.kick = sg2002_kick,
	.da_to_va = sg2002_da_to_va,
	.get_loaded_rsc_table = sg2002_get_loaded_resource_table,
};

static void sg2002_disable_notifications(struct sg2002_c906l_rproc *priv)
{
	WRITE_ONCE(priv->shutting_down, true);
	/*
	 * Leave the mailbox client installed while asking the polling firmware
	 * transport to go offline. One 5 ms RTOS tick is its worst-case polling
	 * interval; the kick normally makes this immediate. This order avoids a
	 * race in which the shared mailbox IRQ observes a client just as it is
	 * being freed.
	 */
	(void)sg2002_stop_transport(priv->rproc);
	msleep(20);
	if (priv->mailbox_irq >= 0)
		synchronize_irq(priv->mailbox_irq);
	if (priv->notify_channel) {
		mbox_free_channel(priv->notify_channel);
		priv->notify_channel = NULL;
	}
	/* Drain a threaded callback which passed the controller's client test. */
	if (priv->mailbox_irq >= 0)
		synchronize_irq(priv->mailbox_irq);
}

static void sg2002_unregister_rproc(struct sg2002_c906l_rproc *priv)
{
	int ret = 0;

	sg2002_disable_notifications(priv);
	if (priv->rproc->state == RPROC_ATTACHED) {
		ret = rproc_detach(priv->rproc);
		if (ret)
			dev_err(priv->dev,
				"detach failed (%d); forcing transport-only cleanup without resetting C906L\n",
				ret);
	}

	/*
	 * rproc_del() invokes the non-resetting .stop fallback if detach failed.
	 * Explicit cleanup is idempotent and covers a second allocation failure
	 * in the remoteproc core's resource-table shutdown path.
	 */
	rproc_del(priv->rproc);
	rproc_resource_cleanup(priv->rproc);
}

static ssize_t transport_stats_show(struct device *dev,
				    struct device_attribute *attribute,
				    char *buffer)
{
	struct sg2002_c906l_rproc *priv = dev_get_drvdata(dev);

	(void)attribute;
	return sysfs_emit(buffer,
			  "kicks_sent=%lld kicks_dropped=%lld notifications=%lld\n",
			  atomic64_read(&priv->kicks_sent),
			  atomic64_read(&priv->kicks_dropped),
			  atomic64_read(&priv->notifications));
}
static DEVICE_ATTR_RO(transport_stats);

static int sg2002_add_carveout(struct sg2002_c906l_rproc *priv,
			       const char *name, u64 offset, size_t size,
			       bool map_for_vring)
{
	struct rproc_mem_entry *entry;
	void *va = NULL;
	dma_addr_t dma = priv->shared_pa + offset;

	if (offset > priv->shared_size || size > priv->shared_size - offset)
		return -EINVAL;
	if (map_for_vring)
		va = (__force void *)(priv->shared + offset);

	entry = rproc_mem_entry_init(priv->dev, va, dma, size, (u32)dma,
				     NULL, NULL, "%s", name);
	if (!entry)
		return -ENOMEM;
	entry->is_iomem = map_for_vring;
	rproc_add_carveout(priv->rproc, entry);
	return 0;
}

static int sg2002_c906l_rproc_probe(struct platform_device *pdev)
{
	struct of_phandle_args mailbox_args;
	struct device_node *memory_node;
	struct sg2002_c906l_rproc *priv;
	struct resource memory;
	struct rproc *rproc;
	int ret;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;
	priv->dev = &pdev->dev;
	priv->mailbox_irq = -1;

	ret = of_parse_phandle_with_args(pdev->dev.of_node, "mboxes",
					 "#mbox-cells", 1, &mailbox_args);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "invalid vq-notify mailbox\n");
	priv->mailbox_irq = of_irq_get(mailbox_args.np, 0);
	of_node_put(mailbox_args.np);
	if (priv->mailbox_irq < 0)
		return dev_err_probe(&pdev->dev, priv->mailbox_irq,
				     "failed to resolve mailbox IRQ\n");

	memory_node = of_parse_phandle(pdev->dev.of_node, "memory-region", 0);
	if (!memory_node)
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "missing memory-region\n");
	ret = of_address_to_resource(memory_node, 0, &memory);
	of_node_put(memory_node);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "invalid memory-region\n");
	if (memory.start != SG2002_SHMEM_ADDRESS ||
	    resource_size(&memory) != SG2002_SHMEM_SIZE)
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "shared-memory contract mismatch\n");

	priv->shared_pa = memory.start;
	priv->shared_size = resource_size(&memory);
	priv->shared = devm_ioremap_wc(&pdev->dev, memory.start,
				       resource_size(&memory));
	if (!priv->shared)
		return dev_err_probe(&pdev->dev, -ENOMEM,
				     "failed to map shared memory\n");

	ret = sg2002_validate_status(priv);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "C906L RPMsg firmware is not ready\n");
	ret = sg2002_validate_resource_table(priv);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "invalid C906L resource table\n");

	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "failed to set 32-bit DMA mask\n");

	rproc = rproc_alloc(&pdev->dev, "sg2002-c906l", &sg2002_rproc_ops,
			    NULL, sizeof(*priv));
	if (!rproc)
		return -ENOMEM;
	/* rproc owns this copy after allocation; keep platform state canonical. */
	memcpy(rproc->priv, priv, sizeof(*priv));
	priv = rproc->priv;
	priv->rproc = rproc;
	priv->kick_words[0] = 0;
	priv->kick_words[1] = 1;
	priv->shutting_down = false;
	rproc->state = RPROC_DETACHED;
	rproc->auto_boot = true;
	rproc->sysfs_read_only = true;
	rproc->recovery_disabled = true;
	rproc->dump_conf = RPROC_COREDUMP_DISABLED;

	priv->mbox_client.dev = &pdev->dev;
	priv->mbox_client.rx_callback = sg2002_mbox_receive;
	priv->mbox_client.tx_block = false;
	priv->mbox_client.knows_txdone = false;
	priv->kick_channel = mbox_request_channel_byname(&priv->mbox_client,
							 "vq-kick");
	if (IS_ERR(priv->kick_channel)) {
		ret = PTR_ERR(priv->kick_channel);
		goto free_rproc;
	}
	priv->notify_channel = mbox_request_channel_byname(&priv->mbox_client,
							   "vq-notify");
	if (IS_ERR(priv->notify_channel)) {
		ret = PTR_ERR(priv->notify_channel);
		goto free_kick;
	}

	ret = sg2002_add_carveout(priv, "vdev0vring0",
				   SG2002_VRING0_OFFSET, SG2002_VRING0_SIZE,
				   true);
	if (ret)
		goto free_notify;
	ret = sg2002_add_carveout(priv, "vdev0vring1",
				   SG2002_VRING1_OFFSET, SG2002_VRING1_SIZE,
				   true);
	if (ret)
		goto free_notify;
	ret = sg2002_add_carveout(priv, "vdev0buffer", SG2002_BUFFER_OFFSET,
				   SG2002_BUFFER_SIZE, false);
	if (ret)
		goto free_notify;

	platform_set_drvdata(pdev, priv);
	ret = rproc_add(rproc);
	if (ret)
		goto free_notify;
	ret = device_create_file(&pdev->dev, &dev_attr_transport_stats);
	if (ret)
		goto del_rproc;

	dev_info(&pdev->dev,
		 "attached FSBL-started C906L with fixed noncoherent RPMsg memory\n");
	return 0;

del_rproc:
	sg2002_unregister_rproc(priv);
	goto free_kick;

free_notify:
	sg2002_disable_notifications(priv);
	rproc_resource_cleanup(rproc);
free_kick:
	mbox_free_channel(priv->kick_channel);
free_rproc:
	rproc_free(rproc);
	return dev_err_probe(&pdev->dev, ret,
			     "failed to attach C906L remoteproc\n");
}

static void sg2002_c906l_rproc_remove(struct platform_device *pdev)
{
	struct sg2002_c906l_rproc *priv = platform_get_drvdata(pdev);

	device_remove_file(&pdev->dev, &dev_attr_transport_stats);
	sg2002_unregister_rproc(priv);
	mbox_free_channel(priv->kick_channel);
	rproc_free(priv->rproc);
}

static const struct of_device_id sg2002_c906l_rproc_of_match[] = {
	{ .compatible = "sophgo,sg2002-c906l-rproc" },
	{ }
};
MODULE_DEVICE_TABLE(of, sg2002_c906l_rproc_of_match);

static struct platform_driver sg2002_c906l_rproc_driver = {
	.probe = sg2002_c906l_rproc_probe,
	.remove = sg2002_c906l_rproc_remove,
	.driver = {
		.name = "sg2002-c906l-rproc",
		.of_match_table = sg2002_c906l_rproc_of_match,
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(sg2002_c906l_rproc_driver);

MODULE_DESCRIPTION("Attach-only SG2002 C906L remoteproc/RPMsg transport");
MODULE_AUTHOR("nixos-nanokvm contributors");
MODULE_LICENSE("GPL");
