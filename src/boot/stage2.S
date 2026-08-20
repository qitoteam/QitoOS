/*
 * QitoOS - Stage 2 bootloader
 *
 * Runs in 16-bit real mode (with an "unreal mode" flat FS selector so that
 * memory above 1 MiB can be written directly) and is responsible for:
 *
 *   1. Enabling the A20 gate.
 *   2. Establishing unreal mode for 32-bit flat access through FS.
 *   3. Collecting the E820 physical memory map.
 *   4. Locating the ACPI RSDP.
 *   5. Loading the kernel image and QitoFS ramdisk over INT 13h extensions.
 *   6. Selecting and programming a VESA/VBE linear framebuffer.
 *   7. Building 4-level page tables covering the low 4 GiB.
 *   8. Switching to 64-bit long mode and entering the kernel.
 *
 * Physical memory layout used while booting:
 *
 *   0x00000500-0x00007C00  real-mode stack
 *   0x00007C00-0x0000FC00  stage 1 + stage 2 image
 *   0x00010000-0x00018000  disk bounce buffer (32 KiB)
 *   0x00018000-0x00019000  struct qito_boot_info
 *   0x00019000-0x00020000  E820 memory map entries
 *   0x00020000-0x00026000  PML4 / PDPT / 4 page directories
 *   0x00100000-            kernel image
 *   0x02000000-            QitoFS ramdisk
 */

    .code16
    .section .stage2.entry, "ax"
    .globl stage2_entry

/* --- Physical layout constants (must match src/boot/bootinfo.h) ------ */
    .set BOUNCE_PHYS,    0x00010000
    .set BOUNCE_SEG,     0x1000
    .set BOUNCE_SECTORS, 16              /* 16 * 2048 = 32 KiB per read   */
    .set BOOTINFO_PHYS,  0x00018000
    .set MMAP_PHYS,      0x00019000
    .set MMAP_MAX,       160
    .set PML4_ADDR,      0x00020000
    .set PDPT_ADDR,      0x00021000
    .set PD_ADDR,        0x00022000      /* 4 directories => 4 GiB        */
    .set KERNEL_PHYS,    0x00100000
    .set RAMDISK_PHYS,   0x02000000
    .set KERNEL_VIRT_LO, 0x80100000      /* low half of the 64-bit entry  */
    .set KERNEL_VIRT_HI, 0xFFFFFFFF

/* ------------------------------------------------------------------ */
/* Payload descriptor - patched in place by tools/mkiso.py             */
/* ------------------------------------------------------------------ */
    .align 16
    .globl qito_payload_table
qito_payload_table:
    .ascii  "QITOPLD1"          /* 0x00 signature located by the builder */
ptbl_kernel_lba:
    .long   0                   /* 0x08 kernel LBA (2048-byte sectors)   */
ptbl_kernel_sectors:
    .long   0                   /* 0x0C kernel length in sectors         */
ptbl_kernel_size:
    .long   0                   /* 0x10 kernel byte size                 */
ptbl_ramdisk_lba:
    .long   0                   /* 0x14 ramdisk LBA                      */
ptbl_ramdisk_sectors:
    .long   0                   /* 0x18 ramdisk length in sectors        */
ptbl_ramdisk_size:
    .long   0                   /* 0x1C ramdisk byte size                */
ptbl_flags:
    .long   0                   /* 0x20 reserved                         */

/* ------------------------------------------------------------------ */
stage2_entry:
    call    serial_init16

    movw    $msg_banner, %si
    call    bios_print

    call    enable_a20
    call    enter_unreal

    call    detect_lowmem
    call    find_rsdp
    call    do_e820

    call    load_payloads
    call    setup_video
    call    fill_bootinfo
    call    build_page_tables

    jmp     enter_long_mode

