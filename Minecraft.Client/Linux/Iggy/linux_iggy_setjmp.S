/*
 * linux_iggy_setjmp.S - a FreeBSD-layout setjmp/longjmp, for the Iggy core only.
 *
 * WHY THIS FILE EXISTS
 *
 * The vendor Iggy core we link (libiggy_orbis.a) was compiled against PS4's
 * FreeBSD-derived headers, where amd64 `jmp_buf` is _JBLEN == 12 longs == 96 bytes.
 * glibc's `jmp_buf` is ~200 bytes, because it also stores a signal mask.
 *
 * Iggy embeds jmp_buf *inside its own structs* (it uses setjmp/longjmp for error
 * unwinding out of SWF parsing, AS3 execution, the allocator and the draw path -
 * eleven objects reference it). Those structs were laid out at compile time with the
 * 96-byte size baked in. Calling glibc's setjmp on one would write ~104 bytes past
 * the end of the field and silently corrupt whatever follows it.
 *
 * So we supply our own, writing only within 96 bytes and using FreeBSD's own slot
 * layout.
 *
 * WHY THE ODD NAMES
 *
 * These are deliberately *not* called setjmp/longjmp. Defining those symbols here
 * would override libc's for the entire Minecraft binary, changing behaviour for any
 * other code that uses them. Instead the build rewrites the references inside the
 * vendor objects only:
 *
 *   objcopy --redefine-sym setjmp=iggy_fbsd_setjmp \
 *           --redefine-sym longjmp=iggy_fbsd_longjmp
 *
 * (see patch_orbis_iggy.py). Everything else in the process keeps glibc's.
 *
 * SLOT LAYOUT (FreeBSD lib/libc/amd64/gen/_setjmp.S)
 *
 *    0  return address        32  r12        64  fpu control word (16 bits)
 *    8  rbx                   40  r13        68  mxcsr
 *   16  rsp                   48  r14        72  (signal mask - unused here)
 *   24  rbp                   56  r15        80..95 (pad)
 *
 * The FPU control word and mxcsr are saved and restored on purpose: Iggy's shape
 * rasteriser and the AS3 numeric paths use SSE, and a longjmp out of them should
 * restore rounding/exception state rather than leave whatever the aborted code set.
 * The signal mask is not saved - Iggy never longjmps out of a signal handler, and
 * skipping it is what makes this a plain register save.
 */

	.text

	.globl	iggy_fbsd_setjmp
	.type	iggy_fbsd_setjmp, @function
	.align	16
iggy_fbsd_setjmp:
	movq	(%rsp), %rdx		/* our return address = where setjmp resumes */
	movq	%rdx,  0(%rdi)
	movq	%rbx,  8(%rdi)
	leaq	8(%rsp), %rdx		/* caller's rsp, i.e. after popping our retaddr */
	movq	%rdx, 16(%rdi)
	movq	%rbp, 24(%rdi)
	movq	%r12, 32(%rdi)
	movq	%r13, 40(%rdi)
	movq	%r14, 48(%rdi)
	movq	%r15, 56(%rdi)
	fnstcw	64(%rdi)
	stmxcsr	68(%rdi)
	xorl	%eax, %eax		/* direct call returns 0 */
	ret
	.size	iggy_fbsd_setjmp, .-iggy_fbsd_setjmp

	.globl	iggy_fbsd_longjmp
	.type	iggy_fbsd_longjmp, @function
	.align	16
iggy_fbsd_longjmp:
	movl	%esi, %eax		/* the value setjmp should appear to return */
	testl	%eax, %eax
	jnz	1f
	movl	$1, %eax		/* longjmp(buf, 0) must still not return 0 */
1:
	movq	 8(%rdi), %rbx
	movq	24(%rdi), %rbp
	movq	32(%rdi), %r12
	movq	40(%rdi), %r13
	movq	48(%rdi), %r14
	movq	56(%rdi), %r15
	fldcw	64(%rdi)
	ldmxcsr	68(%rdi)
	movq	16(%rdi), %rsp		/* switch stacks last */
	jmp	*0(%rdi)
	.size	iggy_fbsd_longjmp, .-iggy_fbsd_longjmp

	/* No executable stack, and mark the object as supporting the usual
	   hardening features so it doesn't drag the whole binary's properties down. */
	.section	.note.GNU-stack,"",@progbits
