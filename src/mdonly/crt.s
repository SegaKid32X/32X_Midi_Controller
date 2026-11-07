| crt.s - the Genesis-only build: the 68000 vectors, the cartridge header,
| start-up. Then md_main (src/md/md_main.c), as in the 32X build.

        .section .text.entry,"ax"
        .global _start

        .long   __stack, _start
        .long   err2, err3, err4, err5, err6, err7, err8, err9, err10, err11
        .rept   14
        .long   ignore                  | 12-25 (25: level 1)
        .endr
        .long   ext_irq                 | 26: level 2, the GenMDM's strobe (TH, port 2)
        .rept   37
        .long   ignore                  | 27-63 (VINT and HINT are off)
        .endr

| 0x100: the header (tools/romfix.py fills in the checksum and the ROM end;
| tools/mkvfs.py puts the test patches' "PATFS@" at 0x1C8)
        .ascii  "SEGA MEGA DRIVE "
        .ascii  "(C)SK32 2026.NOV"
        .ascii  "MD MIDI CONTROLLER                              "
        .ascii  "MD MIDI CONTROLLER                              "
        .ascii  "GM 00000000-00"
        .word   0
        .ascii  "J6              "
        .long   0x000000, 0x0FFFFF
        .long   0xFF0000, 0xFFFFFF
        .ascii  "            "
        .ascii  "            "
        .ascii  "                                        "
        .ascii  "JUE             "

| 0x200
ignore: rte

        .macro  fatal n
err\n:  move.w  #0x2700,sr
        lea     __stack,sp
        pea     \n
        jsr     md_fatal
        .endm
        fatal   2
        fatal   3
        fatal   4
        fatal   5
        fatal   6
        fatal   7
        fatal   8
        fatal   9
        fatal   10
        fatal   11

| the external interrupt (level 2): as src/md/md_crt.s
        .global ext_irq
ext_irq:
        move.l  a0,-(sp)
        move.l  d0,-(sp)
        tst.b   rx_custom
        bne.b   1f
        moveq   #0,d0                   | the stock stream: just catch it
        move.b  rx_head,d0
        lea     rx_ring,a0
        move.b  0xA10005,0(a0,d0.w)
        addq.b  #1,rx_head
        move.l  (sp)+,d0
        move.l  (sp)+,a0
        rte
1:      move.l  d1,-(sp)                | ours: rx_take(port) answers on TR
        move.l  a1,-(sp)
        moveq   #0,d0
        move.b  0xA10005,d0
        move.l  d0,-(sp)
        jsr     rx_take
        addq.l  #4,sp
        move.l  (sp)+,a1
        move.l  (sp)+,d1
        move.l  (sp)+,d0
        move.l  (sp)+,a0
        rte

_start:
        move.w  #0x2700,sr
        move.b  0xA10001,d0             | TMSS: a console that has it wants "SEGA"
        andi.b  #0x0F,d0
        beq.b   1f
        move.l  #0x53454741,0xA14000
1:      lea     __stack,sp
        lea     __bss_start,a0          | clear .bss
        lea     __bss_end,a1
2:      cmpa.l  a1,a0
        bhs.b   3f
        clr.w   (a0)+
        bra.b   2b
3:      lea     __data_load,a0          | copy .data from ROM
        lea     __data_start,a1
        lea     __data_end,a2
4:      cmpa.l  a2,a1
        bhs.b   5f
        move.w  (a0)+,(a1)+
        bra.b   4b
5:      clr.l   -(sp)                   | md_main(0)
        jsr     md_main
6:      bra.b   6b
