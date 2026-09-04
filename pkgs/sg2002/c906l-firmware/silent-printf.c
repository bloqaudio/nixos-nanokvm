/* SPDX-License-Identifier: MIT */

/*
 * The upstream template routes printf through UART0.  UART0 remains owned by
 * Linux in the transport-only profile, including on failure paths, so retain
 * the symbol expected by the BSP without performing any peripheral access.
 */
int printf(const char *format, ...)
{
	(void)format;
	return 0;
}
