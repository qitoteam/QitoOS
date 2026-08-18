/*
 * Qira OS - Stage 1 boot sector (El Torito, no-emulation CD boot)
 *
 * The BIOS loads this image at 0000:7C00 with DL = the emulated drive number
 * and transfers control to offset 0. Because the El Torito entry declares a
 * sector count covering the whole loader, stage 2 is already resident by the
 * time we run; stage 1 only normalises CPU state and hands over.
 *
 * Image layout (matching the isolinux convention):
 *
 *      0x00  short jump over the reserved area
 *      0x08  56-byte boot information table, patched by tools/mkiso.py
 *      0x40  stage 1 code
 *      0x1FE 0xAA55 signature
 *      0x200 stage 2
 *
 * The reserved window at offset 8 is mandatory: the ISO builder writes the
 * boot image's own LBA and length there, so any code placed at that offset
 * would be destroyed.
 */

    .code16
    .section .stage1.entry, "ax"
    .globl _start

_start:
    /* 0x00: hop over the boot information table. */
    jmp     stage1_main
    nop

    /* 0x03-0x07: identification, also keeps the jump target aligned. */
    .ascii  "QIRA1"

    /*
     * 0x08-0x3F: boot information table.
     *   +0x00  LBA of the primary volume descriptor
     *   +0x04  LBA of this boot image
     *   +0x08  length of this boot image in bytes
     *   +0x0C  32-bit checksum of the image beyond byte 64
     *   +0x10  reserved
     */
    .globl boot_info_table
boot_info_table:
    .space  56, 0

    .org    0x40
stage1_main:
    cli
    cld

    /* Normalise the segment registers; some BIOSes enter with junk here. */
    xorw    %ax, %ax
    movw    %ax, %ds
    movw    %ax, %es
    movw    %ax, %ss
    movw    $0x7C00, %sp

    /* Far jump to load CS with a known value (some BIOSes use 07C0:0000). */
    ljmp    $0x0000, $1f
1:
    sti

    /* Preserve the BIOS boot drive number for stage 2. */
    movb    %dl, boot_drive

    /*
     * Bring COM1 up before the first message. Without this the initial
     * characters are emitted at whatever baud rate the firmware left behind
     * and arrive corrupted.
     */
    call    serial_init16

    movw    $msg_stage1, %si
    call    bios_print

    jmp     stage2_entry

/*
 * bios_print - write a NUL terminated string using BIOS teletype output,
 * mirroring each character to the serial port so the boot is observable
 * from a headless emulator.
 *
 * in: %si = string
 */
    .globl bios_print
bios_print:
    pushw   %ax
    pushw   %bx
    pushw   %si
1:  lodsb
    testb   %al, %al
    jz      2f
    pushw   %ax
    movb    $0x0E, %ah
    movw    $0x0007, %bx
    int     $0x10
    popw    %ax
    call    serial_putc16
    jmp     1b
2:  popw    %si
    popw    %bx
    popw    %ax
    ret

/*
 * serial_putc16 - emit AL on COM1 (already configured by serial_init16).
 * Spins with a bounded retry so a missing UART cannot hang the boot.
 */
    .globl serial_putc16
serial_putc16:
    pushw   %ax
    pushw   %cx
    pushw   %dx

    movb    %al, %ah            /* stash the byte                        */
    cmpb    $0x0A, %al          /* translate LF into CR LF               */
    jne     1f
    movw    $0x03FD, %dx
    movw    $0x1000, %cx
0:  inb     %dx, %al
    testb   $0x20, %al
    jnz     2f
    loop    0b
    jmp     3f
2:  movw    $0x03F8, %dx
    movb    $0x0D, %al
    outb    %al, %dx
1:
    movw    $0x03FD, %dx
    movw    $0x1000, %cx
4:  inb     %dx, %al
    testb   $0x20, %al          /* transmit holding register empty?      */
    jnz     5f
    loop    4b
    jmp     3f
5:  movw    $0x03F8, %dx
    movb    %ah, %al
    outb    %al, %dx
3:
    popw    %dx
    popw    %cx
    popw    %ax
    ret

/*
 * serial_init16 - configure COM1 for 115200 8N1 so the loader can report
 * progress before the kernel takes over.
 */
    .globl serial_init16
serial_init16:
    pushw   %ax
    pushw   %dx

    movw    $0x03F9, %dx        /* IER: mask interrupts                  */
    xorb    %al, %al
    outb    %al, %dx

    movw    $0x03FB, %dx        /* LCR: enable DLAB                      */
    movb    $0x80, %al
    outb    %al, %dx

    movw    $0x03F8, %dx        /* divisor low  = 1 (115200 baud)        */
    movb    $0x01, %al
    outb    %al, %dx
    movw    $0x03F9, %dx        /* divisor high = 0                      */
    xorb    %al, %al
    outb    %al, %dx

    movw    $0x03FB, %dx        /* 8 bits, no parity, 1 stop bit         */
    movb    $0x03, %al
    outb    %al, %dx

    movw    $0x03FA, %dx        /* FIFO: enable and clear                */
    movb    $0xC7, %al
    outb    %al, %dx

    movw    $0x03FC, %dx        /* MCR: DTR | RTS | OUT2                 */
    movb    $0x0B, %al
    outb    %al, %dx

    popw    %dx
    popw    %ax
    ret

    .section .stage1, "ax"
    .globl boot_drive
boot_drive:
    .byte   0

msg_stage1:
    .asciz  "Qira OS: stage1\r\n"
