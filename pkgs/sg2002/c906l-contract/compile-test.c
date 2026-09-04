/* SPDX-License-Identifier: MIT */
#include "sg2002-c906l-contract.h"

_Static_assert(SG2002_C906L_EXPECTED_CAPABILITIES == UINT64_C(0x0b)
	       || SG2002_C906L_EXPECTED_CAPABILITIES == UINT64_C(0x0f)
	       || SG2002_C906L_EXPECTED_CAPABILITIES == UINT64_C(0x1b)
	       || SG2002_C906L_EXPECTED_CAPABILITIES == UINT64_C(0x2b)
	       || SG2002_C906L_EXPECTED_CAPABILITIES == UINT64_C(0x4b),
	       "unexpected profile capability mask");
_Static_assert(SG2002_C906L_STATUS_SIZE == 64U, "status ABI changed");
_Static_assert(SG2002_C906L_CAPABILITY_WIRE_WIDTH == 64U,
	       "capability wire width changed");
_Static_assert(_Alignof(struct sg2002_c906l_message) == 8U,
	       "message ABI alignment changed");
_Static_assert(_Alignof(struct sg2002_c906l_status) == 64U,
	       "status ABI alignment changed");
_Static_assert(sizeof(struct sg2002_c906l_manifest) == 128U,
	       "manifest ABI size changed");
_Static_assert(_Alignof(struct sg2002_c906l_manifest) == 64U,
	       "manifest ABI alignment changed");
_Static_assert(sizeof(struct sg2002_c906l_activation_request) == 128U,
	       "activation request ABI size changed");
_Static_assert(_Alignof(struct sg2002_c906l_activation_request) == 64U,
	       "activation request ABI alignment changed");
_Static_assert(SG2002_C906L_STATUS_REGION_SIZE == UINT64_C(0x1000),
	       "status reservation changed");
_Static_assert(SG2002_C906L_RPMSG_PAYLOAD_BYTES == 496U,
	       "RPMsg payload ABI changed");
_Static_assert(SG2002_C906L_MAILBOX_HWSPIN_BASE_ADDRESS ==
	       UINT64_C(0x019000c0), "mailbox hardware-spinlock base changed");
_Static_assert(SG2002_C906L_MAILBOX_HWSPIN_FIELD == 4U,
	       "SPIN_MBOX field changed");
_Static_assert(SG2002_C906L_MAILBOX_HWSPIN_ADDRESS ==
	       UINT64_C(0x019000d0), "SPIN_MBOX register changed");
_Static_assert(SG2002_C906L_MAILBOX_HWSPIN_ACCESS_WIDTH == 2U,
	       "SPIN_MBOX access width changed");
_Static_assert(SG2002_C906L_MAILBOX_HWSPIN_LINUX_TOKEN_MASK ==
	       UINT32_C(0x000000ff), "Linux spin token namespace changed");
_Static_assert(SG2002_C906L_MAILBOX_HWSPIN_C906L_TOKEN_MASK ==
	       UINT32_C(0x0000ff00), "C906L spin token namespace changed");
_Static_assert(SG2002_C906L_MAILBOX_HWSPIN_TASK_ACQUIRE_ATTEMPTS > 0U,
	       "task lock acquisition is unbounded");
_Static_assert(SG2002_C906L_MAILBOX_HWSPIN_IRQ_ACQUIRE_ATTEMPTS > 0U,
	       "IRQ lock acquisition is unbounded");
_Static_assert(SG2002_C906L_MAILBOX_HWSPIN_IRQ_CONSECUTIVE_DEFERRAL_LIMIT == 16U,
	       "IRQ lock deferral bound changed");
_Static_assert(SG2002_C906L_MAILBOX_PROCESSOR_COUNT == 4U,
	       "mailbox processor count changed");
_Static_assert(SG2002_C906L_MAILBOX_CHANNEL_MASK == UINT32_C(0x00000007),
	       "mailbox channel allocation changed");

int contract_header_compiles(void)
{
	return SG2002_C906L_PROFILE_NAME[0] == '\0';
}
