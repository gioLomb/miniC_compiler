	.text
	.globl force_spill
force_spill:
	pushq	%rbp
	movq	%rsp, %rbp
	pushq	%rbx
	pushq	%r12
	pushq	%r13
	movq	%rdi, %rbx
	movq	%rdi, %r8
	movq	%rdi, %rax
	movq	%rdi, %rbx
	movq	%rdi, %r12
	addq	$1, %rbx
	addq	$5, %r12
	addq	$2, %rbx
	movq	%rdi, %r9
	movq	%rdi, %rsi
	movq	%rdi, %r10
	addq	$3, %rax
	addq	$9, %r8
	movq	%rdi, %r11
	movq	%rdi, %rdx
	movq	%rdi, %rcx
	addq	$7, %rdx
	addq	$6, %rcx
	addq	$11, %r10
	movq	%r12, %rsi
	movq	%rdi, %r9
	addq	%rbx, %rbx
	addq	$4, %rsi
	addq	$10, %r9
	movq	%rdi, %r11
	addq	$13, %r11
	movq	%rdi, %r13
	movq	%rdi, %r12
	addq	%r9, %r8
	addq	%rsi, %rax
	movq	%r11, %r12
	movq	%rdi, %r11
	addq	$15, %r11
	addq	%rcx, %rsi
	addq	$12, %r9
	addq	$8, %r12
	addq	$14, %r13
	addq	%r12, %rdx
	addq	%r13, %r12
	addq	%rax, %rbx
	addq	$16, %r11
	addq	%r9, %r10
	movq	%rdi, %rax
	addq	$17, %rax
	addq	%rdx, %rsi
	movq	%rdi, %rcx
	addq	%r11, %r11
	addq	%r10, %r8
	movq	%rdi, %rdx
	addq	$19, %rdx
	addq	%r11, %r12
	addq	$18, %rcx
	addq	%rsi, %rbx
	addq	$20, %rdi
	addq	%rcx, %rax
	addq	%r12, %r8
	addq	%r8, %rbx
	addq	%rdi, %rdx
	addq	%rdx, %rax
	addq	%rax, %rbx
	movq	%rbx, %rax
	popq	%r13
	popq	%r12
	popq	%rbx
	leave
	ret