/* ------------------------------------------------------------------ */
/* A20 gate                                                            */
/* ------------------------------------------------------------------ */
enable_a20:
    call    a20_check
    jc      9f

    movw    $0x2401, %ax        /* BIOS: enable A20                      */
    int     $0x15
    call    a20_check
    jc      9f

    call    kbc_wait_in         /* 8042 keyboard controller route        */
    movb    $0xAD, %al
    outb    %al, $0x64
    call    kbc_wait_in
    movb    $0xD0, %al
    outb    %al, $0x64
    call    kbc_wait_out
    inb     $0x60, %al
    pushw   %ax
    call    kbc_wait_in
    movb    $0xD1, %al
    outb    %al, $0x64
    call    kbc_wait_in
    popw    %ax
    orb     $0x02, %al
    outb    %al, $0x60
    call    kbc_wait_in
    movb    $0xAE, %al
    outb    %al, $0x64
    call    kbc_wait_in
    call    a20_check
    jc      9f

    inb     $0x92, %al          /* fast A20 via System Control Port A    */
    testb   $0x02, %al
    jnz     9f
    orb     $0x02, %al
    andb    $0xFE, %al
    outb    %al, $0x92
9:  ret

/* a20_check - returns CF=1 when the A20 line is enabled. */
a20_check:
    pushw   %ds
    pushw   %es
    pushw   %di
    pushw   %si
    xorw    %ax, %ax
    movw    %ax, %es
    movw    $0xFFFF, %ax
    movw    %ax, %ds
    movw    $0x0500, %di
    movw    $0x0510, %si
    movb    %es:(%di), %al
    pushw   %ax
    movb    %ds:(%si), %al
    pushw   %ax
    movb    $0x00, %es:(%di)
    movb    $0xFF, %ds:(%si)
    cmpb    $0xFF, %es:(%di)
    popw    %ax
    movb    %al, %ds:(%si)
    popw    %ax
    movb    %al, %es:(%di)
    clc
    je      1f
    stc
1:  popw    %si
    popw    %di
    popw    %es
    popw    %ds
    ret

kbc_wait_in:
    inb     $0x64, %al
    testb   $0x02, %al
    jnz     kbc_wait_in
    ret
kbc_wait_out:
    inb     $0x64, %al
    testb   $0x01, %al
    jz      kbc_wait_out
    ret

/* ------------------------------------------------------------------ */
/* Unreal mode                                                         */
/*                                                                     */
/* Briefly enters protected mode to load FS with a 4 GiB flat data      */
/* descriptor, then drops back to real mode. The descriptor cache keeps */
/* the 4 GiB limit, so `%fs:(%eax)` can reach any address below 4 GiB.  */
/* This is called again after BIOS services that may clobber FS.        */
/* ------------------------------------------------------------------ */
    .globl enter_unreal
enter_unreal:
    pushw   %ax
    pushfl
    cli

    lgdtl   gdt_ptr

    movl    %cr0, %eax
    orl     $1, %eax
    movl    %eax, %cr0          /* protected mode, no far jump needed    */

    movw    $GDT_DATA32, %ax
    movw    %ax, %fs

    movl    %cr0, %eax
    andl    $~1, %eax
    movl    %eax, %cr0          /* back to real mode, FS keeps its limit */

    popfl
    popw    %ax
    ret

/* ------------------------------------------------------------------ */
/* Firmware queries                                                    */
/* ------------------------------------------------------------------ */
detect_lowmem:
    clc
    int     $0x12
    jc      1f
    movzwl  %ax, %eax
    movl    %eax, bi_lowmem_kb
1:  ret

/*
 * find_rsdp - scan the EBDA then the 0xE0000-0xFFFFF BIOS area for the
 * ACPI "RSD PTR " signature, validating the checksum of the first 20 bytes.
 */
find_rsdp:
    pushw   %es
    pushw   %ds

    movw    $0x0040, %ax
    movw    %ax, %es
    movw    %es:(0x0E), %ax
    movzwl  %ax, %edx
    shll    $4, %edx
    movl    %edx, bi_ebda

    testw   %ax, %ax
    jz      2f
    movw    %ax, %es
    xorw    %di, %di
    movw    $(1024 / 16), %cx
    call    scan_rsdp
    jnc     3f
2:
    movw    $0xE000, %ax
    movw    %ax, %es
    xorw    %di, %di
    movw    $(0x20000 / 16), %cx
    call    scan_rsdp
    jc      4f
