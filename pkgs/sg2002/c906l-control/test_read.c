/* The production read function is inserted below by test_source.py. */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>

#define __user
#define loff_t int64_t
#define READ_ONCE(value) (value)
typedef uint64_t u64;

struct sg2002_c906l {
	int state_lock, io_lock, response_wait;
	bool response_ready, tx_pending;
	int tx_status;
	u64 response;
};
struct file { int f_flags; void *private_data; };

static int waits, wakes, wait_error;
static bool spin_locked, mutex_locked, copy_fault, steal_response;
static struct sg2002_c906l *current;

static void lock_spin(void)
{
	assert(!spin_locked);
	spin_locked = true;
}
static void unlock_spin(void)
{
	assert(spin_locked);
	spin_locked = false;
	/* A competing reader can consume a response after a bare precheck,
	 * but cannot do so while the production read holds io_lock.
	 */
	if (steal_response && !mutex_locked)
		current->response_ready = false;
}
#define spin_lock_irqsave(lock, flags) do { (void)(lock); (flags) = 0; lock_spin(); } while (0)
#define spin_unlock_irqrestore(lock, flags) do { (void)(lock); (void)(flags); unlock_spin(); } while (0)
static void mutex_lock(int *lock)
{
	(void)lock;
	assert(!mutex_locked);
	mutex_locked = true;
}
static void mutex_unlock(int *lock)
{
	(void)lock;
	assert(mutex_locked);
	mutex_locked = false;
}
static int wait_for_response(bool ready)
{
	++waits;
	return wait_error ? wait_error : (ready ? 0 : -EINTR);
}
#define wait_event_interruptible(queue, ready) ((void)(queue), wait_for_response(ready))
static inline void wake_up_interruptible(int *queue) { (void)queue; ++wakes; }
static int copy_to_user(void *destination, const void *source, size_t length)
{
	assert(!spin_locked);
	assert(mutex_locked);
	if (copy_fault)
		return 1;
	memcpy(destination, source, length);
	return 0;
}

/* @READ_FUNCTION@ */

int main(void)
{
	struct sg2002_c906l ctl = { .response_ready = true, .response = 0x12345678 };
	struct file file = { .f_flags = O_NONBLOCK, .private_data = &ctl };
	u64 output = 0;
	current = &ctl;
	steal_response = true;
	assert(sg2002_c906l_read(&file, (char *)&output, 8, NULL) == 8);
	assert(output == ctl.response && !ctl.response_ready && !waits && wakes == 1);
	steal_response = false;
	assert(sg2002_c906l_read(&file, (char *)&output, 8, NULL) == -EAGAIN);
	assert(!waits);
	ctl.tx_status = -EIO;
	assert(sg2002_c906l_read(&file, (char *)&output, 8, NULL) == -EIO);
	ctl.tx_pending = true;
	assert(sg2002_c906l_read(&file, (char *)&output, 8, NULL) == -EAGAIN);
	ctl.tx_pending = false;
	ctl.tx_status = 0;
	ctl.response_ready = true;
	copy_fault = true;
	assert(sg2002_c906l_read(&file, (char *)&output, 8, NULL) == -EFAULT);
	assert(ctl.response_ready && !mutex_locked && !spin_locked);
	copy_fault = false;
	assert(sg2002_c906l_read(&file, (char *)&output, 8, NULL) == 8);
	assert(!ctl.response_ready && !waits);
	assert(sg2002_c906l_read(&file, (char *)&output, 7, NULL) == -EMSGSIZE);
	file.f_flags = 0;
	wait_error = -EINTR;
	assert(sg2002_c906l_read(&file, (char *)&output, 8, NULL) == -EINTR);
	wait_error = 0;
	ctl.response_ready = true;
	assert(sg2002_c906l_read(&file, (char *)&output, 8, NULL) == 8);
	assert(waits == 2 && !mutex_locked && !spin_locked);
	return 0;
}
