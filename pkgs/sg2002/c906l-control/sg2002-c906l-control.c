// SPDX-License-Identifier: GPL-2.0-only
/*
 * Attach-only control endpoint for an SG2002 C906L started by the FSBL.
 *
 * Firmware, DT and this module are built from one generated contract.  The
 * module will not expose the endpoint until the immutable firmware manifest
 * and the DT identity both match that contract exactly.  It is the sole Linux
 * writer of the activation-request cache line; raw userspace can use the
 * remaining control opcodes but can never authorize a peripheral lease.
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
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/poll.h>
#include <linux/random.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

#include "sg2002-c906l-kernel-contract.h"

#define SG2002_C906L_CLOSE_MS	100U
#define SG2002_C906L_ACTIVATE_RESPONSE	\
	(SG2002_C906L_OP_RESPONSE | SG2002_C906L_OP_ACTIVATE_LEASES)

enum sg2002_activation_outcome {
	SG2002_ACTIVATION_NOT_REQUIRED,
	SG2002_ACTIVATION_ALREADY_ACTIVE,
	SG2002_ACTIVATION_COMPLETED,
	SG2002_ACTIVATION_RECOVERED,
};

struct sg2002_contract_snapshot {
	struct sg2002_c906l_status status;
	struct sg2002_c906l_manifest manifest;
};

struct sg2002_c906l {
	struct device *dev;
	struct mbox_client client;
	struct mbox_chan *channel;
	struct miscdevice misc;
	u8 __iomem *shared;
	spinlock_t state_lock;
	struct mutex io_lock;
	wait_queue_head_t response_wait;
	atomic_t opened;
	u64 response;
	u64 tx_word;
	int tx_status;
	u16 activation_sequence;
	u16 expected_sequence;
	u8 expected_service;
	u8 expected_opcode;
	bool activation_waiting;
	bool request_pending;
	bool response_ready;
	bool tx_pending;
	enum sg2002_activation_outcome activation_outcome;
};

static const u8 sg2002_contract_digest[32] =
	SG2002_C906L_CONTRACT_SHA256_BYTES;

static const char *sg2002_activation_outcome_name(
		enum sg2002_activation_outcome outcome)
{
	switch (outcome) {
	case SG2002_ACTIVATION_NOT_REQUIRED:
		return "not-required";
	case SG2002_ACTIVATION_ALREADY_ACTIVE:
		return "already-active";
	case SG2002_ACTIVATION_COMPLETED:
		return "completed";
	case SG2002_ACTIVATION_RECOVERED:
		return "recovered-lost-response";
	default:
		return "unknown";
	}
}

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
	    sg2002_dt_u32(node, "sophgo,profile-id",
			  SG2002_C906L_PROFILE_ID) ||
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

/*
 * Firmware republishes the complete status record on every heartbeat and
 * transition.  Bracket two manifest reads with two byte-identical status
 * reads so a concurrent publication is retried instead of being diagnosed as
 * an ABI mismatch.  The manifest is commit-last and immutable after boot.
 */
static int sg2002_read_contract_snapshot(struct sg2002_c906l *ctl,
					 struct sg2002_contract_snapshot *snapshot)
{
	struct sg2002_c906l_status before;
	struct sg2002_c906l_manifest first;
	struct sg2002_c906l_manifest second;
	u32 generation;
	int ret;

