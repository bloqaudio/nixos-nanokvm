// SPDX-License-Identifier: GPL-2.0-only
/*
 * Attach-only remoteproc/RPMsg transport for the SG2002 C906L.
 *
 * The vendor FSBL starts immutable firmware before Linux.  This driver never
 * loads, starts, stops, or resets C906L; it maps the firmware-published
 * resource table and gives Linux remoteproc two fixed vrings and a dedicated
 * noncoherent RPMsg buffer pool. AP mailbox channels 1 and 2 are
 * unidirectional doorbells so simultaneous kicks cannot overwrite one shared
 * payload slot.
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
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/remoteproc.h>
#include <linux/rpmsg.h>
#include <linux/string.h>

#include "sg2002-c906l-kernel-contract.h"

/* Exported by remoteproc_virtio.c but currently kept in its internal header. */
extern irqreturn_t rproc_vq_interrupt(struct rproc *rproc, int vq_id);

struct sg2002_contract_snapshot {
	struct sg2002_c906l_status status;
	struct sg2002_c906l_manifest manifest;
};

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

static const u8 sg2002_contract_digest[32] =
	SG2002_C906L_CONTRACT_SHA256_BYTES;

static int sg2002_dt_u32(struct device_node *node, const char *name,
			 u32 expected)
{
	const struct property *property;
	u32 value;
	int length;

	property = of_find_property(node, name, &length);
	if (!property || length != sizeof(__be32) ||
	    of_property_read_u32(node, name, &value) || value != expected)
		return -EINVAL;
	return 0;
}

static int sg2002_dt_u64(struct device_node *node, const char *name,
			 u64 expected)
{
	const struct property *property;
	u64 value;
	int length;

	property = of_find_property(node, name, &length);
	if (!property || length != sizeof(__be64) ||
	    of_property_read_u64(node, name, &value) || value != expected)
		return -EINVAL;
	return 0;
}

static int sg2002_validate_dt_identity(struct device *dev)
{
	struct device_node *node = dev->of_node;
	const struct property *property;
	const char *profile;
	const u8 *digest;
	u32 abi_version = (SG2002_C906L_ABI_MAJOR << 16) |
		SG2002_C906L_ABI_MINOR;
	bool activation_required;
	int length;

	digest = of_get_property(node, "sophgo,contract-sha256", &length);
	if (!digest || length != sizeof(sg2002_contract_digest) ||
	    memcmp(digest, sg2002_contract_digest, sizeof(sg2002_contract_digest)))
		return dev_err_probe(dev, -EINVAL,
				     "DT contract digest mismatch\n");
	if (sg2002_dt_u32(node, "sophgo,contract-epoch",
			  SG2002_C906L_CONTRACT_EPOCH) ||
	    sg2002_dt_u32(node, "sophgo,abi-version", abi_version) ||
	    sg2002_dt_u64(node, "sophgo,expected-capabilities",
			  SG2002_C906L_EXPECTED_CAPABILITIES) ||
	    sg2002_dt_u64(node, "sophgo,dormant-capabilities",
			  SG2002_C906L_DORMANT_CAPABILITIES) ||
	    sg2002_dt_u64(node, "sophgo,lease-mask",
			  SG2002_C906L_LEASE_MASK) ||
	    sg2002_dt_u32(node, "sophgo,profile-id", SG2002_C906L_PROFILE_ID) ||
	    sg2002_dt_u32(node, "sophgo,manifest-flags",
			  SG2002_C906L_MANIFEST_FLAGS))
		return dev_err_probe(dev, -EINVAL,
				     "DT contract scalar mismatch\n");

	property = of_find_property(node, "sophgo,profile", &length);
	if (!property || length != sizeof(SG2002_C906L_PROFILE_NAME) ||
	    of_property_read_string(node, "sophgo,profile", &profile) ||
	    strcmp(profile, SG2002_C906L_PROFILE_NAME))
		return dev_err_probe(dev, -EINVAL,
				     "DT contract profile mismatch\n");
	activation_required =
		of_property_read_bool(node, "sophgo,activation-required");
	if (activation_required != !!SG2002_C906L_ACTIVATION_REQUIRED)
		return dev_err_probe(dev, -EINVAL,
				     "DT activation policy mismatch\n");
	return 0;
}

