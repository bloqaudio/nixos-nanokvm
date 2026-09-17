// SPDX-License-Identifier: GPL-2.0-only
/*
 * SDIO vmmc provider for the C906L-owned PicoClaw GPIOA26 power enable.
 * Only fixed reserved DDR is mapped here. The Rust LCD task is the sole
 * GPIOA owner; every power change is acknowledged after latch readback.
 */
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/of_regulator.h>

#include "sg2002-c906l-kernel-contract.h"

#define LCD(name) SG2002_C906L_PICOCLAW_LCD_##name

struct power_record {
	__le32 magic, generation, sequence, enabled;
	u8 reserved[44];
	__le32 commit;
};
static_assert(sizeof(struct power_record) == 64);
static_assert(offsetof(struct power_record, commit) == 60);

struct c906l_power {
	struct device *dev;
	void __iomem *shared;
	struct mutex lock;
	u32 generation;
	u32 sequence;
	bool enabled;
	int fault;
};

static const u8 contract_digest[32] = SG2002_C906L_CONTRACT_SHA256_BYTES;

static bool manifest_valid(const struct sg2002_c906l_manifest *m)
{
	return le32_to_cpu(m->magic) == SG2002_C906L_MANIFEST_MAGIC &&
		le16_to_cpu(m->format_major) == SG2002_C906L_MANIFEST_FORMAT_MAJOR &&
		le16_to_cpu(m->format_minor) == SG2002_C906L_MANIFEST_FORMAT_MINOR &&
		le32_to_cpu(m->struct_size) == SG2002_C906L_MANIFEST_SIZE &&
		le32_to_cpu(m->contract_epoch) == SG2002_C906L_CONTRACT_EPOCH &&
		le32_to_cpu(m->profile_id) == SG2002_C906L_PROFILE_ID &&
		le16_to_cpu(m->abi_major) == SG2002_C906L_ABI_MAJOR &&
		le16_to_cpu(m->abi_minor) == SG2002_C906L_ABI_MINOR &&
		le16_to_cpu(m->capability_width) == SG2002_C906L_CAPABILITY_WIRE_WIDTH &&
		le16_to_cpu(m->lease_width) == SG2002_C906L_LEASE_WIRE_WIDTH &&
		le64_to_cpu(m->final_capabilities) == SG2002_C906L_EXPECTED_CAPABILITIES &&
		le64_to_cpu(m->dormant_capabilities) == SG2002_C906L_DORMANT_CAPABILITIES &&
		le64_to_cpu(m->lease_mask) == SG2002_C906L_LEASE_MASK &&
		le32_to_cpu(m->flags) == SG2002_C906L_MANIFEST_FLAGS &&
		!le32_to_cpu(m->reserved0) &&
		!memchr_inv(m->reserved1, 0, sizeof(m->reserved1)) &&
		!memcmp(m->contract_sha256, contract_digest, sizeof(contract_digest)) &&
		le32_to_cpu(m->commit) == SG2002_C906L_MANIFEST_COMMIT;
}

static int generation_valid(struct c906l_power *power)
{
	struct sg2002_c906l_status first, second;

	memcpy_fromio(&first, power->shared + SG2002_C906L_STATUS_REGION_OFFSET, sizeof(first));
	rmb();
	memcpy_fromio(&second, power->shared + SG2002_C906L_STATUS_REGION_OFFSET, sizeof(second));
	rmb();
	if (memcmp(&first, &second, sizeof(first)))
		return -EAGAIN;
	if (le32_to_cpu(first.generation) != power->generation)
		return -ESTALE;
	if (le32_to_cpu(first.magic) != SG2002_C906L_SHMEM_MAGIC ||
	    le16_to_cpu(first.abi_major) != SG2002_C906L_ABI_MAJOR ||
	    le16_to_cpu(first.abi_minor) != SG2002_C906L_ABI_MINOR ||
	    le32_to_cpu(first.struct_size) != SG2002_C906L_STATUS_SIZE ||
	    le32_to_cpu(first.state) != SG2002_C906L_STATE_RUNNING ||
	    first.activation_state != SG2002_C906L_ACTIVATION_STATE_ACTIVE ||
	    le64_to_cpu(first.capabilities) != SG2002_C906L_EXPECTED_CAPABILITIES ||
	    le32_to_cpu(first.flags))
		return -EIO;
	return 0;
}

static void __iomem *request_address(struct c906l_power *power)
{
	return power->shared + LCD(WIFI_POWER_OWNERSHIP_ADDRESS) - SG2002_C906L_SHMEM_ADDRESS;
}

