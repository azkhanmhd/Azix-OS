; ============================================================
; Azix OS - Multiboot-compliant boot entry point
; Loaded by GRUB, sets up the stack, then calls kernel_main()
; ============================================================

MBALIGN     equ 1 << 0          ; align loaded modules on page boundaries
MEMINFO     equ 1 << 1          ; provide a memory map
VIDEOMODE   equ 1 << 2          ; request a graphics/video mode from GRUB
FLAGS       equ MBALIGN | MEMINFO | VIDEOMODE
MAGIC       equ 0x1BADB002      ; multiboot magic number
CHECKSUM    equ -(MAGIC + FLAGS) ; checksum must sum to zero with above

section .multiboot
align 4
    dd MAGIC
    dd FLAGS
    dd CHECKSUM
    ; Offsets 12-28: address fields — must occupy these positions even when
    ; bit 16 is NOT set (GRUB always reads mode_type from offset 32).
    dd 0        ; header_addr   (ignored, bit 16 not set)
    dd 0        ; load_addr     (ignored)
    dd 0        ; load_end_addr (ignored)
    dd 0        ; bss_end_addr  (ignored)
    dd 0        ; entry_addr    (ignored)
    ; Offsets 32-44: video mode fields — read by GRUB because bit 2 is set
    dd 0        ; mode_type: 0 = linear graphics framebuffer
    dd 800      ; preferred width
    dd 600      ; preferred height
    dd 32       ; preferred colour depth (bpp)

section .bss
align 16
stack_bottom:
    resb 16384          ; 16 KiB kernel stack
stack_top:

section .text
global _start

_start:
    mov esp, stack_top  ; set up stack pointer

    extern kernel_main
    push ebx            ; arg1: multiboot_info_t* (GRUB stores it in EBX)
    call kernel_main    ; call C kernel entry

    ; If kernel_main ever returns, halt the CPU
    cli
.hang:
    hlt
    jmp .hang
