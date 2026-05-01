# Azix OS

> Vibe Coded Project :>

A custom 32-bit x86 operating system built from scratch in C and NASM assembly. No libc, no external OS libraries, no borrowed kernel code. It boots via GRUB2, runs in VirtualBox (and on real x86 hardware), and ships with a fully interactive shell, a hand written TCP/IP network stack, and a working HTTP text browser.

---

## Features

| Subsystem | Details |
|---|---|
| **Display** | VESA linear framebuffer, 800×600×32bpp, custom 8×8 bitmap font rendered directly to video memory. No VGA text mode; every pixel is drawn manually. |
| **Keyboard** | PS/2 keyboard driver on IRQ1. Circular ring buffer so no keypresses are dropped. Full Ctrl+C detection to cancel long running operations. |
| **Mouse** | PS/2 mouse driver. Reads 3-byte packets, draws a hardware cursor on screen. |
| **Memory** | Free list heap allocator starting at `_kernel_end`. Supports `kmalloc`, `kfree`, `kcalloc`, `krealloc`. 4 MB heap. Coalesces adjacent free blocks on free. |
| **PCI bus** | Scans all 256 buses × 32 devices × 8 functions using I/O ports 0xCF8/0xCFC. Prints vendor ID, device ID, class/subclass, and BAR0 for every detected device. |
| **NIC** | AMD PCNet-PCI II (Am79C970A) driver — the NIC VirtualBox emulates. SWSTYLE=2, polling mode, 4 RX and 4 TX descriptor rings, 1536-byte buffers. Initialised from BAR0. |
| **Network** | Full Ethernet II + ARP + IPv4 + ICMP + UDP stack, hand written with zero dependencies. Single-entry ARP cache. |
| **DNS** | A-record lookup over UDP port 53 against Google DNS (8.8.8.8). Used automatically when you pass a hostname to `ping` or `browse`. |
| **TCP** | Single-connection client. Full 3-way handshake, MSS negotiation (1460 bytes), ACK tracking, FIN teardown. Polling mode (no IRQ). |
| **Browser** | HTTP/1.0 GET request. Strips HTML tags, decodes HTML entities, word-wraps at 80 columns, paginates at 22 rows with `[-- More --]` prompt. Ctrl+C cancels at any point. |
| **Shell** | Reads line input from keyboard, tokenises, dispatches to built-in commands. Supports: `help`, `about`, `version`, `clear`, `color`, `ram`, `cpu`, `uptime`, `echo`, `reboot`, `shutdown`, `halt`, `mem`, `pci`, `ifconfig`, `ping`, `browse`. |

---

## Shell commands

| Command | Usage | Description |
|---|---|---|
| `help` | `help` | Print all available commands |
| `about` | `about` | Show OS name, architecture, and bootloader info |
| `version` | `version` | Print version number and build date |
| `clear` | `clear` | Clear screen and redraw the boot banner |
| `color` | `color` | Display all colours from the framebuffer palette |
| `ram` | `ram` | Show total RAM detected by GRUB and current heap usage |
| `cpu` | `cpu` | Read and print the CPU vendor string via CPUID |
| `uptime` | `uptime` | Show PIT tick count since boot |
| `echo` | `echo <text>` | Print text back to the terminal |
| `reboot` | `reboot` | Ask y/n then reboot via keyboard controller reset |
| `shutdown` | `shutdown` | Ask y/n then ACPI S5 shutdown |
| `halt` | `halt` | Immediately halt the CPU (HLT instruction) |
| `mem` | `mem` | Dump heap block list — shows allocated and free blocks with sizes |
| `pci` | `pci` | Scan and print all PCI devices in `[BB:DD.F] VendorID:DeviceID Class` format |
| `ifconfig` | `ifconfig` | Show NIC MAC address, IP (10.0.2.15), gateway (10.0.2.2), netmask |
| `ping` | `ping <host> [count]` | ICMP echo to IP or hostname. Default 4 packets. Shows RTT per packet and min/avg/max stats. Ctrl+C stops early and shows partial stats. |
| `browse` | `browse <url>` | Resolve hostname, open TCP connection, send HTTP/1.0 GET, strip HTML, display text. HTTP only (no HTTPS). Press any key to page through content, Ctrl+C to quit. |

---

## Building

### Requirements

You need a Linux environment (WSL2 on Windows works perfectly):

```bash
sudo apt update && sudo apt install -y \
  nasm gcc-i686-linux-gnu binutils-i686-linux-gnu \
  grub-pc-bin grub-common xorriso mtools
```

- **nasm** — assembles `boot.asm` and `idt_asm.asm`
- **gcc-i686-linux-gnu** — cross-compiles 32-bit C without a host libc
- **grub-pc-bin / grub-common** — provides `grub-mkrescue`
- **xorriso + mtools** — `grub-mkrescue` uses these to create the ISO

### Build steps

```bash
# 1. Compile everything
make

# 2. Copy kernel binary into the ISO staging tree
cp azix.kernel iso/boot/

# 3. Build the bootable ISO
grub-mkrescue -o azix_new.iso iso/
```

