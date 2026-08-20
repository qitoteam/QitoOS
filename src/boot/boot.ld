/*
 * QitoOS - stage1/stage2 bootloader link script
 *
 * The BIOS loads the El Torito no-emulation boot image at physical 0x7C00.
 * Stage 1 (the first 512 bytes) and stage 2 are linked as one flat blob.
 */
OUTPUT_FORMAT(binary)
ENTRY(_start)

SECTIONS
{
    . = 0x7C00;

    .stage1 : {
        *(.stage1.entry)
        *(.stage1)
        . = 0x1FE;
        SHORT(0xAA55)
    }

    __stage2_start = .;
    .stage2 : {
        *(.stage2.entry)
        *(.stage2)
        *(.rodata*)
        *(.data*)
    }
    .bss : { *(.bss*) *(COMMON) }
    __boot_end = .;

    /DISCARD/ : { *(.note*) *(.comment) *(.eh_frame*) }
}