static int sg2002_validate_manifest(
		const struct sg2002_c906l_manifest *manifest)
{
	if (le32_to_cpu(manifest->magic) != SG2002_C906L_MANIFEST_MAGIC ||
	    le16_to_cpu(manifest->format_major) !=
		SG2002_C906L_MANIFEST_FORMAT_MAJOR ||
	    le16_to_cpu(manifest->format_minor) !=
		SG2002_C906L_MANIFEST_FORMAT_MINOR ||
	    le32_to_cpu(manifest->struct_size) != SG2002_C906L_MANIFEST_SIZE ||
	    le32_to_cpu(manifest->contract_epoch) !=
		SG2002_C906L_CONTRACT_EPOCH ||
	    le32_to_cpu(manifest->profile_id) != SG2002_C906L_PROFILE_ID ||
	    le16_to_cpu(manifest->abi_major) != SG2002_C906L_ABI_MAJOR ||
	    le16_to_cpu(manifest->abi_minor) != SG2002_C906L_ABI_MINOR ||
	    le16_to_cpu(manifest->capability_width) !=
		SG2002_C906L_CAPABILITY_WIRE_WIDTH ||
	    le16_to_cpu(manifest->lease_width) != SG2002_C906L_LEASE_WIRE_WIDTH ||
	    le64_to_cpu(manifest->final_capabilities) !=
		SG2002_C906L_EXPECTED_CAPABILITIES ||
	    le64_to_cpu(manifest->dormant_capabilities) !=
		SG2002_C906L_DORMANT_CAPABILITIES ||
	    le64_to_cpu(manifest->lease_mask) != SG2002_C906L_LEASE_MASK ||
	    le32_to_cpu(manifest->flags) != SG2002_C906L_MANIFEST_FLAGS ||
	    le32_to_cpu(manifest->reserved0) != 0 ||
	    memcmp(manifest->contract_sha256, sg2002_contract_digest,
		   sizeof(sg2002_contract_digest)) ||
	    memchr_inv(manifest->reserved1, 0, sizeof(manifest->reserved1)) ||
	    le32_to_cpu(manifest->commit) != SG2002_C906L_MANIFEST_COMMIT)
		return -EPROTO;
	return 0;
}

static int sg2002_read_contract_snapshot(struct sg2002_c906l_rproc *priv,
					 struct sg2002_contract_snapshot *snapshot)
{
	struct sg2002_c906l_status before;
	struct sg2002_c906l_manifest first;
	struct sg2002_c906l_manifest second;
	u64 capabilities;
	u32 flags;
	u32 generation;
	u32 request_id;
	u16 attempts;
	u8 activation_error;
	int ret;

	memcpy_fromio(&before, priv->shared + SG2002_C906L_STATUS_REGION_OFFSET,
		      sizeof(before));
	rmb();
	memcpy_fromio(&first, priv->shared + SG2002_C906L_MANIFEST_OFFSET,
		      sizeof(first));
	rmb();
	memcpy_fromio(&second, priv->shared + SG2002_C906L_MANIFEST_OFFSET,
		      sizeof(second));
	rmb();
	memcpy_fromio(&snapshot->status,
		      priv->shared + SG2002_C906L_STATUS_REGION_OFFSET,
		      sizeof(snapshot->status));
	if (memcmp(&first, &second, sizeof(first)) ||
	    memcmp(&before, &snapshot->status, sizeof(before)))
		return -EAGAIN;
	generation = le32_to_cpu(snapshot->status.generation);
	if (le32_to_cpu(first.generation) != generation)
		return -EAGAIN;
	if (le32_to_cpu(snapshot->status.magic) != SG2002_C906L_SHMEM_MAGIC)
		return -EAGAIN;
	if (le16_to_cpu(snapshot->status.abi_major) != SG2002_C906L_ABI_MAJOR ||
	    le16_to_cpu(snapshot->status.abi_minor) != SG2002_C906L_ABI_MINOR ||
	    le32_to_cpu(snapshot->status.struct_size) != SG2002_C906L_STATUS_SIZE)
		return -EAGAIN;
	if (le32_to_cpu(snapshot->status.state) == SG2002_C906L_STATE_BOOTING)
		return -EAGAIN;
	if (le32_to_cpu(snapshot->status.state) != SG2002_C906L_STATE_RUNNING)
		return -EREMOTEIO;
	ret = sg2002_validate_manifest(&first);
	if (ret)
		return ret;

