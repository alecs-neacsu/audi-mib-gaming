/* SPDX-License-Identifier: GPL-2.0-or-later - part of the audi-mib-gaming launcher, see psxfe.c */
@ apeleaza retro_run() salvand registrele VFP d8-d15 (codul NEON al emulatorului le strica fara sa le salveze)
	.syntax unified
	.arm
	.fpu neon
	.text
	.global run_safe
	.type run_safe, %function
run_safe:
	push	{r4, lr}
	vpush	{d8-d15}
	bl	retro_run
	vpop	{d8-d15}
	pop	{r4, pc}
