    .text
    .globl _start
_start:
    b       code              // code0
    .word   0                 // code1
    .quad   0                 // text_offset
    .quad   0x10000           // image_size
    .quad   0xA               // flags: LE, 4K pages, place anywhere
    .quad   0, 0, 0           // res2-4
    .ascii  "ARMd"            // magic
    .word   0                 // res5
code:
    ldr     x0, =0x5C000000   // splash / continuous-splash framebuffer
    ldr     x1, =0x003C0000   // 0xF00000 bytes / 4 = whole splash region
    ldr     w2, =0xFFFF0000   // A=FF R=FF (BGRA/XRGB) -> red
1:  str     w2, [x0], #4
    subs    x1, x1, #1
    b.ne    1b
    dsb     sy
2:  wfe
    b       2b
    .ltorg
    .balign 0x1000