3:
    movw    %es, %ax
    movzwl  %ax, %eax
    shll    $4, %eax
    movzwl  %di, %edx
    addl    %edx, %eax
    movl    %eax, bi_rsdp
4:
    popw    %ds
    popw    %es
    ret

/*
 * scan_rsdp - search ES:DI for the RSDP signature, CX paragraphs.
 * Returns CF=0 and DI = matching offset.
 */
scan_rsdp:
1:  pushw   %cx
    pushw   %di
    pushw   %si
    movw    $rsdp_sig, %si
    movw    $8, %cx
    cld
    repe    cmpsb
    popw    %si
    popw    %di
    popw    %cx
    je      3f
    addw    $16, %di
    loop    1b
    stc
    ret
3:
    /* Verify the 20-byte checksum before accepting the candidate. */
    pushw   %cx
    pushw   %si
    pushw   %bx
    movw    %di, %si
    movw    $20, %cx
    xorb    %bl, %bl
4:  movb    %es:(%si), %al
    addb    %al, %bl
    incw    %si
    loop    4b
    testb   %bl, %bl
    popw    %bx
    popw    %si
    popw    %cx
    jnz     5f
    clc
    ret
5:  addw    $16, %di
    decw    %cx
    jnz     1b
    stc
    ret

rsdp_sig:
    .ascii  "RSD PTR "

/* ------------------------------------------------------------------ */
/* E820 memory map                                                     */
/* ------------------------------------------------------------------ */
do_e820:
    pushw   %es
    pushw   %bp

    /* The map is assembled in a low real-mode window then relocated. */
    movw    $(MMAP_PHYS >> 4), %ax
    movw    %ax, %es
    xorw    %di, %di
    xorl    %ebx, %ebx
    xorw    %bp, %bp
1:
    movl    $0x0000E820, %eax
    movl    $0x534D4150, %edx
    movl    $24, %ecx
    movl    $1, %es:20(%di)     /* default the ACPI 3.0 attribute field  */
    int     $0x15
    jc      3f
    cmpl    $0x534D4150, %eax
    jne     3f
    jcxz    2f
    cmpl    $20, %ecx
    jbe     4f
    testb   $0x01, %es:20(%di)  /* honour the "ignore this entry" bit    */
    jz      2f
4:
    movl    %es:8(%di), %eax    /* drop zero-length regions              */
    orl     %es:12(%di), %eax
    jz      2f
    incw    %bp
    cmpw    $MMAP_MAX, %bp
    jae     3f
    addw    $24, %di
2:
    testl   %ebx, %ebx
    jnz     1b
3:
    movzwl  %bp, %eax
    movl    %eax, bi_mmap_count
    popw    %bp
    popw    %es
    ret

/* ------------------------------------------------------------------ */
/* Payload loading over INT 13h extensions                             */
/* ------------------------------------------------------------------ */
    .align 4
dap:
    .byte   0x10
    .byte   0
dap_count:  .word 0
dap_off:    .word 0
dap_seg:    .word 0
dap_lba:    .quad 0

cur_lba:    .long 0
cur_dest:   .long 0
cur_left:   .long 0

load_payloads:
    movw    $msg_kernel, %si
    call    bios_print
    movl    ptbl_kernel_lba, %eax
    movl    ptbl_kernel_sectors, %ecx
    movl    $KERNEL_PHYS, %edx
    call    read_to_phys
    movw    $msg_ok, %si
    call    bios_print

    movl    ptbl_ramdisk_sectors, %ecx
    testl   %ecx, %ecx
    jz      1f
    movw    $msg_ramdisk, %si
    call    bios_print
    movl    ptbl_ramdisk_lba, %eax
    movl    ptbl_ramdisk_sectors, %ecx
    movl    $RAMDISK_PHYS, %edx
    call    read_to_phys
    movw    $msg_ok, %si
    call    bios_print
1:  ret

/*
 * read_to_phys - copy a run of CD sectors to an arbitrary physical address.
 *   EAX = first LBA, ECX = sector count, EDX = destination physical address
 */
