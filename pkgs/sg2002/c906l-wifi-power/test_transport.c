/* Fake kernel I/O surrounds the exact production request/ack state machine. */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "sg2002-c906l-contract.h"

#define __iomem
#define LCD(name) SG2002_C906L_PICOCLAW_LCD_##name
#define le32_to_cpu(v) ((uint32_t)(v))
#define le16_to_cpu(v) ((uint16_t)(v))
#define le64_to_cpu(v) ((uint64_t)(v))
#define cpu_to_le32(v) ((uint32_t)(v))
#define U32_MAX UINT32_MAX
#define time_after_eq(a, b) ((long)((a) - (b)) >= 0)
#define time_before(a, b) ((long)((a) - (b)) < 0)
#define msecs_to_jiffies(v) (v)
#define dev_err(...) ((void)0)
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint32_t __le32;
struct mutex { bool locked; };
struct c906l_power {
	u8 *shared;
	struct mutex lock;
	u32 generation, sequence;
	bool enabled;
	int fault;
};
struct regulator_dev { struct c906l_power *power; };
static struct c906l_power *rdev_get_drvdata(struct regulator_dev *rdev) { return rdev->power; }
static u8 memory[SG2002_C906L_SHMEM_SIZE];
static unsigned long jiffies, change_time[8];
static bool change_value[8];
static unsigned int writes, changes, mode;
static u32 firmware_sequence;
static void rmb(void) { }
static void wmb(void) { }
static void memcpy_fromio(void *to, const void *from, size_t size) { memcpy(to, from, size); }
static void memcpy_toio(void *to, const void *from, size_t size)
{
	assert(size == 64);
	assert(((const u32 *)from)[15] == 0); /* commit must be published later */
	memcpy(to, from, size);
}
static void writel(u32 value, void *address) { *(u32 *)address = value; writes++; }
static void *memchr_inv(const void *address, int value, size_t size)
{
	const u8 *bytes = address;
	for (size_t i = 0; i < size; i++)
		if (bytes[i] != value) return (void *)(bytes + i);
	return NULL;
}
static void mutex_lock(struct mutex *lock) { assert(!lock->locked); lock->locked = true; }
static void mutex_unlock(struct mutex *lock) { assert(lock->locked); lock->locked = false; }
static void msleep(unsigned int delay);

/* @PRODUCTION@ */

static void msleep(unsigned int delay)
{
	struct power_record *request = (void *)(memory + LCD(WIFI_POWER_OWNERSHIP_ADDRESS) - SG2002_C906L_SHMEM_ADDRESS);
	struct power_record *response = request + 1;
	jiffies += delay;
	if (request->sequence && request->sequence != firmware_sequence && request->commit == request->sequence) {
		assert(changes < 8);
		change_time[changes] = jiffies;
		change_value[changes++] = request->enabled;
		firmware_sequence = request->sequence;
		if (mode == 1) return; /* GPIO changed, acknowledgement lost */
		*response = *request;
		response->magic = LCD(WIFI_POWER_COMPLETION_MAGIC);
		if (mode == 2) response->generation++;
		if (mode == 3) response->reserved[0] = 1;
		if (mode == 4) response->enabled ^= 1;
	}
}

static struct c906l_power fresh(void)
{
	struct sg2002_c906l_status *status = (void *)(memory + SG2002_C906L_STATUS_REGION_OFFSET);
	memset(memory, 0, sizeof(memory));
	jiffies = writes = changes = mode = firmware_sequence = 0;
	status->magic = SG2002_C906L_SHMEM_MAGIC;
	status->abi_major = SG2002_C906L_ABI_MAJOR;
	status->abi_minor = SG2002_C906L_ABI_MINOR;
	status->struct_size = SG2002_C906L_STATUS_SIZE;
	status->state = SG2002_C906L_STATE_RUNNING;
	status->activation_state = SG2002_C906L_ACTIVATION_STATE_ACTIVE;
	status->capabilities = SG2002_C906L_EXPECTED_CAPABILITIES;
	status->generation = 7;
	return (struct c906l_power) { .shared = memory, .generation = 7 };
}

int main(void)
{
	struct c906l_power power = fresh();
	struct regulator_dev rdev = { .power = &power };
	assert(power_is_enabled(&rdev) == 0);
	assert(power_enable(&rdev) == 0);
	assert(power_is_enabled(&rdev) == 1);
	assert(changes == 2 && !change_value[0] && change_value[1]);
	assert(change_time[1] - change_time[0] >= LCD(WIFI_POWER_OFF_MILLISECONDS));
	assert(jiffies - change_time[1] >= LCD(WIFI_POWER_ON_MILLISECONDS));
	assert(power_disable(&rdev) == 0);
	assert(changes == 3 && !change_value[2]);
	assert(power_is_enabled(&rdev) == 0);

	for (unsigned int failure = 1; failure <= 4; failure++) {
		power = fresh();
		mode = failure;
		assert(power_enable(&rdev) < 0);
		assert(power.fault < 0 && changes == 1);
		if (failure <= 2) assert(power.fault == -ETIMEDOUT);
		if (failure == 3) assert(power.fault == -EPROTO);
		if (failure == 4) assert(power.fault == -EIO);
		unsigned int retained = writes;
		assert(power_enable(&rdev) == power.fault);
		assert(power_disable(&rdev) == power.fault);
		assert(writes == retained); /* uncertain ownership never overwritten */
	}

	power = fresh();
	power.generation = 6;
	assert(power_enable(&rdev) == -ESTALE && writes == 0);
	power = fresh();
	power.sequence = U32_MAX;
	assert(power_enable(&rdev) == -EOVERFLOW && writes == 0);

	struct power_record a = { .magic = LCD(WIFI_POWER_COMPLETION_MAGIC),
		.generation = 7, .sequence = 1, .commit = 1, .enabled = 1 };
	struct power_record b = a;
	assert(acknowledged(&a, &b, 7, 1, true) == 1);
	b.commit = 0;
	assert(acknowledged(&a, &b, 7, 1, true) == 0);
	assert(acknowledged(&a, &a, 7, 2, true) == 0);
	assert(acknowledged(&a, &a, 8, 1, true) == 0);
	puts("Wi-Fi power: timings, stale/torn ACKs, errors, timeout ownership and wrap passed");
	return 0;
}
