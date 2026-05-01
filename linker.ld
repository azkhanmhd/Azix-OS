/*
 * Azix OS Linker Script
 * Kernel is loaded at the 1 MiB physical address by GRUB.
 */

ENTRY(_start)

SECTIONS
{
    . = 1M;

    /* Executable code — multiboot header MUST come first */
    .text BLOCK(4K) : ALIGN(4K)
    {
        *(.multiboot)
        *(.text)
    }

    /* Read-only data (string literals, const globals) */
    .rodata BLOCK(4K) : ALIGN(4K)
    {
        *(.rodata)
    }

    /* Initialised read-write data */
    .data BLOCK(4K) : ALIGN(4K)
    {
        *(.data)
    }

    /* Zero-initialised data (BSS) */
    .bss BLOCK(4K) : ALIGN(4K)
    {
        *(COMMON)
        *(.bss)
    }

    /* Symbol at the very end of the kernel image.
       The heap allocator uses this as its start address. */
    _kernel_end = ALIGN(4K);
}
