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
static volatile uint32_t request_drops;

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

	(void)irqn;
	(void)priv;
	pending = mbox->cpu_mbox_set[RTOS_CPU_ID].cpu_mbox_int_int.mbox_int;
	if (!(pending & CONTROL_BIT))
		return 0;

	word = slots[CONTROL_CHANNEL];
	io_fence();
	mbox->cpu_mbox_set[RTOS_CPU_ID].cpu_mbox_int_clr.mbox_int_clr =
		CONTROL_BIT;
	mbox->cpu_mbox_en[RTOS_CPU_ID].mbox_info &= ~CONTROL_BIT;
	io_fence();

	if (xQueueSendFromISR(requests, &word, &wake) != pdPASS)
		request_drops++;
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

int c906l_platform_start(rust_task_t task)
{
	BaseType_t task_result;
	int irq_result;

	requests = xQueueCreate(8U, sizeof(uint64_t));
	if (requests == NULL)
		return -1;

	request_drops = 0;
	mbox->cpu_mbox_set[RTOS_CPU_ID].cpu_mbox_int_clr.mbox_int_clr =
		CONTROL_BIT;
	mbox->cpu_mbox_en[RTOS_CPU_ID].mbox_info &= ~CONTROL_BIT;
	slots[CONTROL_CHANNEL] = 0;
	io_fence();

	irq_result = request_irq(MBOX_INT_C906_2ND, mailbox_isr, 0,
				 "c906l-mailbox", NULL);
	if (irq_result != 0)
		return -2;

	task_result = xTaskCreate(task, "c906l-control", 1024U, NULL,
				  tskIDLE_PRIORITY + 2U, NULL);
	if (task_result != pdPASS)
		return -3;

	vTaskStartScheduler();
	return -4;
}

int c906l_request_receive(uint64_t *word, uint32_t timeout_ticks)
{
	return xQueueReceive(requests, word, timeout_ticks) == pdPASS ? 0 : -1;
}

int c906l_response_send(uint64_t word)
{
	for (unsigned int retry = 0; retry < RESPONSE_RETRIES; ++retry) {
		if (!(mbox->cpu_mbox_en[LINUX_CPU_ID].mbox_info & CONTROL_BIT)) {
			slots[CONTROL_CHANNEL] = word;
			io_fence();
			mbox->cpu_mbox_set[LINUX_CPU_ID]
				.cpu_mbox_int_clr.mbox_int_clr = CONTROL_BIT;
			mbox->cpu_mbox_en[LINUX_CPU_ID].mbox_info |= CONTROL_BIT;
			mbox->mbox_set.mbox_set = CONTROL_BIT;
			io_fence();
			return 0;
		}
		vTaskDelay(1U);
	}
	return -1;
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
