/*
 * x86_64 dispatch entry points.
 *
 * _objc_msgSend        self, sel, ...
 * _objc_msgSendSuper2  struct objc_super *, sel, ...   (IMP self = receiver)
 * _objc_msgSend_stret  return buffer, self, sel, ...
 *
 * Every variant saves all integer and SSE argument registers, performs the
 * lookup, restores the registers, then tail-jumps to the IMP.
 */
    .text

/* ---- _objc_msgSend ---- */
    .globl _objc_msgSend
    .type _objc_msgSend,@function
_objc_msgSend:
    subq    $184, %rsp
    movq    %rdi, 0(%rsp)
    movq    %rsi, 8(%rsp)
    movq    %rdx, 16(%rsp)
    movq    %rcx, 24(%rsp)
    movq    %r8, 32(%rsp)
    movq    %r9, 40(%rsp)
    movdqu  %xmm0, 56(%rsp)
    movdqu  %xmm1, 72(%rsp)
    movdqu  %xmm2, 88(%rsp)
    movdqu  %xmm3, 104(%rsp)
    movdqu  %xmm4, 120(%rsp)
    movdqu  %xmm5, 136(%rsp)
    movdqu  %xmm6, 152(%rsp)
    movdqu  %xmm7, 168(%rsp)
    call    plsr_objc_lookup_imp@PLT
    movq    %rax, %r11
    movq    0(%rsp), %rdi
    movq    8(%rsp), %rsi
    movq    16(%rsp), %rdx
    movq    24(%rsp), %rcx
    movq    32(%rsp), %r8
    movq    40(%rsp), %r9
    movdqu  56(%rsp), %xmm0
    movdqu  72(%rsp), %xmm1
    movdqu  88(%rsp), %xmm2
    movdqu  104(%rsp), %xmm3
    movdqu  120(%rsp), %xmm4
    movdqu  136(%rsp), %xmm5
    movdqu  152(%rsp), %xmm6
    movdqu  168(%rsp), %xmm7
    addq    $184, %rsp
    testq   %r11, %r11
    je      1f
    jmp     *%r11
1:
    jmp     plsr_objc_unrecognized@PLT
    .size _objc_msgSend, .-_objc_msgSend

/* ---- _objc_msgSendSuper2 ---- */
    .globl _objc_msgSendSuper2
    .type _objc_msgSendSuper2,@function
_objc_msgSendSuper2:
    subq    $184, %rsp
    movq    %rdi, 0(%rsp)
    movq    %rsi, 8(%rsp)
    movq    %rdx, 16(%rsp)
    movq    %rcx, 24(%rsp)
    movq    %r8, 32(%rsp)
    movq    %r9, 40(%rsp)
    movdqu  %xmm0, 56(%rsp)
    movdqu  %xmm1, 72(%rsp)
    movdqu  %xmm2, 88(%rsp)
    movdqu  %xmm3, 104(%rsp)
    movdqu  %xmm4, 120(%rsp)
    movdqu  %xmm5, 136(%rsp)
    movdqu  %xmm6, 152(%rsp)
    movdqu  %xmm7, 168(%rsp)
    call    plsr_objc_lookup_super@PLT
    movq    %rax, %r11
    movq    0(%rsp), %r10          /* objc_super * */
    movq    0(%r10), %r10          /* receiver becomes IMP self */
    movq    8(%rsp), %rsi
    movq    16(%rsp), %rdx
    movq    24(%rsp), %rcx
    movq    32(%rsp), %r8
    movq    40(%rsp), %r9
    movdqu  56(%rsp), %xmm0
    movdqu  72(%rsp), %xmm1
    movdqu  88(%rsp), %xmm2
    movdqu  104(%rsp), %xmm3
    movdqu  120(%rsp), %xmm4
    movdqu  136(%rsp), %xmm5
    movdqu  152(%rsp), %xmm6
    movdqu  168(%rsp), %xmm7
    movq    %r10, %rdi
    addq    $184, %rsp
    testq   %r11, %r11
    je      1f
    jmp     *%r11
1:
    jmp     plsr_objc_unrecognized@PLT
    .size _objc_msgSendSuper2, .-_objc_msgSendSuper2