/* Positive means complete, zero means an old/in-flight acknowledgement. */
static int acknowledged(const struct power_record *a, const struct power_record *b,
			u32 generation, u32 sequence, bool enabled)
{
	if (memcmp(a, b, sizeof(*a)) ||
	    le32_to_cpu(a->magic) != LCD(WIFI_POWER_COMPLETION_MAGIC) ||
	    le32_to_cpu(a->generation) != generation ||
	    le32_to_cpu(a->sequence) != sequence ||
	    le32_to_cpu(a->commit) != sequence)
		return 0;
	if (memchr_inv(a->reserved, 0, sizeof(a->reserved)))
		return -EPROTO;
	if (le32_to_cpu(a->enabled) != enabled)
		return -EIO;
	return 1;
}

/* Caller holds lock across a complete power cycle. After a timeout or any
 * terminal error ownership is retained; no later request can overwrite a
 * command whose eventual execution is unknown. Recovery is a board reset. */
static int set_power_locked(struct c906l_power *power, bool enabled)
{
	struct power_record request = { 0 }, first, second;
	void __iomem *address = request_address(power);
	unsigned long deadline = jiffies + msecs_to_jiffies(LCD(WIFI_POWER_TIMEOUT_MILLISECONDS));
	int ret;

	if (power->fault)
		return power->fault;
	if (power->sequence == U32_MAX) {
		ret = -EOVERFLOW;
		goto fault;
	}
	/* A changing status snapshot is normal; wait before publishing. */
	do {
		ret = generation_valid(power);
		if (ret != -EAGAIN)
			break;
		msleep(5);
	} while (time_before(jiffies, deadline));
	if (ret)
		goto fault;
	request.magic = cpu_to_le32(LCD(WIFI_POWER_REQUEST_MAGIC));
	request.generation = cpu_to_le32(power->generation);
	request.sequence = cpu_to_le32(++power->sequence);
	request.enabled = cpu_to_le32(enabled);
	memcpy_toio(address, &request, sizeof(request));
	wmb();
	writel(power->sequence, address + offsetof(struct power_record, commit));
	wmb();
	for (;;) {
		ret = generation_valid(power);
		if (ret && ret != -EAGAIN)
			goto fault;
		if (!ret) {
			memcpy_fromio(&first, address + 64, sizeof(first));
			rmb();
			memcpy_fromio(&second, address + 64, sizeof(second));
			rmb();
			ret = acknowledged(&first, &second, power->generation, power->sequence, enabled);
			if (ret < 0)
				goto fault;
			if (ret) {
				power->enabled = enabled;
				return 0;
			}
		}
		if (time_after_eq(jiffies, deadline)) {
			ret = -ETIMEDOUT;
			goto fault;
		}
		msleep(5);
	}
fault:
	power->fault = ret;
	dev_err(power->dev, "C906L Wi-Fi power failed: %d; command retained, reboot required\n", ret);
	return ret;
}

static int power_enable(struct regulator_dev *rdev)
{
	struct c906l_power *power = rdev_get_drvdata(rdev);
	int ret;

	mutex_lock(&power->lock);
	/* Cold boot and later MMC power cycles both have a real 60 ms off
	 * interval followed by at least 10 ms settling, as on the proven SDIO
	 * bring-up path. Delays never block C906L's LCD/IPC service task. */
	ret = set_power_locked(power, false);
	if (!ret) {
		msleep(LCD(WIFI_POWER_OFF_MILLISECONDS));
		ret = set_power_locked(power, true);
		if (!ret)
			msleep(LCD(WIFI_POWER_ON_MILLISECONDS));
	}
	mutex_unlock(&power->lock);
	return ret;
}

static int power_disable(struct regulator_dev *rdev)
{
	struct c906l_power *power = rdev_get_drvdata(rdev);
	int ret;

	mutex_lock(&power->lock);
	ret = set_power_locked(power, false);
	mutex_unlock(&power->lock);
	return ret;
}

static int power_is_enabled(struct regulator_dev *rdev)
{
	struct c906l_power *power = rdev_get_drvdata(rdev);
	int ret;

	mutex_lock(&power->lock);
	ret = power->fault ? power->fault : power->enabled;
	mutex_unlock(&power->lock);
	return ret;
}

static const struct regulator_ops power_ops = {
	.enable = power_enable,
	.disable = power_disable,
	.is_enabled = power_is_enabled,
	.list_voltage = regulator_list_voltage_linear,
};

