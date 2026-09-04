/* SPDX-License-Identifier: MIT */
#include <stdint.h>

#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"

#include "arch_helpers.h"
#include "cvi_mailbox.h"
#include "intr_conf.h"
#include "sg2002-c906l-contract.h"
#include "top_reg.h"

#define CONTROL_BIT (1U << SG2002_C906L_CHANNEL_CONTROL)
#define VQ_KICK_BIT (1U << SG2002_C906L_CHANNEL_VQ_KICK)
#define VQ_NOTIFY_BIT (1U << SG2002_C906L_CHANNEL_VQ_NOTIFY)
#define RESPONSE_RETRIES 20U
#define MAILBOX_BUSY (-1)
#define MAILBOX_LOCK_ERROR (-2)

_Static_assert(MAILBOX_REG_BASE == SG2002_C906L_MAILBOX_ADDRESS,
	       "vendor and generated mailbox register addresses differ");
_Static_assert(MAILBOX_REG_BUFF == SG2002_C906L_MAILBOX_PAYLOAD_ADDRESS,
	       "vendor and generated mailbox payload addresses differ");
_Static_assert(SPINLOCK_REG_BASE ==
	       SG2002_C906L_MAILBOX_HWSPIN_BASE_ADDRESS,
	       "vendor and generated hardware-spinlock addresses differ");
_Static_assert(SG2002_C906L_MAILBOX_HWSPIN_FIELD == 4U,
	       "SPIN_MBOX must remain hardware-spinlock field 4");
_Static_assert(SG2002_C906L_MAILBOX_HWSPIN_ADDRESS ==
	       SG2002_C906L_MAILBOX_HWSPIN_BASE_ADDRESS +
	       SG2002_C906L_MAILBOX_HWSPIN_FIELD *
	       SG2002_C906L_MAILBOX_HWSPIN_REGISTER_STRIDE,
	       "SPIN_MBOX register address is inconsistent");
_Static_assert(SG2002_C906L_MAILBOX_HWSPIN_ACCESS_WIDTH ==
	       sizeof(uint16_t), "SPIN_MBOX requires halfword MMIO");
_Static_assert(SG2002_C906L_MAILBOX_HWSPIN_C906L_TOKEN_MASK ==
	       UINT32_C(0x0000ff00),
	       "C906L must use the vendor high-byte token namespace");
_Static_assert((SG2002_C906L_MAILBOX_HWSPIN_C906L_TOKEN_MASK &
		SG2002_C906L_MAILBOX_HWSPIN_LINUX_TOKEN_MASK) == 0U,
	       "Linux and C906L hardware-spinlock tokens overlap");
_Static_assert(SG2002_C906L_MAILBOX_CHANNEL_MASK ==
	       (CONTROL_BIT | VQ_KICK_BIT | VQ_NOTIFY_BIT),
	       "generated mailbox channel mask is inconsistent");
_Static_assert(SG2002_C906L_MAILBOX_PROCESSOR_COUNT ==
	       sizeof(((struct mailbox_set_register *)0)->cpu_mbox_en) /
	       sizeof(((struct mailbox_set_register *)0)->cpu_mbox_en[0]),
	       "generated mailbox processor count differs from vendor registers");
_Static_assert(MBOX_INT_C906_2ND == SG2002_C906L_MAILBOX_C906L_IRQ,
	       "vendor and generated C906L mailbox IRQs differ");

/* The pinned BSP implements this API but its installed header omits it. */
extern void disable_irq(unsigned int irqn);

#ifdef SG2002_C906L_TIMER4
/* SoC Timer4 is the timer IP's one-based Timer5 register group. */
#if TIMER_INTR_4 != SG2002_C906L_TIMER4_IRQ
#error "cv181x C906L Timer4 IRQ routing changed"
#endif
#endif

typedef void (*rust_task_t)(void *);

static volatile struct mailbox_set_register *const mbox =
	(volatile struct mailbox_set_register *)(uintptr_t)
		SG2002_C906L_MAILBOX_ADDRESS;
static volatile uint64_t *const slots =
	(volatile uint64_t *)(uintptr_t)
		SG2002_C906L_MAILBOX_PAYLOAD_ADDRESS;
static volatile uint16_t *const mailbox_hwspin =
	(volatile uint16_t *)(uintptr_t)
		SG2002_C906L_MAILBOX_HWSPIN_ADDRESS;
