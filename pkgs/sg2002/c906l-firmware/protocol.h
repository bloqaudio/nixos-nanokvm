/* SPDX-License-Identifier: MIT */
#ifndef SG2002_C906L_PROTOCOL_H
#define SG2002_C906L_PROTOCOL_H

#include <stdint.h>

/*
 * The hardware mailbox transports one little-endian 64-bit word.  Channel 0
 * is reserved for this control protocol; future OpenAMP vring notifications
 * get their own channels so control traffic cannot consume their doorbells.
 */
#define SG2002_C906L_MAILBOX_CHANNEL 0U
#define SG2002_C906L_LINUX_CPU_ID    1U
#define SG2002_C906L_RTOS_CPU_ID     2U

#define SG2002_C906L_ABI_MAJOR       1U
#define SG2002_C906L_ABI_MINOR       0U

#define SG2002_C906L_SHMEM_BASE      UINT64_C(0x8ff00000)
#define SG2002_C906L_SHMEM_MAGIC     UINT32_C(0x4d564b4e) /* "NKVM" LE */

enum sg2002_c906l_state {
	SG2002_C906L_STATE_BOOTING = 1,
	SG2002_C906L_STATE_RUNNING = 2,
	SG2002_C906L_STATE_FAULT = 3,
};

enum sg2002_c906l_service {
	SG2002_C906L_SERVICE_CONTROL = 1,
};

enum sg2002_c906l_opcode {
	SG2002_C906L_OP_PING = 1,
	SG2002_C906L_OP_GET_ABI = 2,
	SG2002_C906L_OP_GET_CAPABILITIES = 3,
	SG2002_C906L_OP_RESPONSE = 0x80,
	SG2002_C906L_OP_ERROR = 0xff,
};

enum sg2002_c906l_capability {
	SG2002_C906L_CAP_MAILBOX = UINT64_C(1) << 0,
	SG2002_C906L_CAP_SHMEM_HEARTBEAT = UINT64_C(1) << 1,
	/* Set only after the opt-in Timer4 hardware self-test succeeds. */
	SG2002_C906L_CAP_TIMER4_SELF_TEST = UINT64_C(1) << 2,
};

enum sg2002_c906l_status_flag {
	SG2002_C906L_FLAG_RESPONSE_TIMEOUT = UINT32_C(1) << 0,
	SG2002_C906L_FLAG_REQUEST_DROPPED = UINT32_C(1) << 1,
	/* Set when the selected Timer4 lease fails validation or its test. */
	SG2002_C906L_FLAG_TIMER4_SELF_TEST_FAILED = UINT32_C(1) << 2,
};

struct sg2002_c906l_message {
	uint8_t service;
	uint8_t opcode;
	uint16_t sequence;
	uint32_t value;
} __attribute__((packed, aligned(8)));

/*
 * One cache line, owned and written by C906L.  Linux must invalidate before
 * reading; the firmware cleans this line after every update.  New fields may
 * consume reserved[] without changing ABI major or struct_size.
 */
struct sg2002_c906l_status {
	uint32_t magic;
	uint16_t abi_major;
	uint16_t abi_minor;
	uint32_t struct_size;
	uint32_t state;
	uint32_t generation;
	uint32_t flags;
	uint64_t heartbeat;
	uint64_t capabilities;
	uint64_t last_request;
	uint64_t last_response;
	uint8_t reserved[8];
} __attribute__((packed, aligned(64)));

_Static_assert(sizeof(struct sg2002_c906l_message) == 8,
	       "mailbox messages must match the SG2002 8-byte slot");
_Static_assert(sizeof(struct sg2002_c906l_status) == 64,
	       "status block must occupy exactly one C906 cache line");

#endif
