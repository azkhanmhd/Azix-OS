; ============================================================
; Azix OS - IDT / IRQ assembly stubs
; ============================================================

global idt_load
global irq0
global irq1
global irq12

extern timer_tick
extern keyboard_handler
extern mouse_handler
extern exception_handler

; ============================================================
; CPU Exception stubs — ISR 0-31
; Exceptions WITHOUT an error code: push dummy 0, push int#.
; Exceptions WITH    an error code: CPU already pushed it,
;                                   just push int#.
; Both variants jump to common_isr_stub.
; ============================================================

%macro ISR_NOERRCODE 1
global isr%1
isr%1:
    cli
    push dword 0        ; dummy error code
    push dword %1       ; interrupt number
    jmp common_isr_stub
%endmacro

%macro ISR_ERRCODE 1
global isr%1
isr%1:
    cli
    push dword %1       ; interrupt number (error code already on stack)
    jmp common_isr_stub
%endmacro

ISR_NOERRCODE  0    ; #DE  Division Error
ISR_NOERRCODE  1    ; #DB  Debug
ISR_NOERRCODE  2    ;      NMI
ISR_NOERRCODE  3    ; #BP  Breakpoint
ISR_NOERRCODE  4    ; #OF  Overflow
ISR_NOERRCODE  5    ; #BR  Bound Range Exceeded
ISR_NOERRCODE  6    ; #UD  Invalid Opcode
ISR_NOERRCODE  7    ; #NM  Device Not Available
ISR_ERRCODE    8    ; #DF  Double Fault
ISR_NOERRCODE  9    ;      Coprocessor Segment Overrun
ISR_ERRCODE   10    ; #TS  Invalid TSS
ISR_ERRCODE   11    ; #NP  Segment Not Present
ISR_ERRCODE   12    ; #SS  Stack-Segment Fault
ISR_ERRCODE   13    ; #GP  General Protection Fault
ISR_ERRCODE   14    ; #PF  Page Fault
ISR_NOERRCODE 15    ;      Reserved
ISR_NOERRCODE 16    ; #MF  x87 FP Exception
ISR_ERRCODE   17    ; #AC  Alignment Check
ISR_NOERRCODE 18    ; #MC  Machine Check
ISR_NOERRCODE 19    ; #XM  SIMD FP Exception
ISR_NOERRCODE 20    ; #VE  Virtualization Exception
ISR_ERRCODE   21    ; #CP  Control Protection
ISR_NOERRCODE 22
ISR_NOERRCODE 23
ISR_NOERRCODE 24
ISR_NOERRCODE 25
ISR_NOERRCODE 26
ISR_NOERRCODE 27
ISR_NOERRCODE 28    ; #HV  Hypervisor Injection
ISR_ERRCODE   29    ; #VC  VMM Communication
ISR_ERRCODE   30    ; #SX  Security Exception
ISR_NOERRCODE 31    ;      Reserved

; ---------------------------------------------------------------
; Stack layout when we arrive here (bottom of frame = low addr):
;   [esp+ 0]  int_num    (pushed by our stub)
;   [esp+ 4]  err_code   (CPU-pushed or dummy 0)
;   [esp+ 8]  EIP        (CPU-saved return address)
;   [esp+12]  CS         (CPU-saved)
;   [esp+16]  EFLAGS     (CPU-saved)
; After pushad (8 regs × 4 = 32 bytes):
;   [esp+32]  int_num
;   [esp+36]  err_code
; ---------------------------------------------------------------
common_isr_stub:
    pushad              ; save all general-purpose registers
    cld                 ; ABI: direction flag must be 0

    mov eax, [esp + 32] ; int_num
    mov ebx, [esp + 36] ; err_code
    push ebx            ; arg2 (cdecl: rightmost arg pushed first)
    push eax            ; arg1
    call exception_handler

    ; exception_handler never returns, but halt just in case
.hang:
    cli
    hlt
    jmp .hang

; Load the IDT register from the pointer passed as argument
idt_load:
    mov eax, [esp + 4]
    lidt [eax]
    ret

; IRQ0 handler — PIT timer tick
irq0:
    pushad
    cld
    call timer_tick
    mov al, 0x20
    out 0x20, al        ; EOI to master PIC
    popad
    iret

; IRQ1 handler — PS/2 keyboard
irq1:
    pushad              ; save all general-purpose registers
    cld                 ; System V ABI: DF must be 0 on function entry

    call keyboard_handler

    ; Send End-Of-Interrupt (EOI) to the master PIC (port 0x20)
    mov al, 0x20
    out 0x20, al

    popad               ; restore registers
    iret                ; return from interrupt

; Default handler for every other IRQ (spurious, timer, etc.)
; Sends EOI to slave then master PIC and returns silently.
global irq_default
irq_default:
    pushad
    mov al, 0x20
    out 0xA0, al        ; EOI to slave PIC  (in case it came from IRQ8-15)
    out 0x20, al        ; EOI to master PIC
    popad
    iret

; IRQ12 handler — PS/2 mouse (slave PIC IRQ4)
irq12:
    pushad
    cld
    call mouse_handler
    mov al, 0x20
    out 0xA0, al        ; EOI to slave PIC first
    out 0x20, al        ; then EOI to master PIC
    popad
    iret