static QueueHandle_t requests;
static QueueHandle_t vq_kicks;
static volatile uint32_t request_drops;
static volatile uint32_t vq_kick_drops;
static volatile uint32_t mailbox_lock_failures;
static volatile uint32_t mailbox_irq_lock_deferrals;
static uint32_t mailbox_irq_lock_streak;
static volatile uint32_t unexpected_mailbox_events;
static uint8_t mailbox_lock_counter;

extern void c906l_rust_main(void) __attribute__((noreturn));
#ifdef SG2002_C906L_TIMER4
extern int c906l_timer4_interrupt(void);
#endif

static inline void io_fence(void)
{
	__asm__ volatile("fence iorw, iorw" ::: "memory");
}

struct mailbox_lock_guard {
	uint16_t token;
	uint8_t restore_irqs;
};

static inline uint8_t mailbox_local_irq_save(void)
{
	uintptr_t previous;
	uintptr_t mie = 8U;

	__asm__ volatile("csrrc %0, mstatus, %1"
			 : "=r"(previous) : "r"(mie) : "memory");
	return (previous & mie) != 0U;
}

static inline void mailbox_local_irq_restore(uint8_t restore_irqs)
{
	uintptr_t mie = 8U;

	if (restore_irqs)
		__asm__ volatile("csrs mstatus, %0" : : "r"(mie) : "memory");
}

/*
 * The SG2002 semaphore acquires when a non-zero owner token is written to an
 * idle field and releases when that owner writes the same token again.  The
 * vendor protocol reserves the low byte for Linux and the high byte for the
 * C906L.  Local IRQ masking serializes the C906L task and ISR users without
 * depending on scheduler state, which is also required during early setup.
 */
static uint16_t mailbox_lock_next_token(void)
{
	mailbox_lock_counter++;
	if (mailbox_lock_counter == 0U)
		mailbox_lock_counter = 1U;
	return (uint16_t)mailbox_lock_counter
		<< SG2002_C906L_MAILBOX_HWSPIN_C906L_TOKEN_SHIFT;
}

static int mailbox_lock_acquire(struct mailbox_lock_guard *guard,
				unsigned int attempts, int terminal_failure)
{
	uint16_t token;

	guard->restore_irqs = mailbox_local_irq_save();
	token = mailbox_lock_next_token();
	for (unsigned int attempt = 0; attempt < attempts; ++attempt) {
		*mailbox_hwspin = token;
		io_fence();
		if (*mailbox_hwspin == token) {
			io_fence();
			guard->token = token;
			return 0;
		}
	}

	if (terminal_failure)
		mailbox_lock_failures++;
	mailbox_local_irq_restore(guard->restore_irqs);
	return MAILBOX_LOCK_ERROR;
}

static int mailbox_lock_release(const struct mailbox_lock_guard *guard)
{
	int result = 0;

	io_fence();
	if (*mailbox_hwspin == guard->token) {
		/* A second write of the owning token releases this hardware field. */
		*mailbox_hwspin = guard->token;
		io_fence();
	} else {
		mailbox_lock_failures++;
		result = MAILBOX_LOCK_ERROR;
	}
	mailbox_local_irq_restore(guard->restore_irqs);
	return result;
}