	memcpy_fromio(&before, ctl->shared + SG2002_C906L_STATUS_REGION_OFFSET,
		      sizeof(before));
	rmb();
	memcpy_fromio(&first, ctl->shared + SG2002_C906L_MANIFEST_OFFSET,
		      sizeof(first));
	rmb();
	memcpy_fromio(&second, ctl->shared + SG2002_C906L_MANIFEST_OFFSET,
		      sizeof(second));
	rmb();
	memcpy_fromio(&snapshot->status,
		      ctl->shared + SG2002_C906L_STATUS_REGION_OFFSET,
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
	snapshot->manifest = first;
	return 0;
}

static int sg2002_validate_activation_source(
		const struct sg2002_contract_snapshot *snapshot)
{
	u32 flags = le32_to_cpu(snapshot->status.flags);
	u16 attempts = le16_to_cpu(snapshot->status.activation_attempts);
	u32 request_id = le32_to_cpu(snapshot->status.activation_request_id);
	u8 error = snapshot->status.activation_error;

	if (!SG2002_C906L_ACTIVATION_REQUIRED ||
	    le64_to_cpu(snapshot->status.capabilities) !=
		SG2002_C906L_DORMANT_CAPABILITIES ||
	    !(SG2002_C906L_MANIFEST_FLAGS &
		SG2002_C906L_MANIFEST_FLAG_ACK_REQUIRED))
		return -EAGAIN;

	if (snapshot->status.activation_state ==
	    SG2002_C906L_ACTIVATION_STATE_DORMANT) {
		if (error != SG2002_C906L_ACTIVATION_RESULT_SUCCESS || attempts ||
		    request_id ||
		    (flags & (SG2002_C906L_FLAG_ACTIVATION_REJECTED |
			      SG2002_C906L_FLAG_ACTIVATION_FAILED)))
			return -EAGAIN;
		return 0;
	}

	/* Firmware permits a corrected request after a validation rejection. */
	if (snapshot->status.activation_state !=
	    SG2002_C906L_ACTIVATION_STATE_REJECTED || !attempts ||
	    error < SG2002_C906L_ACTIVATION_RESULT_INVALID_ENVELOPE ||
	    error > SG2002_C906L_ACTIVATION_RESULT_LEASE_MASK_MISMATCH ||
	    (!request_id && error !=
	     SG2002_C906L_ACTIVATION_RESULT_INVALID_ENVELOPE) ||
	    !(flags & SG2002_C906L_FLAG_ACTIVATION_REJECTED) ||
	    (flags & SG2002_C906L_FLAG_ACTIVATION_FAILED))
		return -EAGAIN;
	return 0;
}

static int sg2002_validate_active(
		const struct sg2002_contract_snapshot *snapshot)
{
	u32 flags = le32_to_cpu(snapshot->status.flags);
	u16 attempts = le16_to_cpu(snapshot->status.activation_attempts);
	u32 request_id = le32_to_cpu(snapshot->status.activation_request_id);

	if (snapshot->status.activation_state !=
		SG2002_C906L_ACTIVATION_STATE_ACTIVE ||
	    snapshot->status.activation_error !=
		SG2002_C906L_ACTIVATION_RESULT_SUCCESS ||
	    (SG2002_C906L_ACTIVATION_REQUIRED ?
	     (!attempts || !request_id) : (attempts || request_id)) ||
	    (SG2002_C906L_ACTIVATION_REQUIRED ?
	     (flags & SG2002_C906L_FLAG_ACTIVATION_FAILED) :
	     (flags & (SG2002_C906L_FLAG_ACTIVATION_REJECTED |
		       SG2002_C906L_FLAG_ACTIVATION_FAILED))) ||
	    le64_to_cpu(snapshot->status.capabilities) !=
		SG2002_C906L_EXPECTED_CAPABILITIES)
		return -EAGAIN;
	return 0;
}

static void sg2002_c906l_receive(struct mbox_client *client, void *message)
{
	struct sg2002_c906l *ctl =
		container_of(client, struct sg2002_c906l, client);
	struct sg2002_c906l_message response;
	unsigned long flags;
	u64 word;

	memcpy(&response, message, sizeof(response));
	memcpy(&word, message, sizeof(word));

	spin_lock_irqsave(&ctl->state_lock, flags);
	if (!ctl->request_pending)
		goto unlock;
	if (response.service != ctl->expected_service ||
	    le16_to_cpu(response.sequence) != ctl->expected_sequence ||
	    (response.opcode != ctl->expected_opcode &&
	     (ctl->activation_waiting ||
	      response.opcode != SG2002_C906L_OP_ERROR)))
		goto unlock;

	ctl->response = word;
	ctl->request_pending = false;
	ctl->response_ready = true;
unlock:
	spin_unlock_irqrestore(&ctl->state_lock, flags);
	wake_up_interruptible(&ctl->response_wait);
}

static void sg2002_c906l_txdone(struct mbox_client *client, void *message,
				int result)
{
	struct sg2002_c906l *ctl =
		container_of(client, struct sg2002_c906l, client);
	unsigned long flags;

	spin_lock_irqsave(&ctl->state_lock, flags);
	if (message == &ctl->tx_word && ctl->tx_pending) {
		ctl->tx_pending = false;
		ctl->tx_status = result;
		if (result < 0)
			ctl->request_pending = false;
	}
	spin_unlock_irqrestore(&ctl->state_lock, flags);
	wake_up_interruptible(&ctl->response_wait);
}

static void sg2002_write_activation_request(struct sg2002_c906l *ctl,
					     u32 generation, u32 request_id)
{
	struct sg2002_c906l_activation_request request = { };
	u8 __iomem *destination =
		ctl->shared + SG2002_C906L_ACTIVATION_REQUEST_OFFSET;

	request.magic = cpu_to_le32(SG2002_C906L_ACTIVATION_REQUEST_MAGIC);
	request.format_major =
		cpu_to_le16(SG2002_C906L_ACTIVATION_REQUEST_FORMAT_MAJOR);
	request.format_minor =
		cpu_to_le16(SG2002_C906L_ACTIVATION_REQUEST_FORMAT_MINOR);
	request.struct_size = cpu_to_le32(SG2002_C906L_ACTIVATION_REQUEST_SIZE);
	request.generation = cpu_to_le32(generation);
	request.request_id = cpu_to_le32(request_id);
	request.contract_epoch = cpu_to_le32(SG2002_C906L_CONTRACT_EPOCH);
	request.profile_id = cpu_to_le32(SG2002_C906L_PROFILE_ID);
	request.abi_version = cpu_to_le32((SG2002_C906L_ABI_MAJOR << 16) |
					 SG2002_C906L_ABI_MINOR);
	request.final_capabilities =
		cpu_to_le64(SG2002_C906L_EXPECTED_CAPABILITIES);
	request.lease_mask = cpu_to_le64(SG2002_C906L_LEASE_MASK);
	memcpy(request.contract_sha256, sg2002_contract_digest,
	       sizeof(request.contract_sha256));
	request.commit = cpu_to_le32(SG2002_C906L_ACTIVATION_REQUEST_COMMIT);

	/* Invalidate the old record, publish its body, then commit it last. */
	writel(0, destination +
	       offsetof(struct sg2002_c906l_activation_request, commit));
	wmb();
	memcpy_toio(destination, &request,
		    offsetof(struct sg2002_c906l_activation_request, commit));
	wmb();
	writel(SG2002_C906L_ACTIVATION_REQUEST_COMMIT,
	       destination +
	       offsetof(struct sg2002_c906l_activation_request, commit));
	wmb();
}

static int sg2002_activation_terminal(struct sg2002_c906l *ctl,
				      u32 request_id, bool got_response,
				      u32 response_result,
				      bool allow_status_recovery)
{
	struct sg2002_contract_snapshot snapshot;
	int ret;

	ret = sg2002_read_contract_snapshot(ctl, &snapshot);
	if (ret)
		return ret;
	if (got_response && response_result !=
	    SG2002_C906L_ACTIVATION_RESULT_SUCCESS)
		return -EREMOTEIO;
	if (snapshot.status.activation_state ==
		SG2002_C906L_ACTIVATION_STATE_ACTIVE &&
	    le32_to_cpu(snapshot.status.activation_request_id) == request_id) {
		ret = sg2002_validate_active(&snapshot);
		if (ret)
			return ret;
		if (!got_response && !allow_status_recovery)
			return -EINPROGRESS;
		ctl->activation_outcome = got_response ?
			SG2002_ACTIVATION_COMPLETED :
			SG2002_ACTIVATION_RECOVERED;
		return 0;
	}
	if (snapshot.status.activation_state ==
		SG2002_C906L_ACTIVATION_STATE_REJECTED ||
	    snapshot.status.activation_state ==
		SG2002_C906L_ACTIVATION_STATE_LEASE_FAULT)
		return -EREMOTEIO;
	return -EINPROGRESS;
}

static int sg2002_activate_leases(struct sg2002_c906l *ctl,
				  const struct sg2002_contract_snapshot *initial)
{
	struct sg2002_c906l_message envelope = { };
	struct sg2002_c906l_message response;
	unsigned long deadline;
	unsigned long flags;
	u32 request_id;
	u32 response_result = SG2002_C906L_ACTIVATION_RESULT_SUCCESS;
	bool got_response = false;
	int ret;

	ret = sg2002_validate_activation_source(initial);
	if (ret)
		return ret;

	request_id = get_random_u32();
	if (!request_id)
		request_id = 1;
	ctl->activation_sequence = request_id & 0xffff;
	sg2002_write_activation_request(ctl,
		le32_to_cpu(initial->status.generation), request_id);

	envelope.service = SG2002_C906L_SERVICE_CONTROL;
	envelope.opcode = SG2002_C906L_OP_ACTIVATE_LEASES;
	envelope.sequence = cpu_to_le16(ctl->activation_sequence);
	envelope.value = cpu_to_le32(request_id);

	spin_lock_irqsave(&ctl->state_lock, flags);
	memcpy(&ctl->tx_word, &envelope, sizeof(ctl->tx_word));
	ctl->tx_status = -EINPROGRESS;
	ctl->tx_pending = true;
	ctl->request_pending = true;
	ctl->response_ready = false;
	ctl->activation_waiting = true;
	ctl->expected_service = SG2002_C906L_SERVICE_CONTROL;
	ctl->expected_opcode = SG2002_C906L_ACTIVATE_RESPONSE;
	ctl->expected_sequence = ctl->activation_sequence;
	spin_unlock_irqrestore(&ctl->state_lock, flags);

	ret = mbox_send_message(ctl->channel, &ctl->tx_word);
	if (ret < 0) {
		spin_lock_irqsave(&ctl->state_lock, flags);
		/* A rejected send transfers no ownership of tx_word. */
		ctl->tx_pending = false;
		ctl->tx_status = ret;
		ctl->request_pending = false;
		ctl->activation_waiting = false;
		spin_unlock_irqrestore(&ctl->state_lock, flags);
		return ret;
	}

	deadline = jiffies +
		msecs_to_jiffies(SG2002_C906L_ACTIVATION_RESPONSE_TIMEOUT_MS);
	for (;;) {
		unsigned long remaining;
		int tx_status = 0;

		spin_lock_irqsave(&ctl->state_lock, flags);
		if (ctl->response_ready) {
			memcpy(&response, &ctl->response, sizeof(response));
			ctl->response_ready = false;
			got_response = true;
			response_result = le32_to_cpu(response.value);
		}
		if (!ctl->tx_pending && ctl->tx_status < 0)
			tx_status = ctl->tx_status;
		spin_unlock_irqrestore(&ctl->state_lock, flags);
		if (tx_status) {
			ret = tx_status;
			break;
		}

		ret = sg2002_activation_terminal(ctl, request_id,
						 got_response, response_result,
						 false);
		if (ret != -EINPROGRESS && ret != -EAGAIN)
			break;
		if (time_after_eq(jiffies, deadline)) {
			ret = sg2002_activation_terminal(ctl, request_id,
							 got_response,
							 response_result, true);
			if (ret == -EINPROGRESS || ret == -EAGAIN)
				ret = -ETIMEDOUT;
			break;
		}
		remaining = deadline - jiffies;
		wait_event_timeout(ctl->response_wait,
			READ_ONCE(ctl->response_ready),
			min_t(unsigned long, remaining,
			      msecs_to_jiffies(20)));
	}

	spin_lock_irqsave(&ctl->state_lock, flags);
	/*
	 * An accepted send is released exclusively by tx_done, even if activation
	 * validation or response matching fails first.
	 */
	ctl->request_pending = false;
	ctl->response_ready = false;
	ctl->activation_waiting = false;
	spin_unlock_irqrestore(&ctl->state_lock, flags);
	return ret;
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
	ctl->activation_waiting = false;
	ctl->tx_status = 0;
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
	struct sg2002_c906l_message message;
	unsigned long flags;
	u64 request;
	int ret;

	(void)offset;
	if (length != sizeof(request))
		return -EMSGSIZE;
	if (copy_from_user(&request, buffer, sizeof(request)))
		return -EFAULT;
	memcpy(&message, &request, sizeof(message));
	if (message.opcode == SG2002_C906L_OP_ACTIVATE_LEASES)
		return -EPERM;

	mutex_lock(&ctl->io_lock);
	spin_lock_irqsave(&ctl->state_lock, flags);
	if (ctl->request_pending || ctl->response_ready || ctl->tx_pending) {
		spin_unlock_irqrestore(&ctl->state_lock, flags);
		mutex_unlock(&ctl->io_lock);
		return -EBUSY;
	}
	ctl->tx_word = request;
	ctl->tx_status = -EINPROGRESS;
	ctl->tx_pending = true;
	ctl->request_pending = true;
	ctl->activation_waiting = false;
	ctl->expected_service = message.service;
	ctl->expected_opcode = message.opcode | SG2002_C906L_OP_RESPONSE;
	ctl->expected_sequence = le16_to_cpu(message.sequence);
	spin_unlock_irqrestore(&ctl->state_lock, flags);

	ret = mbox_send_message(ctl->channel, &ctl->tx_word);
	if (ret < 0) {
		spin_lock_irqsave(&ctl->state_lock, flags);
		ctl->tx_pending = false;
		ctl->tx_status = ret;
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

	(void)offset;
	if (length < sizeof(response))
		return -EMSGSIZE;
	/* An fd may be shared by several threads despite single-open access.
	 * Never enter the wait after a nonblocking readiness check: another
	 * reader can consume that response between the check and the wait.
	 */
	if (!(file->f_flags & O_NONBLOCK)) {
		ret = wait_event_interruptible(ctl->response_wait,
			READ_ONCE(ctl->response_ready) ||
			(!READ_ONCE(ctl->tx_pending) && READ_ONCE(ctl->tx_status) < 0));
		if (ret)
			return ret;
	}

	mutex_lock(&ctl->io_lock);
	spin_lock_irqsave(&ctl->state_lock, flags);
	if (!ctl->response_ready) {
		ret = !ctl->tx_pending && ctl->tx_status < 0 ?
			ctl->tx_status : -EAGAIN;
		spin_unlock_irqrestore(&ctl->state_lock, flags);
		mutex_unlock(&ctl->io_lock);
		return ret;
	}
	response = ctl->response;
	spin_unlock_irqrestore(&ctl->state_lock, flags);

	/* Keep writers/readers serialized, but never hold a spinlock across a
	 * userspace fault. Leave the response available if the copy fails.
	 */
	if (copy_to_user(buffer, &response, sizeof(response))) {
		mutex_unlock(&ctl->io_lock);
		return -EFAULT;
	}
	spin_lock_irqsave(&ctl->state_lock, flags);
	ctl->response_ready = false;
	spin_unlock_irqrestore(&ctl->state_lock, flags);
	mutex_unlock(&ctl->io_lock);
	wake_up_interruptible(&ctl->response_wait);
	return sizeof(response);
}

static __poll_t sg2002_c906l_poll(struct file *file, poll_table *wait)
{
	struct sg2002_c906l *ctl = file->private_data;
	__poll_t mask = 0;

	poll_wait(file, &ctl->response_wait, wait);
	if (READ_ONCE(ctl->response_ready))
		mask |= EPOLLIN | EPOLLRDNORM;
	if (READ_ONCE(ctl->tx_status) < 0 && !READ_ONCE(ctl->tx_pending))
		mask |= EPOLLERR;
	if (!READ_ONCE(ctl->request_pending) &&
	    !READ_ONCE(ctl->response_ready) && !READ_ONCE(ctl->tx_pending))
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
	struct sg2002_contract_snapshot snapshot;
	int ret;

	(void)attr;
	ret = sg2002_read_contract_snapshot(ctl, &snapshot);
	if (ret)
		return sysfs_emit(buffer, "unavailable error=%d\n", ret);

	return sysfs_emit(buffer,
		"abi=%u.%u state=%u generation=%u flags=0x%08x heartbeat=%llu "
		"capabilities=0x%016llx last_request=0x%016llx "
		"last_response=0x%016llx\n",
		le16_to_cpu(snapshot.status.abi_major),
		le16_to_cpu(snapshot.status.abi_minor),
		le32_to_cpu(snapshot.status.state),
		le32_to_cpu(snapshot.status.generation),
		le32_to_cpu(snapshot.status.flags),
		le64_to_cpu(snapshot.status.heartbeat),
		le64_to_cpu(snapshot.status.capabilities),
		le64_to_cpu(snapshot.status.last_request),
		le64_to_cpu(snapshot.status.last_response));
}
static DEVICE_ATTR_RO(status);

static ssize_t contract_show(struct device *dev, struct device_attribute *attr,
			     char *buffer)
{
	(void)dev;
	(void)attr;
	return sysfs_emit(buffer,
		"profile=%s profile_id=%u sha256=%s epoch=%u abi=%u.%u "
		"final_capabilities=0x%016llx dormant_capabilities=0x%016llx "
		"lease_mask=0x%016llx manifest_flags=0x%08x "
		"activation_required=%u\n",
		SG2002_C906L_PROFILE_NAME, SG2002_C906L_PROFILE_ID,
		SG2002_C906L_CONTRACT_SHA256, SG2002_C906L_CONTRACT_EPOCH,
		SG2002_C906L_ABI_MAJOR, SG2002_C906L_ABI_MINOR,
		(unsigned long long)SG2002_C906L_EXPECTED_CAPABILITIES,
		(unsigned long long)SG2002_C906L_DORMANT_CAPABILITIES,
		(unsigned long long)SG2002_C906L_LEASE_MASK,
		SG2002_C906L_MANIFEST_FLAGS,
		SG2002_C906L_ACTIVATION_REQUIRED);
}
static DEVICE_ATTR_RO(contract);

static ssize_t activation_show(struct device *dev,
			       struct device_attribute *attr, char *buffer)
{
	struct sg2002_c906l *ctl = dev_get_drvdata(dev);
	struct sg2002_contract_snapshot snapshot;
	int ret;

	(void)attr;
	ret = sg2002_read_contract_snapshot(ctl, &snapshot);
	if (ret)
		return sysfs_emit(buffer, "unavailable error=%d\n", ret);
	return sysfs_emit(buffer,
		"driver_outcome=%s state=%u error=%u attempts=%u request_id=%u\n",
		sg2002_activation_outcome_name(ctl->activation_outcome),
		snapshot.status.activation_state,
		snapshot.status.activation_error,
		le16_to_cpu(snapshot.status.activation_attempts),
		le32_to_cpu(snapshot.status.activation_request_id));
}
static DEVICE_ATTR_RO(activation);

static struct attribute *sg2002_c906l_attributes[] = {
	&dev_attr_status.attr,
	&dev_attr_contract.attr,
	&dev_attr_activation.attr,
	NULL,
};

static const struct attribute_group sg2002_c906l_attribute_group = {
	.attrs = sg2002_c906l_attributes,
};

static int sg2002_c906l_probe(struct platform_device *pdev)
{
	struct sg2002_contract_snapshot snapshot;
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

	ret = sg2002_validate_dt_identity(&pdev->dev);
	if (ret)
		return ret;
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
	ctl->shared = devm_ioremap_wc(&pdev->dev, memory.start,
				     resource_size(&memory));
	if (!ctl->shared)
		return dev_err_probe(&pdev->dev, -ENOMEM,
				     "failed to map write-combined shared memory\n");

	ret = sg2002_read_contract_snapshot(ctl, &snapshot);
	if (ret)
		return dev_err_probe(&pdev->dev,
			ret == -EAGAIN ? -EPROBE_DEFER : ret,
			"C906L contract manifest is not ready or does not match\n");

	ctl->client.dev = &pdev->dev;
	ctl->client.rx_callback = sg2002_c906l_receive;
	ctl->client.tx_done = sg2002_c906l_txdone;
	ctl->client.tx_block = false;
	ctl->client.knows_txdone = false;
	ctl->channel = mbox_request_channel_byname(&ctl->client, "control");
	if (IS_ERR(ctl->channel))
		return dev_err_probe(&pdev->dev, PTR_ERR(ctl->channel),
				     "failed to request control mailbox\n");

	if (SG2002_C906L_ACTIVATION_REQUIRED) {
		if (!sg2002_validate_active(&snapshot)) {
			ctl->activation_outcome = SG2002_ACTIVATION_ALREADY_ACTIVE;
		} else {
			ret = sg2002_activate_leases(ctl, &snapshot);
			if (ret) {
				if (!sg2002_read_contract_snapshot(ctl, &snapshot))
					dev_err(&pdev->dev,
						"activation state=%u error=%u attempts=%u request_id=%u\n",
						snapshot.status.activation_state,
						snapshot.status.activation_error,
						le16_to_cpu(snapshot.status.activation_attempts),
						le32_to_cpu(snapshot.status.activation_request_id));
				goto free_channel;
			}
		}
	} else {
		ret = sg2002_validate_active(&snapshot);
		if (ret)
			goto free_channel;
		ctl->activation_outcome = SG2002_ACTIVATION_NOT_REQUIRED;
	}

	ctl->misc.minor = MISC_DYNAMIC_MINOR;
	ctl->misc.name = "sg2002-c906l-control";
	ctl->misc.fops = &sg2002_c906l_fops;
	ctl->misc.parent = &pdev->dev;
	platform_set_drvdata(pdev, ctl);

	ret = misc_register(&ctl->misc);
	if (ret)
		goto free_channel;
	ret = sysfs_create_group(&pdev->dev.kobj,
				 &sg2002_c906l_attribute_group);
	if (ret)
		goto deregister_misc;

	dev_info(&pdev->dev,
		 "validated C906L %s contract (%s), activation %s\n",
		 SG2002_C906L_PROFILE_NAME, SG2002_C906L_CONTRACT_SHA256,
		 sg2002_activation_outcome_name(ctl->activation_outcome));
	return 0;

deregister_misc:
	misc_deregister(&ctl->misc);
free_channel:
	mbox_free_channel(ctl->channel);
	return dev_err_probe(&pdev->dev, ret,
			     "failed to initialize C906L control endpoint\n");
}

static void sg2002_c906l_remove(struct platform_device *pdev)
{
	struct sg2002_c906l *ctl = platform_get_drvdata(pdev);

	sysfs_remove_group(&pdev->dev.kobj, &sg2002_c906l_attribute_group);
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

MODULE_DESCRIPTION("Exact-contract SG2002 C906L mailbox control endpoint");
MODULE_AUTHOR("nixos-nanokvm contributors");
MODULE_LICENSE("GPL");
