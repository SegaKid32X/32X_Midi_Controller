/* The Genesis-only build: code in ROM from 0, variables in work RAM
 * (test telemetry at 0xFFFF00, the stack below it). */
OUTPUT_ARCH(m68k)
ENTRY(_start)
SECTIONS
{
    .text 0x000000 : {
        KEEP(*(.text.entry))
        *(.text) *(.text.*)
        *(.ramtext)                 /* (the EverDrive driver: no 32X, so it runs from ROM) */
        *(.rodata) *(.rodata.*)
        . = ALIGN(4);
    }
    __data_load = LOADADDR(.text) + SIZEOF(.text);
    .data 0xFF0000 : AT(__data_load) {
        __data_start = .;
        *(.data) *(.data.*)
        . = ALIGN(4);
        __data_end = .;
    }
    .bss : {
        __bss_start = .;
        *(.bss) *(.bss.*) *(COMMON)
        . = ALIGN(4);
        __bss_end = .;
    }
    __stack = 0xFFFE00;
    __rom_base = 0;
    ASSERT(__bss_end <= 0xFFF400, "work RAM: no room left for the stack")
    /DISCARD/ : { *(.comment) *(.note*) }
}