read_to_phys:
    pushal
    movl    %eax, cur_lba
    movl    %edx, cur_dest
    movl    %ecx, cur_left
1:
    cmpl    $0, cur_left
    je      9f

    movl    cur_left, %ecx
    cmpl    $BOUNCE_SECTORS, %ecx
    jbe     2f
    movl    $BOUNCE_SECTORS, %ecx
2:
    movw    %cx, dap_count
    movw    $0, dap_off
    movw    $BOUNCE_SEG, dap_seg
    movl    cur_lba, %eax
    movl    %eax, dap_lba
    movl    $0, (dap_lba + 4)

    pushl   %ecx
    movw    $dap, %si
    movb    $0x42, %ah
    movb    boot_drive, %dl
    int     $0x13
    popl    %ecx
    jc      disk_error

    /* Move the chunk above 1 MiB through the flat FS selector. */
    pushl   %ecx
    call    enter_unreal
    popl    %ecx

    pushl   %ecx
    movl    $BOUNCE_PHYS, %esi
    movl    cur_dest, %edi
    shll    $9, %ecx            /* sectors * 2048 / 4 = dwords           */
    call    copy_flat
    popl    %ecx

    /* Advance. */
    movl    %ecx, %eax
    addl    %eax, cur_lba
    subl    %eax, cur_left
    shll    $11, %eax           /* sectors * 2048 bytes                  */
    addl    %eax, cur_dest

    movw    $msg_dot, %si
    call    bios_print
    jmp     1b
9:
    popal
    ret

/*
 * copy_flat - copy ECX dwords from physical ESI to physical EDI using the
 * unreal-mode FS selector. Both addresses may live above 1 MiB.
 */
copy_flat:
    testl   %ecx, %ecx
    jz      2f
1:  movl    %fs:(%esi), %eax
    movl    %eax, %fs:(%edi)
    addl    $4, %esi
    addl    $4, %edi
    decl    %ecx
    jnz     1b
2:  ret

disk_error:
    movw    $msg_diskerr, %si
    call    bios_print
boot_halt:
    cli
1:  hlt
    jmp     1b

/* ------------------------------------------------------------------ */
/* VESA BIOS Extensions                                                */
/* ------------------------------------------------------------------ */
    .align 4
vbe_info:       .space 512
vbe_mode_info:  .space 256
mode_list:      .space 512      /* local copy of the VBE mode number list */

/* Candidate resolutions in descending order of preference. */
pref_modes:
    .word   1280, 720
    .word   1024, 768
    .word   800,  600
    .word   640,  480
pref_modes_end:

want_w:     .word 0
want_h:     .word 0
want_bpp:   .word 0
found_mode: .word 0xFFFF

setup_video:
    pushw   %es
    pushw   %ds

    xorw    %ax, %ax
    movw    %ax, %es
    movw    $vbe_info, %di
    movl    $0x32454256, %es:(%di)      /* ask for VBE 2.0+ information  */
    movw    $0x4F00, %ax
    int     $0x10
    cmpw    $0x004F, %ax
    jne     no_video

    call    copy_mode_list

    /* Two passes: prefer 32bpp, then accept 24bpp. */
    movw    $32, want_bpp
    call    try_resolutions
    jnc     got_mode
    movw    $24, want_bpp
    call    try_resolutions
    jc      no_video

got_mode:
    movw    found_mode, %bx
    orw     $0x4000, %bx                /* request the linear framebuffer */
    movw    $0x4F02, %ax
    int     $0x10
    cmpw    $0x004F, %ax
    jne     no_video

    movb    $1, bi_fb_valid
    popw    %ds
    popw    %es
    ret

no_video:
    movb    $0, bi_fb_valid
    movw    $msg_novideo, %si
    call    bios_print
    popw    %ds
    popw    %es
    ret

/*
 * copy_mode_list - pull the far-pointer mode list into `mode_list` so the
 * rest of the search can run with DS pointing at our own segment.
 */