static const struct regulator_desc power_desc = {
	.name = "picoclaw-c906l-wifi-power",
	.type = REGULATOR_VOLTAGE,
	.owner = THIS_MODULE,
	.ops = &power_ops,
	.n_voltages = 1,
	.fixed_uV = 3300000,
	.min_uV = 3300000,
};

static ssize_t transport_status_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	struct c906l_power *power = dev_get_drvdata(dev);

	return sysfs_emit(buf, "generation=%u sequence=%u enabled=%u fault=%d\n",
		power->generation, READ_ONCE(power->sequence),
		READ_ONCE(power->enabled), READ_ONCE(power->fault));
}
static DEVICE_ATTR_RO(transport_status);

static int power_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct regulator_config config = { .dev = dev, .of_node = dev->of_node };
	struct sg2002_c906l_manifest manifest, again;
	struct c906l_power *power;
	struct regulator_dev *rdev;
	struct power_record old;
	struct device_node *node;
	struct resource memory;
	u8 digest[32];
	int ret;

	if (!of_machine_is_compatible("sipeed,licheerv-nano-picoclaw"))
		return -ENODEV;
	if (of_property_read_u8_array(dev->of_node, "sophgo,contract-sha256", digest, sizeof(digest)) ||
	    memcmp(digest, contract_digest, sizeof(digest)))
		return -EPROTO;
	node = of_parse_phandle(dev->of_node, "memory-region", 0);
	if (!node)
		return -EINVAL;
	ret = of_address_to_resource(node, 0, &memory);
	of_node_put(node);
	if (ret)
		return ret;
	if (memory.start != SG2002_C906L_SHMEM_ADDRESS || resource_size(&memory) != SG2002_C906L_SHMEM_SIZE)
		return -EINVAL;
	power = devm_kzalloc(dev, sizeof(*power), GFP_KERNEL);
	if (!power)
		return -ENOMEM;
	power->dev = dev;
	power->shared = devm_ioremap_wc(dev, memory.start, resource_size(&memory));
	if (!power->shared)
		return -ENOMEM;
	memcpy_fromio(&manifest, power->shared + SG2002_C906L_MANIFEST_OFFSET, sizeof(manifest));
	rmb();
	memcpy_fromio(&again, power->shared + SG2002_C906L_MANIFEST_OFFSET, sizeof(again));
	rmb();
	if (memcmp(&manifest, &again, sizeof(manifest)) || !manifest_valid(&manifest) || !le32_to_cpu(manifest.generation))
		return -EPROBE_DEFER;
	power->generation = le32_to_cpu(manifest.generation);
	ret = generation_valid(power);
	if (ret)
		return -EPROBE_DEFER;
	memcpy_fromio(&old, request_address(power), sizeof(old));
	rmb();
	if (le32_to_cpu(old.magic) == LCD(WIFI_POWER_REQUEST_MAGIC) &&
	    le32_to_cpu(old.generation) == power->generation && le32_to_cpu(old.sequence))
		return dev_err_probe(dev, -EBUSY, "same-generation regulator rebind requires board reset\n");
	mutex_init(&power->lock);
	config.driver_data = power;
	config.init_data = of_get_regulator_init_data(dev, dev->of_node, &power_desc);
	if (!config.init_data)
		return -EINVAL;
	/* Confirm that firmware has constructed the sole GPIO owner and has
	 * actually driven power low before exposing the provider to MMC. */
	mutex_lock(&power->lock);
	ret = set_power_locked(power, false);
	mutex_unlock(&power->lock);
	if (ret)
		return ret;
	rdev = devm_regulator_register(dev, &power_desc, &config);
	if (IS_ERR(rdev))
		return PTR_ERR(rdev);
	platform_set_drvdata(pdev, power);
	ret = device_create_file(dev, &dev_attr_transport_status);
	if (ret)
		return ret;
	/* Firmware is FSBL-started and attach-only. Runtime unbind/unload cannot
	 * revoke a possibly in-flight command; retain this transport until reset. */
	__module_get(THIS_MODULE);
	dev_info(dev, "C906L Wi-Fi power regulator ready, generation %u\n", power->generation);
	return 0;
}

static const struct of_device_id power_match[] = {
	{ .compatible = "sophgo,sg2002-c906l-wifi-power" },
	{ }
};
MODULE_DEVICE_TABLE(of, power_match);

static struct platform_driver power_driver = {
	.probe = power_probe,
	.driver = {
		.name = "sg2002-c906l-wifi-power",
		.of_match_table = power_match,
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(power_driver);
MODULE_DESCRIPTION("SG2002 C906L acknowledged PicoClaw Wi-Fi power regulator");
MODULE_LICENSE("GPL");