	capabilities = le64_to_cpu(snapshot->status.capabilities);
	flags = le32_to_cpu(snapshot->status.flags);
	attempts = le16_to_cpu(snapshot->status.activation_attempts);
	request_id = le32_to_cpu(snapshot->status.activation_request_id);
	activation_error = snapshot->status.activation_error;
	if (snapshot->status.activation_state ==
	    SG2002_C906L_ACTIVATION_STATE_ACTIVE) {
		if (activation_error != SG2002_C906L_ACTIVATION_RESULT_SUCCESS ||
		    (SG2002_C906L_ACTIVATION_REQUIRED ?
		     (!attempts || !request_id) : (attempts || request_id)) ||
		    (SG2002_C906L_ACTIVATION_REQUIRED ?
		     (flags & SG2002_C906L_FLAG_ACTIVATION_FAILED) :
		     (flags & (SG2002_C906L_FLAG_ACTIVATION_REJECTED |
			       SG2002_C906L_FLAG_ACTIVATION_FAILED))) ||
		    capabilities != SG2002_C906L_EXPECTED_CAPABILITIES)
			return -EAGAIN;
	} else if (snapshot->status.activation_state ==
		   SG2002_C906L_ACTIVATION_STATE_DORMANT) {
		if (!SG2002_C906L_ACTIVATION_REQUIRED ||
		    activation_error != SG2002_C906L_ACTIVATION_RESULT_SUCCESS ||
		    attempts || request_id ||
		    !(SG2002_C906L_MANIFEST_FLAGS &
		      SG2002_C906L_MANIFEST_FLAG_ACK_REQUIRED) ||
		    !(SG2002_C906L_MANIFEST_FLAGS &
		      SG2002_C906L_MANIFEST_FLAG_RPMSG_WHILE_DORMANT) ||
		    (flags & (SG2002_C906L_FLAG_ACTIVATION_REJECTED |
			      SG2002_C906L_FLAG_ACTIVATION_FAILED)) ||
		    capabilities != SG2002_C906L_DORMANT_CAPABILITIES)
			return -EAGAIN;
	} else if (snapshot->status.activation_state ==
		   SG2002_C906L_ACTIVATION_STATE_REJECTED) {
		if (!SG2002_C906L_ACTIVATION_REQUIRED || !attempts ||
		    activation_error <
			SG2002_C906L_ACTIVATION_RESULT_INVALID_ENVELOPE ||
		    activation_error >
			SG2002_C906L_ACTIVATION_RESULT_LEASE_MASK_MISMATCH ||
		    (!request_id && activation_error !=
		     SG2002_C906L_ACTIVATION_RESULT_INVALID_ENVELOPE) ||
		    !(SG2002_C906L_MANIFEST_FLAGS &
		      SG2002_C906L_MANIFEST_FLAG_RPMSG_WHILE_DORMANT) ||
		    !(flags & SG2002_C906L_FLAG_ACTIVATION_REJECTED) ||
		    (flags & SG2002_C906L_FLAG_ACTIVATION_FAILED) ||
		    capabilities != SG2002_C906L_DORMANT_CAPABILITIES)
			return -EAGAIN;
	} else if (snapshot->status.activation_state ==
		   SG2002_C906L_ACTIVATION_STATE_LEASE_FAULT) {
		if (!SG2002_C906L_ACTIVATION_REQUIRED || !attempts || !request_id ||
		    activation_error <
			SG2002_C906L_ACTIVATION_RESULT_PRECONDITION_FAILED ||
		    activation_error >
			SG2002_C906L_ACTIVATION_RESULT_INTERNAL_FAILURE ||
		    !(SG2002_C906L_MANIFEST_FLAGS &
		      SG2002_C906L_MANIFEST_FLAG_RPMSG_WHILE_DORMANT) ||
		    !(flags & SG2002_C906L_FLAG_ACTIVATION_FAILED) ||
		    capabilities != SG2002_C906L_DORMANT_CAPABILITIES)
			return -EAGAIN;
	} else {
		/* In-progress activation states are transient and retried. */
		return -EAGAIN;
	}
	if (!(capabilities & SG2002_C906L_CAP_RPMSG))
		return -EAGAIN;