copy_mode_list:
    pushw   %ds
    pushw   %es
    pushal
    xorw    %ax, %ax
    movw    %ax, %es
    movw    $mode_list, %di
    movw    (vbe_info + 14), %si
    movw    (vbe_info + 16), %ax
    movw    %ax, %ds
    movw    $255, %cx
    cld
1:  lodsw
    stosw
    cmpw    $0xFFFF, %ax
    je      2f
    loop    1b
    xorw    %ax, %ax
    decw    %ax
    stosw
2:  popal
    popw    %es
    popw    %ds
    ret

/* try_resolutions - walk pref_modes for the current want_bpp. */
try_resolutions:
    pushw   %bx
    movw    $pref_modes, %bx
1:
    cmpw    $pref_modes_end, %bx
    jae     8f
    movw    0(%bx), %ax
    movw    %ax, want_w
    movw    2(%bx), %ax
    movw    %ax, want_h
    pushw   %bx
    call    search_mode_list
    popw    %bx
    jnc     9f
    addw    $4, %bx
    jmp     1b
8:  popw    %bx
    stc
    ret
9:  popw    %bx
    clc
    ret

/* search_mode_list - test every mode number against the current request. */
search_mode_list:
    pushw   %si
    movw    $mode_list, %si
1:
    movw    (%si), %cx
    cmpw    $0xFFFF, %cx
    je      8f
    addw    $2, %si
    pushw   %si
    call    probe_mode
    popw    %si
    jnc     9f
    jmp     1b
8:  popw    %si
    stc
    ret
9:  popw    %si
    clc
    ret

/*
 * probe_mode - query mode CX and check it against want_w/want_h/want_bpp.
 * On success records the framebuffer geometry in the boot info staging area.
 */
probe_mode:
    pushw   %es
    pushw   %di
    pushw   %ax

    xorw    %ax, %ax
    movw    %ax, %es
    movw    $vbe_mode_info, %di
    movw    $0x4F01, %ax
    int     $0x10
    cmpw    $0x004F, %ax
    jne     9f

    movw    (vbe_mode_info + 0), %ax
    testw   $0x0001, %ax            /* mode supported                    */
    jz      9f
    testw   $0x0010, %ax            /* graphics (not text)               */
    jz      9f
    testw   $0x0080, %ax            /* linear framebuffer available      */
    jz      9f

    cmpb    $6, (vbe_mode_info + 27)  /* direct colour memory model      */
    jne     9f

    movw    (vbe_mode_info + 18), %ax
    cmpw    want_w, %ax
    jne     9f
    movw    (vbe_mode_info + 20), %ax
    cmpw    want_h, %ax
    jne     9f
    movzbw  (vbe_mode_info + 25), %ax
    cmpw    want_bpp, %ax
    jne     9f

    movw    %cx, found_mode
    movl    (vbe_mode_info + 40), %eax
    movl    %eax, bi_fb_addr
    movzwl  (vbe_mode_info + 16), %eax
    movl    %eax, bi_fb_pitch
    movw    (vbe_mode_info + 18), %ax
    movw    %ax, bi_fb_w
    movw    (vbe_mode_info + 20), %ax
    movw    %ax, bi_fb_h
    movb    (vbe_mode_info + 25), %al
    movb    %al, bi_fb_bpp
    /*
     * Colour channel layout. In the VBE ModeInfoBlock each channel is a
     * (MaskSize, FieldPosition) pair, mask size first:
     *   31 RedMaskSize    32 RedFieldPosition
     *   33 GreenMaskSize  34 GreenFieldPosition
     *   35 BlueMaskSize   36 BlueFieldPosition
     */
    movb    (vbe_mode_info + 31), %al
    movb    %al, bi_fb_rsz
    movb    (vbe_mode_info + 32), %al
    movb    %al, bi_fb_rs
    movb    (vbe_mode_info + 33), %al
    movb    %al, bi_fb_gsz
    movb    (vbe_mode_info + 34), %al
    movb    %al, bi_fb_gs
    movb    (vbe_mode_info + 35), %al
    movb    %al, bi_fb_bsz
    movb    (vbe_mode_info + 36), %al
    movb    %al, bi_fb_bs

    popw    %ax
    popw    %di
    popw    %es
    clc
    ret
