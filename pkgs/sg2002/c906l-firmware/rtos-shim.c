/* SPDX-License-Identifier: MIT */
#include <stdint.h>

#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"

#include "arch_helpers.h"
#include "cvi_mailbox.h"
#include "intr_conf.h"
#include "top_reg.h"

#define CONTROL_CHANNEL 0U
#define CONTROL_BIT (1U << CONTROL_CHANNEL)
#define VQ_KICK_CHANNEL 1U
#define VQ_KICK_BIT (1U << VQ_KICK_CHANNEL)
#define VQ_NOTIFY_CHANNEL 2U
#define LINUX_CPU_ID 1U
#define RTOS_CPU_ID 2U
#define RESPONSE_RETRIES 20U

#ifdef SG2002_C906L_TIMER4
/* SoC Timer4 is the timer IP's one-based Timer5 register group. */
#define TIMER4_C906L_IRQ 55

#if TIMER_INTR_4 != TIMER4_C906L_IRQ
#error "cv181x C906L Timer4 IRQ routing changed"
#endif

/* The pinned BSP implements this API but leaves its prototype commented. */
extern void disable_irq(unsigned int irqn);
#endif

typedef void (*rust_task_t)(void *);

static volatile struct mailbox_set_register *const mbox =
	(volatile struct mailbox_set_register *)(uintptr_t)MAILBOX_REG_BASE;
static volatile uint64_t *const slots =
	(volatile uint64_t *)(uintptr_t)MAILBOX_REG_BUFF;
static QueueHandle_t requests;
static QueueHandle_t vq_kicks;
static volatile uint32_t request_drops;
static volatile uint32_t vq_kick_drops;

extern void c906l_rust_main(void) __attribute__((noreturn));
#ifdef SG2002_C906L_TIMER4
extern int c906l_timer4_interrupt(void);
#endif

static inline void io_fence(void)
{
	__asm__ volatile("fence iorw, iorw" ::: "memory");
}

static int mailbox_isr(int irqn, void *priv)
{
	BaseType_t wake = pdFALSE;
	uint8_t pending;
	uint64_t word;
	uint32_t vqid;

	(void)irqn;
	(void)priv;
	pending = mbox->cpu_mbox_set[RTOS_CPU_ID].cpu_mbox_int_int.mbox_int;
	if (pending & CONTROL_BIT) {
		word = slots[CONTROL_CHANNEL];
		io_fence();
		mbox->cpu_mbox_set[RTOS_CPU_ID].cpu_mbox_int_clr.mbox_int_clr =
			CONTROL_BIT;
		mbox->cpu_mbox_en[RTOS_CPU_ID].mbox_info &= ~CONTROL_BIT;
		io_fence();
		if (xQueueSendFromISR(requests, &word, &wake) != pdPASS)
			request_drops++;
	}

	if (pending & VQ_KICK_BIT) {
		word = slots[VQ_KICK_CHANNEL];
		vqid = (uint32_t)word;
		io_fence();
		mbox->cpu_mbox_set[RTOS_CPU_ID].cpu_mbox_int_clr.mbox_int_clr =
			VQ_KICK_BIT;
		mbox->cpu_mbox_en[RTOS_CPU_ID].mbox_info &= ~VQ_KICK_BIT;
		io_fence();
		if (xQueueSendFromISR(vq_kicks, &vqid, &wake) != pdPASS)
			vq_kick_drops++;
	}
	portYIELD_FROM_ISR(wake);
	return 0;
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
	return request_irq(TIMER4_C906L_IRQ, timer4_isr, 0,
			   "c906l-timer4", NULL);
}

void c906l_timer4_irq_disable(void)
{
	disable_irq(TIMER4_C906L_IRQ);
}
#endif

int c906l_platform_start(rust_task_t control_task, rust_task_t rpmsg_task)
{
	BaseType_t task_result;
	int irq_result;

	requests = xQueueCreate(8U, sizeof(uint64_t));
	if (requests == NULL)
		return -1;
	vq_kicks = xQueueCreate(32U, sizeof(uint32_t));
	if (vq_kicks == NULL)
		return -2;

	request_drops = 0;
	vq_kick_drops = 0;
	mbox->cpu_mbox_set[RTOS_CPU_ID].cpu_mbox_int_clr.mbox_int_clr =
		CONTROL_BIT | VQ_KICK_BIT;
	mbox->cpu_mbox_en[RTOS_CPU_ID].mbox_info &=
		~(CONTROL_BIT | VQ_KICK_BIT);
	slots[CONTROL_CHANNEL] = 0;
	slots[VQ_KICK_CHANNEL] = 0;
	slots[VQ_NOTIFY_CHANNEL] = 0;
	io_fence();

	irq_result = request_irq(MBOX_INT_C906_2ND, mailbox_isr, 0,
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
		taskENTER_CRITICAL();
		if (!(mbox->cpu_mbox_en[LINUX_CPU_ID].mbox_info & bit)) {
			slots[channel] = word;
			io_fence();
			mbox->cpu_mbox_set[LINUX_CPU_ID]
				.cpu_mbox_int_clr.mbox_int_clr = bit;
			mbox->cpu_mbox_en[LINUX_CPU_ID].mbox_info |= bit;
			mbox->mbox_set.mbox_set = bit;
			io_fence();
			taskEXIT_CRITICAL();
			return 0;
		}
		taskEXIT_CRITICAL();
		if (retry + 1U < retries)
			vTaskDelay(1U);
	}
	return -1;
}

int c906l_response_send(uint64_t word)
{
	return mailbox_send(CONTROL_CHANNEL, word, RESPONSE_RETRIES);
}

int c906l_vq_kick_receive(uint32_t *vqid, uint32_t timeout_ticks)
{
	return xQueueReceive(vq_kicks, vqid, timeout_ticks) == pdPASS ? 0 : -1;
}

int c906l_vq_notify(uint32_t vqid)
{
	/* Rust retains failed notifications and retries without blocking here. */
	return mailbox_send(VQ_NOTIFY_CHANNEL, (uint64_t)vqid, 1U);
}

uint32_t c906l_request_drops(void)
{
	return request_drops;
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
