/* glibc loads its unwind library at runtime, outside the ELF DT_NEEDED
 * closure. Exercise cancellation and cleanup inside the pruned initrd. */
#define _POSIX_C_SOURCE 200809L
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static pthread_barrier_t ready;
static int cleaned;

static void cleanup(void *unused)
{
	(void)unused;
	cleaned = 1;
}

static void *worker(void *unused)
{
	(void)unused;
	pthread_cleanup_push(cleanup, NULL);
	pthread_barrier_wait(&ready);
	for (;;)
		pause();
	pthread_cleanup_pop(0);
	return NULL;
}

int main(void)
{
	pthread_t thread;
	void *result;

	if (pthread_barrier_init(&ready, NULL, 2) ||
	    pthread_create(&thread, NULL, worker, NULL))
		return EXIT_FAILURE;
	pthread_barrier_wait(&ready);
	if (pthread_cancel(thread) || pthread_join(thread, &result) ||
	    result != PTHREAD_CANCELED || !cleaned ||
	    pthread_barrier_destroy(&ready))
		return EXIT_FAILURE;
	puts("pthread-cancel-ok");
	return EXIT_SUCCESS;
}