9:
    popw    %ax
    popw    %di
    popw    %es
    stc
    ret

/* ------------------------------------------------------------------ */
/* Boot information staging area                                       */
/* Mirrors `struct qito_boot_info` from src/boot/bootinfo.h.            */
/* ------------------------------------------------------------------ */
    .align 8
bi_start:
bi_magic:       .long   0x51544249
bi_version:     .long   1
bi_fb_addr:     .quad   0
bi_fb_pitch:    .long   0
bi_fb_w:        .word   0
bi_fb_h:        .word   0
bi_fb_bpp:      .byte   0
bi_fb_rs:       .byte   0
bi_fb_rsz:      .byte   0
bi_fb_gs:       .byte   0
bi_fb_gsz:      .byte   0
bi_fb_bs:       .byte   0
bi_fb_bsz:      .byte   0
bi_fb_valid:    .byte   0
bi_mmap_count:  .long   0
bi_mmap_esz:    .long   24
bi_mmap_addr:   .quad   MMAP_PHYS
bi_kern_phys:   .quad   KERNEL_PHYS
bi_kern_size:   .quad   0
bi_rd_phys:     .quad   RAMDISK_PHYS
bi_rd_size:     .quad   0
bi_drive:       .byte   0
bi_res0:        .space  7
bi_rsdp:        .quad   0
bi_ebda:        .quad   0
bi_lowmem_kb:   .long   0
bi_res1:        .long   0
/*
 * The kernel command line. tools/mkiso.py locates the "QITOCMDLINE:" prefix
 * and overwrites the text that follows it, so an image can be built with
 * different boot options without rebuilding the loader.
 */
bi_cmdline:     .ascii  "QITOCMDLINE:"
                .asciz  "root=qitofs console=fb"
                .space  256 - 12 - 23
bi_end:
    .set BI_SIZE, bi_end - bi_start

/* fill_bootinfo - finalise the block and publish it at BOOTINFO_PHYS. */
fill_bootinfo:
    pushal
    movb    boot_drive, %al
    movb    %al, bi_drive
    movl    ptbl_kernel_size, %eax
    movl    %eax, bi_kern_size
    movl    ptbl_ramdisk_size, %eax
    movl    %eax, bi_rd_size

    call    enter_unreal

    /* Byte-wise copy so odd structure sizes are handled correctly. */
    movl    $0, %esi
    movw    $bi_start, %si
    movl    $BOOTINFO_PHYS, %edi
    movl    $BI_SIZE, %ecx
1:  movb    %fs:(%esi), %al
    movb    %al, %fs:(%edi)
    incl    %esi
    incl    %edi
    decl    %ecx
    jnz     1b
    popal
    ret

/* ------------------------------------------------------------------ */
/* Page tables                                                         */
/*                                                                     */
/* Maps the low 4 GiB with 2 MiB pages, both identity mapped and        */
/* mirrored at 0xFFFFFFFF80000000 for the higher-half kernel.           */
/* ------------------------------------------------------------------ */
build_page_tables:
    pushal
    call    enter_unreal

    /* Zero PML4 + PDPT + 4 page directories (6 * 4 KiB). */
    movl    $PML4_ADDR, %edi
    movl    $((6 * 4096) / 4), %ecx
    xorl    %eax, %eax
1:  movl    %eax, %fs:(%edi)
    addl    $4, %edi
    decl    %ecx
    jnz     1b

    /* PML4[0] and PML4[511] both reference the single PDPT. */
    movl    $(PDPT_ADDR | 0x03), %eax
    movl    $PML4_ADDR, %edi
    movl    %eax, %fs:(%edi)
    movl    $0, %fs:4(%edi)
    movl    $(PML4_ADDR + 511 * 8), %edi
    movl    %eax, %fs:(%edi)
    movl    $0, %fs:4(%edi)

    /* PDPT[0..3] cover the identity mapping of the low 4 GiB. */
    movl    $PDPT_ADDR, %edi
    movl    $(PD_ADDR | 0x03), %eax
    movl    $4, %ecx