static int mailbox_isr(int irqn, void *priv)
{
	BaseType_t wake = pdFALSE;
	struct mailbox_lock_guard guard;
	uint8_t pending;
	uint8_t unexpected;
	uint64_t control_word = 0;
	uint64_t vq_word = 0;
	int have_control = 0;
	int have_vq_kick = 0;
	int lock_result;

	(void)irqn;
	(void)priv;
	lock_result = mailbox_lock_acquire(
		&guard, SG2002_C906L_MAILBOX_HWSPIN_IRQ_ACQUIRE_ATTEMPTS, 0);
	if (lock_result != 0) {
		/*
		 * Early misses leave the level interrupt pending so the vendor
		 * dispatcher retries it.  Its claim loop ignores our return value,
		 * therefore a permanently owned semaphore must eventually mask the
		 * PLIC source to avoid trapping forever.  The control task then
		 * publishes the terminal lock-failure diagnostic.
		 */
		mailbox_irq_lock_deferrals++;
		if (mailbox_irq_lock_streak <
		    SG2002_C906L_MAILBOX_HWSPIN_IRQ_CONSECUTIVE_DEFERRAL_LIMIT) {
			mailbox_irq_lock_streak++;
			if (mailbox_irq_lock_streak ==
			    SG2002_C906L_MAILBOX_HWSPIN_IRQ_CONSECUTIVE_DEFERRAL_LIMIT) {
				mailbox_lock_failures++;
				disable_irq(SG2002_C906L_MAILBOX_C906L_IRQ);
			}
		}
		return lock_result;
	}
	mailbox_irq_lock_streak = 0;

	pending = mbox->cpu_mbox_set[SG2002_C906L_RTOS_CPU_ID]
		.cpu_mbox_int_int.mbox_int;
	unexpected = pending & (uint8_t)~(CONTROL_BIT | VQ_KICK_BIT);
	if (pending & CONTROL_BIT) {
		control_word = slots[SG2002_C906L_CHANNEL_CONTROL];
		io_fence();
		mbox->cpu_mbox_set[SG2002_C906L_RTOS_CPU_ID]
			.cpu_mbox_int_clr.mbox_int_clr =
			CONTROL_BIT;
		mbox->cpu_mbox_en[SG2002_C906L_RTOS_CPU_ID].mbox_info &=
			~CONTROL_BIT;
		io_fence();
		have_control = 1;
	}

	if (pending & VQ_KICK_BIT) {
		vq_word = slots[SG2002_C906L_CHANNEL_VQ_KICK];
		io_fence();
		mbox->cpu_mbox_set[SG2002_C906L_RTOS_CPU_ID]
			.cpu_mbox_int_clr.mbox_int_clr =
			VQ_KICK_BIT;
		mbox->cpu_mbox_en[SG2002_C906L_RTOS_CPU_ID].mbox_info &=
			~VQ_KICK_BIT;
		io_fence();
		have_vq_kick = 1;
	}
	if (unexpected != 0U) {
		/* Drain protocol-invalid channels without ever interpreting slots. */
		mbox->cpu_mbox_set[SG2002_C906L_RTOS_CPU_ID]
			.cpu_mbox_int_clr.mbox_int_clr = unexpected;
		mbox->cpu_mbox_en[SG2002_C906L_RTOS_CPU_ID].mbox_info &=
			(uint8_t)~unexpected;
		io_fence();
		for (uint8_t bits = unexpected; bits != 0U; bits >>= 1)
			unexpected_mailbox_events += bits & 1U;
	}
	lock_result = mailbox_lock_release(&guard);

	/* Do not hold the cross-core lock across FreeRTOS queue operations. */
	if (have_control &&
	    xQueueSendFromISR(requests, &control_word, &wake) != pdPASS)
		request_drops++;
	if (have_vq_kick) {
		uint32_t vqid = (uint32_t)vq_word;

		if (xQueueSendFromISR(vq_kicks, &vqid, &wake) != pdPASS)
			vq_kick_drops++;
	}
	portYIELD_FROM_ISR(wake);
	return lock_result;
}

#ifdef SG2002_C906L_TIMER4
static int timer4_isr(int irqn, void *priv)
{
	(void)irqn;
	(void)priv;
	/*
	 * Rust reads Timer4's per-channel EOI and masks/disables the channel
	 * before returning.  The vendor dispatcher completes the PLIC claim only
	 * after this trampoline returns.
	 */
	return c906l_timer4_interrupt();
}

int c906l_timer4_irq_install(void)
{
	return request_irq(SG2002_C906L_TIMER4_IRQ, timer4_isr, 0,
			   "c906l-timer4", NULL);
}

void c906l_timer4_irq_disable(void)
{
	disable_irq(SG2002_C906L_TIMER4_IRQ);
}
#endif

