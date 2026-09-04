/* SPDX-License-Identifier: MIT */
#include "sg2002-c906l-contract.h"

_Static_assert(SG2002_C906L_EXPECTED_CAPABILITIES == UINT64_C(0x0b)
	       || SG2002_C906L_EXPECTED_CAPABILITIES == UINT64_C(0x0f),
	       "unexpected profile capability mask");
_Static_assert(SG2002_C906L_STATUS_SIZE == 64U, "status ABI changed");
_Static_assert(_Alignof(struct sg2002_c906l_message) == 8U,
	       "message ABI alignment changed");
_Static_assert(_Alignof(struct sg2002_c906l_status) == 64U,
	       "status ABI alignment changed");
_Static_assert(SG2002_C906L_STATUS_REGION_SIZE == UINT64_C(0x1000),
	       "status reservation changed");
_Static_assert(SG2002_C906L_RPMSG_PAYLOAD_BYTES == 496U,
	       "RPMsg payload ABI changed");

int contract_header_compiles(void)
{
	return SG2002_C906L_PROFILE_NAME[0] == '\0';
}