2:  movl    %eax, %fs:(%edi)
    movl    $0, %fs:4(%edi)
    addl    $0x1000, %eax
    addl    $8, %edi
    decl    %ecx
    jnz     2b

    /* PDPT[510] aliases the first directory so 0xFFFFFFFF80000000 -> 0. */
    movl    $(PDPT_ADDR + 510 * 8), %edi
    movl    $(PD_ADDR | 0x03), %eax
    movl    %eax, %fs:(%edi)
    movl    $0, %fs:4(%edi)

    /* 2048 large pages of 2 MiB = 4 GiB. */
    movl    $PD_ADDR, %edi
    xorl    %eax, %eax
    xorl    %edx, %edx
    movl    $2048, %ecx
3:  movl    %eax, %ebx
    orl     $0x83, %ebx         /* present | writable | page size        */
    movl    %ebx, %fs:(%edi)
    movl    %edx, %fs:4(%edi)
    addl    $8, %edi
    addl    $0x200000, %eax
    adcl    $0, %edx
    decl    %ecx
    jnz     3b

    popal
    ret

/* ------------------------------------------------------------------ */
/* Long mode switch                                                    */
/* ------------------------------------------------------------------ */
enter_long_mode:
    movw    $msg_lm, %si
    call    bios_print

    cli
    lgdtl   gdt_ptr

    movl    %cr4, %eax
    orl     $((1 << 5) | (1 << 7)), %eax    /* PAE | PGE                 */
    movl    %eax, %cr4

    movl    $PML4_ADDR, %eax
    movl    %eax, %cr3

    movl    $0xC0000080, %ecx               /* IA32_EFER                 */
    rdmsr
    orl     $((1 << 8) | (1 << 11)), %eax   /* LME | NXE                 */
    wrmsr

    movl    %cr0, %eax
    orl     $((1 << 31) | (1 << 16) | 1), %eax  /* PG | WP | PE          */
    movl    %eax, %cr0

    ljmpl   $GDT_CODE64, $long_entry

/* ------------------------------------------------------------------ */
/* Global descriptor table shared by unreal mode and long mode          */
/* ------------------------------------------------------------------ */
    .align 16
gdt_base:
    .quad   0x0000000000000000              /* 0x00 null                 */
    .quad   0x00AF9A000000FFFF              /* 0x08 64-bit code          */
    .quad   0x00CF92000000FFFF              /* 0x10 flat 32-bit data     */
    .quad   0x00CF9A000000FFFF              /* 0x18 flat 32-bit code     */
gdt_end:

    .set GDT_CODE64, 0x08
    .set GDT_DATA32, 0x10
    .set GDT_CODE32, 0x18

gdt_ptr:
    .word   gdt_end - gdt_base - 1
    .long   gdt_base

/* ------------------------------------------------------------------ */
    .code64
    .align 16
long_entry:
    movw    $GDT_DATA32, %ax
    movw    %ax, %ds
    movw    %ax, %es
    movw    %ax, %fs
    movw    %ax, %gs
    movw    %ax, %ss

    movq    $0x0000000000090000, %rsp
    xorq    %rbp, %rbp

    /* System V: first argument in RDI, magic in RAX per boot protocol. */
    movq    $BOOTINFO_PHYS, %rdi
    movq    $0x51544249, %rax

    /* Jump to the higher-half kernel entry point via RCX so RAX keeps magic. */
    movabsq $((KERNEL_VIRT_HI << 32) | KERNEL_VIRT_LO), %rcx
    jmp     *%rcx

1:  hlt
    jmp     1b

/* ------------------------------------------------------------------ */
    .code16
    .section .stage2, "ax"
msg_banner:   .asciz "QitoOS stage2\r\n"
msg_kernel:   .asciz "kernel "
msg_ramdisk:  .asciz "\r\nramdisk "
msg_ok:       .asciz " ok\r\n"
msg_lm:       .asciz "long mode\r\n"
msg_novideo:  .asciz "VBE unavailable\r\n"
msg_diskerr:  .asciz "\r\ndisk error\r\n"
msg_dot:      .asciz "."