	snapshot->manifest = first;
	return 0;
}

static int sg2002_validate_resource_table(struct sg2002_c906l_rproc *priv)
{
	struct sg2002_c906l_resource_snapshot first;
	struct sg2002_c906l_resource_snapshot second;
	unsigned int index;

	memcpy_fromio(&first,
		      priv->shared + SG2002_C906L_RESOURCE_TABLE_REGION_OFFSET,
		      sizeof(first));
	rmb();
	memcpy_fromio(&second,
		      priv->shared + SG2002_C906L_RESOURCE_TABLE_REGION_OFFSET,
		      sizeof(second));
	if (memcmp(&first, &second, sizeof(first)))
		return -EAGAIN;
	if (le32_to_cpu(first.version) != SG2002_C906L_RSC_TABLE_VERSION ||
	    le32_to_cpu(first.entries) != SG2002_C906L_RSC_TABLE_ENTRIES ||
	    le32_to_cpu(first.reserved[0]) != 0 ||
	    le32_to_cpu(first.reserved[1]) != 0 ||
	    le32_to_cpu(first.offset) != SG2002_C906L_RSC_TABLE_ENTRY_OFFSET ||
	    le32_to_cpu(first.resource_type) != SG2002_C906L_RSC_VDEV ||
	    le32_to_cpu(first.device_id) != SG2002_C906L_VIRTIO_ID_RPMSG ||
	    le32_to_cpu(first.notify_id) !=
		SG2002_C906L_RSC_VDEV_NOTIFY_ID_INITIAL ||
	    le32_to_cpu(first.device_features) !=
		SG2002_C906L_VIRTIO_RPMSG_FEATURES ||
	    le32_to_cpu(first.guest_features) !=
		SG2002_C906L_RSC_GUEST_FEATURES_INITIAL ||
	    le32_to_cpu(first.config_length) != SG2002_C906L_RSC_CONFIG_LENGTH ||
	    first.status != SG2002_C906L_RSC_STATUS_INITIAL ||
	    first.vring_count != SG2002_C906L_RSC_VRING_COUNT ||
	    first.vdev_reserved[0] != 0 || first.vdev_reserved[1] != 0)
		return -EINVAL;

	for (index = 0; index < SG2002_C906L_RSC_VRING_COUNT; ++index) {
		u64 expected_address = index ? SG2002_C906L_RPMSG_VRING1_REGION_ADDRESS :
			SG2002_C906L_RPMSG_VRING0_REGION_ADDRESS;

		if (le32_to_cpu(first.vrings[index].device_address) !=
			expected_address ||
		    le32_to_cpu(first.vrings[index].align) !=
			SG2002_C906L_VRING_ALIGN ||
		    le32_to_cpu(first.vrings[index].descriptors) !=
			SG2002_C906L_VRING_DESCRIPTORS ||
		    le32_to_cpu(first.vrings[index].notify_id) !=
			SG2002_C906L_VRING_NOTIFY_ID_INITIAL ||
		    le32_to_cpu(first.vrings[index].physical_address) !=
			SG2002_C906L_VRING_PHYSICAL_ADDRESS_INITIAL)
			return -EINVAL;
	}

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
	struct sg2002_contract_snapshot snapshot;

	return sg2002_read_contract_snapshot(priv, &snapshot);
}

