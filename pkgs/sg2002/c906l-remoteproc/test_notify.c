/* test_source.py inserts the production mailbox callback below. */
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef uint64_t u64;
typedef uint32_t u32;
struct mbox_client { int unused; };
struct rproc {
	int max_notifyid;
	bool used[2];
	unsigned int wakes[2];
};
struct sg2002_c906l_rproc {
	struct mbox_client mbox_client;
	struct rproc *rproc;
	void *dev;
	bool shutting_down;
	unsigned int notifications;
};
#define READ_ONCE(value) (value)
#define container_of(pointer, type, member) ((type *)((char *)(pointer) - offsetof(type, member)))
#define atomic64_inc(value) (++*(value))
#define dev_warn_ratelimited(device, format, ...) ((void)(device), (void)(format))
static unsigned int calls, order[8];

static int rproc_vq_interrupt(struct rproc *rproc, int id)
{
	assert(id >= 0 && id <= rproc->max_notifyid && id < 2);
	assert(calls < 8);
	order[calls++] = (unsigned int)id;
	if (!rproc->used[id])
		return 0; /* Linux vring_interrupt returns IRQ_NONE for stale kicks. */
	rproc->used[id] = false;
	++rproc->wakes[id];
	return 1;
}

/* @NOTIFY_FUNCTION@ */

int main(void)
{
	struct rproc rproc = { .max_notifyid = 1, .used = { true, true } };
	struct sg2002_c906l_rproc priv = { .rproc = &rproc };
	u64 word = 1;

	/* TX completion arrives first, while RX echo is already in shared RAM.
	 * It must wake the RX consumer without requiring another notification.
	 */
	sg2002_mbox_receive(&priv.mbox_client, &word);
	assert(calls == 2 && order[0] == 0 && order[1] == 1);
	assert(rproc.wakes[0] == 1 && rproc.wakes[1] == 1);
	word = 0;
	sg2002_mbox_receive(&priv.mbox_client, &word);
	assert(calls == 4 && rproc.wakes[0] == 1 && rproc.wakes[1] == 1);
	assert(priv.notifications == 2);

	/* Invalid IDs, teardown, and incomplete attachment must remain inert. */
	word = 2;
	sg2002_mbox_receive(&priv.mbox_client, &word);
	word = UINT32_MAX;
	sg2002_mbox_receive(&priv.mbox_client, &word);
	assert(calls == 4);
	word = 0;
	priv.shutting_down = true;
	sg2002_mbox_receive(&priv.mbox_client, &word);
	assert(calls == 4 && priv.notifications == 4);
	priv.shutting_down = false;
	priv.rproc = NULL;
	sg2002_mbox_receive(&priv.mbox_client, &word);
	priv.rproc = &rproc;
	rproc.max_notifyid = -1;
	sg2002_mbox_receive(&priv.mbox_client, &word);
	assert(calls == 4);
	rproc.max_notifyid = 0;
	word = 1;
	sg2002_mbox_receive(&priv.mbox_client, &word);
	assert(calls == 4);
	word = 0;
	rproc.used[0] = true;
	sg2002_mbox_receive(&priv.mbox_client, &word);
	assert(calls == 5 && order[4] == 0 && rproc.wakes[0] == 2);
	return 0;
}