/* ---- _objc_msgSend_stret (buffer, self, sel, ...) ---- */
    .globl _objc_msgSend_stret
    .type _objc_msgSend_stret,@function
_objc_msgSend_stret:
    subq    $184, %rsp
    movq    %rdi, 0(%rsp)
    movq    %rsi, 8(%rsp)
    movq    %rdx, 16(%rsp)
    movq    %rcx, 24(%rsp)
    movq    %r8, 32(%rsp)
    movq    %r9, 40(%rsp)
    movdqu  %xmm0, 56(%rsp)
    movdqu  %xmm1, 72(%rsp)
    movdqu  %xmm2, 88(%rsp)
    movdqu  %xmm3, 104(%rsp)
    movdqu  %xmm4, 120(%rsp)
    movdqu  %xmm5, 136(%rsp)
    movdqu  %xmm6, 152(%rsp)
    movdqu  %xmm7, 168(%rsp)
    movq    8(%rsp), %rdi
    movq    16(%rsp), %rsi
    call    plsr_objc_lookup_imp@PLT
    movq    %rax, %r11
    movq    0(%rsp), %rdi
    movq    8(%rsp), %rsi
    movq    16(%rsp), %rdx
    movq    24(%rsp), %rcx
    movq    32(%rsp), %r8
    movq    40(%rsp), %r9
    movdqu  56(%rsp), %xmm0
    movdqu  72(%rsp), %xmm1
    movdqu  88(%rsp), %xmm2
    movdqu  104(%rsp), %xmm3
    movdqu  120(%rsp), %xmm4
    movdqu  136(%rsp), %xmm5
    movdqu  152(%rsp), %xmm6
    movdqu  168(%rsp), %xmm7
    addq    $184, %rsp
    testq   %r11, %r11
    je      1f
    jmp     *%r11
1:
    jmp     plsr_objc_unrecognized@PLT
    .size _objc_msgSend_stret, .-_objc_msgSend_stret

/* ---- _objc_msgSendSuper2_stret (buffer, super, sel, ...) ---- */
    .globl _objc_msgSendSuper2_stret
    .type _objc_msgSendSuper2_stret,@function
_objc_msgSendSuper2_stret:
    subq    $184, %rsp
    movq    %rdi, 0(%rsp)          /* stret buffer */
    movq    %rsi, 8(%rsp)          /* objc_super * */
    movq    %rdx, 16(%rsp)         /* sel */
    movq    %rcx, 24(%rsp)
    movq    %r8, 32(%rsp)
    movq    %r9, 40(%rsp)
    movdqu  %xmm0, 56(%rsp)
    movdqu  %xmm1, 72(%rsp)
    movdqu  %xmm2, 88(%rsp)
    movdqu  %xmm3, 104(%rsp)
    movdqu  %xmm4, 120(%rsp)
    movdqu  %xmm5, 136(%rsp)
    movdqu  %xmm6, 152(%rsp)
    movdqu  %xmm7, 168(%rsp)
    movq    8(%rsp), %rdi
    movq    16(%rsp), %rsi
    call    plsr_objc_lookup_super@PLT
    movq    %rax, %r11
    movq    8(%rsp), %r10          /* objc_super * */
    movq    0(%r10), %r10          /* receiver */
    movq    0(%rsp), %rdi          /* IMP self = stret buffer */
    movq    %r10, %rsi             /* IMP sel arg = receiver */
    movq    16(%rsp), %rdx         /* IMP second arg = sel */
    movq    24(%rsp), %rcx
    movq    32(%rsp), %r8
    movq    40(%rsp), %r9
    movdqu  56(%rsp), %xmm0
    movdqu  72(%rsp), %xmm1
    movdqu  88(%rsp), %xmm2
    movdqu  104(%rsp), %xmm3
    movdqu  120(%rsp), %xmm4
    movdqu  136(%rsp), %xmm5
    movdqu  152(%rsp), %xmm6
    movdqu  168(%rsp), %xmm7
    addq    $184, %rsp
    testq   %r11, %r11
    je      1f
    jmp     *%r11
1:
    jmp     plsr_objc_unrecognized@PLT
    .size _objc_msgSendSuper2_stret, .-_objc_msgSendSuper2_stret

    .section .note.GNU-stack,"",@progbits