static int sg2002_detach(struct rproc *rproc)
{
	struct sg2002_c906l_rproc *priv = rproc->priv;

	/* Publish the core-restored clean table before notifying firmware. */
	wmb();
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
	writeb(0, priv->shared + SG2002_C906L_RESOURCE_TABLE_REGION_OFFSET +
	       offsetof(struct sg2002_c906l_resource_snapshot, status));
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

	*size = SG2002_C906L_RSC_TABLE_SERIALIZED_SIZE;
	return (__force struct resource_table *)(priv->shared +
		SG2002_C906L_RESOURCE_TABLE_REGION_OFFSET);
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

	/* rproc_del() invokes the non-resetting .stop fallback if detach failed. */
	rproc_del(priv->rproc);
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

static ssize_t contract_state_show(struct device *dev,
				   struct device_attribute *attribute,
				   char *buffer)
{
	struct sg2002_c906l_rproc *priv = dev_get_drvdata(dev);
	struct sg2002_contract_snapshot snapshot;
	int ret;

	(void)attribute;
	ret = sg2002_read_contract_snapshot(priv, &snapshot);
	if (ret)
		return sysfs_emit(buffer, "unavailable error=%d\n", ret);
	return sysfs_emit(buffer,
		"profile=%s profile_id=%u sha256=%s generation=%u "
		"activation_state=%u capabilities=0x%016llx\n",
		SG2002_C906L_PROFILE_NAME, SG2002_C906L_PROFILE_ID,
		SG2002_C906L_CONTRACT_SHA256,
		le32_to_cpu(snapshot.status.generation),
		snapshot.status.activation_state,
		le64_to_cpu(snapshot.status.capabilities));
}
static DEVICE_ATTR_RO(contract_state);

static struct attribute *sg2002_c906l_rproc_attributes[] = {
	&dev_attr_transport_stats.attr,
	&dev_attr_contract_state.attr,
	NULL,
};

static const struct attribute_group sg2002_c906l_rproc_attribute_group = {
	.attrs = sg2002_c906l_rproc_attributes,
};

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
	struct sg2002_contract_snapshot snapshot;
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

	ret = sg2002_validate_dt_identity(&pdev->dev);
	if (ret)
		return ret;
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
	if (memory.start != SG2002_C906L_SHMEM_ADDRESS ||
	    resource_size(&memory) != SG2002_C906L_SHMEM_SIZE)
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "shared-memory contract mismatch\n");

	priv->shared_pa = memory.start;
	priv->shared_size = resource_size(&memory);
	priv->shared = devm_ioremap_wc(&pdev->dev, memory.start,
				       resource_size(&memory));
	if (!priv->shared)
		return dev_err_probe(&pdev->dev, -ENOMEM,
				     "failed to map shared memory\n");

	ret = sg2002_read_contract_snapshot(priv, &snapshot);
	if (ret)
		return dev_err_probe(&pdev->dev,
			ret == -EAGAIN ? -EPROBE_DEFER : ret,
			"C906L RPMsg manifest is not ready or does not match\n");
	ret = sg2002_validate_resource_table(priv);
	if (ret)
		return dev_err_probe(&pdev->dev,
			ret == -EAGAIN ? -EPROBE_DEFER : ret,
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
				   SG2002_C906L_RPMSG_VRING0_REGION_OFFSET,
				   SG2002_C906L_RPMSG_VRING0_REGION_SIZE,
				   true);
	if (ret)
		goto free_notify;
	ret = sg2002_add_carveout(priv, "vdev0vring1",
				   SG2002_C906L_RPMSG_VRING1_REGION_OFFSET,
				   SG2002_C906L_RPMSG_VRING1_REGION_SIZE,
				   true);
	if (ret)
		goto free_notify;
	ret = sg2002_add_carveout(priv, "vdev0buffer",
				   SG2002_C906L_RPMSG_BUFFER_REGION_OFFSET,
				   SG2002_C906L_RPMSG_BUFFER_REGION_SIZE, false);
	if (ret)
		goto free_notify;

	platform_set_drvdata(pdev, priv);
	ret = rproc_add(rproc);
	if (ret)
		goto free_notify;
	ret = sysfs_create_group(&pdev->dev.kobj,
				 &sg2002_c906l_rproc_attribute_group);
	if (ret)
		goto del_rproc;

	dev_info(&pdev->dev,
		 "attached FSBL-started C906L %s contract (%s) with fixed noncoherent RPMsg memory\n",
		 SG2002_C906L_PROFILE_NAME, SG2002_C906L_CONTRACT_SHA256);
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

	sysfs_remove_group(&pdev->dev.kobj,
			   &sg2002_c906l_rproc_attribute_group);
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
