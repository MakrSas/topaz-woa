;
; VOID TopazArmCallSmc (ARM_SMC_ARGS *Args)
; x0..x7 <- Args->Arg0..Arg7, SMC #0, Args->Arg0..Arg7 <- x0..x7 (same as EDK2 ArmCallSmc).
; The struct pointer is kept on the stack: SMCCC may clobber x0-x17.
;
        AREA    |.text|, CODE, READONLY

        EXPORT  TopazArmCallSmc

TopazArmCallSmc PROC
        sub     sp, sp, #16
        str     x0, [sp]
        mov     x9, x0
        ldp     x0, x1, [x9, #0]
        ldp     x2, x3, [x9, #16]
        ldp     x4, x5, [x9, #32]
        ldp     x6, x7, [x9, #48]
        dsb     sy                      ; buffers TZ reads by PA are write-combined
        smc     #0
        ldr     x9, [sp]
        stp     x0, x1, [x9, #0]
        stp     x2, x3, [x9, #16]
        stp     x4, x5, [x9, #32]
        stp     x6, x7, [x9, #48]
        add     sp, sp, #16
        ret
        ENDP

        END
