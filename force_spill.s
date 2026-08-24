	.text
	.globl force_spill
force_spill:
	pushq	%rbp
	movq	%rsp, %rbp
	subq	$144, %rsp
	pushq	%rbx
	pushq	%r12
	pushq	%r13
	pushq	%r14
	pushq	%r15
	movq	%rdi, %r12
	movq	%rdi, %r14
	movq	%rdi, %r8
	movq	%rdi, %rdx
	movq	%rdi, %r9
	addq	$1, %r12
	addq	$5, %r9
	addq	$2, %rdx
	movq	%rdi, %r10
	movq	%rdi, %r11
	movq	%rdi, %r13
	addq	$3, %r8
	addq	$9, %r14
	movq	%rdi, %r15
	movq	%rdi, %rcx
	movq	%rcx, -8(%rbp)
	movq	%rdi, %rcx
	movq	%rcx, -16(%rbp)
	movq	-8(%rbp), %rcx
	addq	$7, %rcx
	movq	%rcx, -8(%rbp)
	movq	-16(%rbp), %rcx
	addq	$6, %rcx
	movq	%rcx, -16(%rbp)
	addq	$11, %r13
	movq	%rdi, %rcx
	addq	%rdx, %r12
	addq	$4, %r11
	addq	$10, %r10
	movq	%rdi, %rsi
	addq	$13, %r15
	movq	%rdi, %rdx
	movq	%rdi, %rbx
	movq	%rbx, -144(%rbp)
	movq	%r14, %rbx
	addq	%r10, %rbx
	addq	%r11, %r8
	movq	-8(%rbp), %r10
	movq	%rdi, %r11
	addq	$15, %rsi
	movq	-16(%rbp), %r14
	movq	%r14, -136(%rbp)
	movq	-136(%rbp), %r14
	movq	%r14, -72(%rbp)
	movq	-72(%rbp), %r14
	movq	%r14, -128(%rbp)
	movq	-128(%rbp), %r14
	movq	%r14, -40(%rbp)
	movq	-40(%rbp), %r14
	movq	%r14, -120(%rbp)
	movq	-120(%rbp), %r14
	movq	%r14, -64(%rbp)
	movq	-64(%rbp), %r14
	movq	%r14, -112(%rbp)
	movq	-112(%rbp), %r14
	movq	%r14, -24(%rbp)
	movq	-24(%rbp), %r14
	movq	%r14, -104(%rbp)
	movq	-104(%rbp), %r14
	movq	%r14, -56(%rbp)
	movq	-56(%rbp), %r14
	movq	%r14, -96(%rbp)
	movq	-96(%rbp), %r14
	movq	%r14, -32(%rbp)
	movq	-32(%rbp), %r14
	movq	%r14, -88(%rbp)
	movq	-88(%rbp), %r14
	movq	%r14, -48(%rbp)
	movq	-48(%rbp), %r14
	movq	%r14, -80(%rbp)
	movq	-80(%rbp), %r14
	addq	%r14, %r9
	addq	$12, %rcx
	movq	-144(%rbp), %r14
	addq	$8, %r14
	movq	%r14, -144(%rbp)
	addq	$14, %rdx
	movq	-144(%rbp), %r14
	addq	%r14, %r10
	addq	%rdx, %r15
	addq	%r8, %r12
	addq	$16, %r11
	addq	%rcx, %r13
	movq	%rdi, %rcx
	addq	$17, %rcx
	addq	%r10, %r9
	movq	%rdi, %rdx
	addq	%r11, %rsi
	addq	%r13, %rbx
	movq	%rdi, %r8
	addq	$19, %r8
	addq	%rsi, %r15
	addq	$18, %rdx
	addq	%r9, %r12
	addq	$20, %rdi
	addq	%rdx, %rcx
	addq	%r15, %rbx
	addq	%rbx, %r12
	addq	%rdi, %r8
	addq	%r8, %rcx
	addq	%rcx, %r12
	movq	%r12, %rax
	popq	%r15
	popq	%r14
	popq	%r13
	popq	%r12
	popq	%rbx
	leave
	ret

