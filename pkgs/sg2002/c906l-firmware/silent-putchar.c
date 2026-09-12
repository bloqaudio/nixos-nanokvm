/* SPDX-License-Identifier: MIT */

/* See silent-printf.c: UART0 is intentionally outside this firmware's ACL. */
int putchar(int character)
{
	return character;
}