int c906l_platform_start(rust_task_t control_task, rust_task_t rpmsg_task)
{
	BaseType_t task_result;
	struct mailbox_lock_guard guard;
	int irq_result;
	int lock_result;

	requests = xQueueCreate(8U, sizeof(uint64_t));
	if (requests == NULL)
		return -1;
	vq_kicks = xQueueCreate(32U, sizeof(uint32_t));
	if (vq_kicks == NULL)
		return -2;

	request_drops = 0;
	vq_kick_drops = 0;
	mailbox_lock_failures = 0;
	mailbox_irq_lock_deferrals = 0;
	mailbox_irq_lock_streak = 0;
	unexpected_mailbox_events = 0;
	lock_result = mailbox_lock_acquire(
		&guard, SG2002_C906L_MAILBOX_HWSPIN_TASK_ACQUIRE_ATTEMPTS, 1);
	if (lock_result != 0)
		return -7;
	mbox->cpu_mbox_set[SG2002_C906L_RTOS_CPU_ID]
		.cpu_mbox_int_clr.mbox_int_clr =
		(uint8_t)((1U << SG2002_C906L_MAILBOX_SLOT_COUNT) - 1U);
	mbox->cpu_mbox_en[SG2002_C906L_RTOS_CPU_ID].mbox_info &=
		(uint8_t)~((1U << SG2002_C906L_MAILBOX_SLOT_COUNT) - 1U);
	io_fence();
	if (mailbox_lock_release(&guard) != 0)
		return -8;

	irq_result = request_irq(SG2002_C906L_MAILBOX_C906L_IRQ, mailbox_isr, 0,
				 "c906l-mailbox", NULL);
	if (irq_result != 0)
		return -3;

	task_result = xTaskCreate(control_task, "c906l-control", 1024U, NULL,
				  tskIDLE_PRIORITY + 2U, NULL);
	if (task_result != pdPASS)
		return -4;

	task_result = xTaskCreate(rpmsg_task, "c906l-rpmsg", 2048U, NULL,
				  tskIDLE_PRIORITY + 2U, NULL);
	if (task_result != pdPASS)
		return -5;

	vTaskStartScheduler();
	return -6;
}

int c906l_request_receive(uint64_t *word, uint32_t timeout_ticks)
{
	return xQueueReceive(requests, word, timeout_ticks) == pdPASS ? 0 : -1;
}

static int mailbox_send(unsigned int channel, uint64_t word,
			unsigned int retries)
{
	uint8_t bit = (uint8_t)(1U << channel);

	for (unsigned int retry = 0; retry < retries; ++retry) {
		struct mailbox_lock_guard guard;
		int busy = 0;
		int sent = 0;

		if (mailbox_lock_acquire(
			    &guard,
			    SG2002_C906L_MAILBOX_HWSPIN_TASK_ACQUIRE_ATTEMPTS,
			    1) != 0)
			return MAILBOX_LOCK_ERROR;
		for (unsigned int processor = 0;
		     processor < SG2002_C906L_MAILBOX_PROCESSOR_COUNT;
		     ++processor) {
			if (mbox->cpu_mbox_en[processor].mbox_info & bit) {
				busy = 1;
				break;
			}
		}
		if (!busy) {
			slots[channel] = word;
			io_fence();
			mbox->cpu_mbox_set[SG2002_C906L_LINUX_CPU_ID]
				.cpu_mbox_int_clr.mbox_int_clr = bit;
			mbox->cpu_mbox_en[SG2002_C906L_LINUX_CPU_ID].mbox_info |= bit;
			mbox->mbox_set.mbox_set = bit;
			io_fence();
			sent = 1;
		}
		if (mailbox_lock_release(&guard) != 0)
			return MAILBOX_LOCK_ERROR;
		if (sent)
			return 0;
		if (retry + 1U < retries)
			vTaskDelay(1U);
	}
	return MAILBOX_BUSY;
}

int c906l_response_send(uint64_t word)
{
	return mailbox_send(SG2002_C906L_CHANNEL_CONTROL, word,
			    RESPONSE_RETRIES);
}

int c906l_vq_kick_receive(uint32_t *vqid, uint32_t timeout_ticks)
{
	return xQueueReceive(vq_kicks, vqid, timeout_ticks) == pdPASS ? 0 : -1;
}

int c906l_vq_notify(uint32_t vqid)
{
	/* Rust retains failed notifications and retries without blocking here. */
	return mailbox_send(SG2002_C906L_CHANNEL_VQ_NOTIFY,
			    (uint64_t)vqid, 1U);
}

uint32_t c906l_request_drops(void)
{
	return request_drops;
}

uint32_t c906l_mailbox_lock_failures(void)
{
	return mailbox_lock_failures;
}

uint32_t c906l_unexpected_mailbox_events(void)
{
	return unexpected_mailbox_events;
}

uint32_t c906l_ticks(void)
{
	return (uint32_t)xTaskGetTickCount();
}

void c906l_cache_clean(uintptr_t address, uintptr_t size)
{
	clean_dcache_range(address, size);
}

void c906l_cache_invalidate(uintptr_t address, uintptr_t size)
{
	inv_dcache_range(address, size);
}

void c906l_delay(uint32_t ticks)
{
	vTaskDelay(ticks);
}

void main_cvirtos(void)
{
	c906l_rust_main();
}