`make` compiles all `.c` and `.asm` files under `kernel/` and `boot/`, links them with `linker.ld` into `azix.kernel` (ELF32 loaded at 1 MiB).  
`grub-mkrescue` wraps the kernel and `grub.cfg` into a GRUB2 El Torito ISO.

The output is **`azix_new.iso`** — a bootable CD-ROM image.

---

## Running in VirtualBox

1. **New VM** → Type: `Other` → Version: `Other/Unknown (32-bit)`
2. RAM: 64 MB or more
3. No hard disk needed
4. **Settings → Storage** → add `azix_new.iso` as an optical drive <a href="https://drive.google.com/file/d/15T4lB_iZQ55BLIDIXzmPUb34WjMYGeWm/view?usp=sharing" target="_blank">Download ISO</a>
5. **Settings → Network → Adapter 1 → NAT** *(required for ping and browse to work)*
6. **Start** — GRUB menu appears, boots automatically after 3 seconds

> With NAT, VirtualBox gives the OS a fixed IP of `10.0.2.15`, gateway `10.0.2.2`, DNS `8.8.8.8`. The `ifconfig` command will confirm this.

**To test the network:**
```
ping 8.8.8.8
ping google.com
browse http://neverssl.com
```

---

## Project structure

```
Azix-OS/
├── boot/
│   └── boot.asm          # Multiboot v1 header, 16 KiB stack, calls kernel_main()
├── kernel/
│   ├── kernel.c          # Kernel entry point + full shell + command handlers
│   ├── fb.c / .h         # VESA framebuffer driver, GUI terminal, gui_puts/gui_putchar
│   ├── terminal.c / .h   # Legacy VGA text terminal (kept for reference)
│   ├── keyboard.c / .h   # PS/2 driver, IRQ1, ring buffer, Ctrl+C detection
│   ├── mouse.c / .h      # PS/2 mouse driver, cursor rendering
│   ├── idt.c / .h        # IDT + 8259 PIC remapping (IRQ0-7 → INT 0x20-0x27)
│   ├── idt_asm.asm        # IRQ stub wrappers (pushad/popad, iret)
│   ├── heap.c / .h       # Free-list allocator: kmalloc, kfree, kcalloc, krealloc
│   ├── pci.c / .h        # PCI config-space scanner via I/O ports 0xCF8/0xCFC
│   ├── pcnet.c / .h      # AMD PCNet Am79C970A NIC driver (polling)
│   ├── net.c / .h        # Ethernet II + ARP + IPv4 + ICMP + UDP + DNS
│   ├── tcp.c / .h        # TCP client: tcp_connect, tcp_write, tcp_read, tcp_close
│   ├── browser.c / .h    # HTTP/1.0 GET + HTML stripper + paginator
│   ├── io.h              # Inline port I/O: outb, outw, inb, outl, inl
│   └── vga.h             # VGA colour enum
├── grub/
│   └── grub.cfg          # GRUB2 menu entry, sets gfxpayload=800x600x32
├── linker.ld             # ELF linker script — loads kernel at physical 0x100000 (1 MiB)
├── Makefile              # Compile rules for all kernel + boot objects
├── build.sh              # Optional build helper with dependency checks
└── README.md             # This file
```

---

## How it works

1. **GRUB2** reads `grub.cfg`, sets the video mode to 800×600×32bpp via `gfxpayload`, then loads `azix.kernel` as a Multiboot ELF binary at 1 MiB and passes a Multiboot info struct pointer in `ebx`.
2. **`boot.asm`** validates the Multiboot magic, sets up a 16 KiB stack in BSS, saves the Multiboot pointer, and calls `kernel_main()`.
3. **`kernel_main()`** runs the full init sequence: heap → framebuffer → IDT/PIC → keyboard → mouse → PCNet NIC → ARP/IP network stack → draw UI → enter shell loop.
4. The **shell loop** calls `keyboard_getchar()` which reads from the PS/2 ring buffer (filled by IRQ1). Characters are echoed to the framebuffer terminal. On Enter, the line is tokenised and dispatched.
5. **`ping`**: resolves hostname via DNS if needed → ARP for gateway MAC → sends ICMP echo request → polls PCNet RX ring for ICMP echo reply → prints RTT. Repeats for each packet. Ctrl+C flag checked each iteration.
6. **`browse`**: resolves hostname → ARP → TCP 3-way handshake (SYN with MSS option) → sends `GET / HTTP/1.0` → polls for all data until FIN → strips HTML tags → decodes entities → word-wraps at 80 cols → paginates at 22 rows.

---

## Known limitations

- **HTTP only** — no HTTPS/TLS. Any HTTPS URL will fail. Use `http://` sites like `neverssl.com` for testing.
- **Single TCP connection** — only one active connection at a time, no multiplexing.
- **No filesystem** — no disk driver, no FAT/ext2. Everything lives in RAM and is lost on reboot.
- **No userspace** — everything runs at ring 0. No process isolation.
- **No TCP retransmission** — if a packet is dropped, the connection stalls. Works fine on VirtualBox NAT which is lossless.
- **No DHCP** — IP is hardcoded for VirtualBox NAT (10.0.2.15). Real hardware would need DHCP or manual config.

---

## License

MIT — do whatever you want with it.

