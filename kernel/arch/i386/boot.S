.set ALIGN, 1<<0
.set MEMINFO, 1<<1
.set VIDEO, 1<<2
.set FLAGS, ALIGN | MEMINFO | VIDEO
.set MAGIC, 0x1BADB002
.set CHECKSUM, -(MAGIC + FLAGS)
.section .multiboot.data, "aw"
.align 4
.long MAGIC
.long FLAGS
.long CHECKSUM
.long 0
.long 0
.long 0
.long 0
.long 0
.long 0
.long 1024
.long 768
.long 32

multiboot_magic_low: .long 0
multiboot_ptr_low: .long 0

.section .bootstrap_stack, "aw", @nobits
stack_bottom:
.skip 4096
stack_top:

.section .bss
.align 4096
.global boot_page_directory
boot_page_directory:
.skip 4096
.global boot_page_table1
boot_page_table1:
.skip 4096
.global boot_page_table2
boot_page_table2:
.skip 4096
.global boot_page_table3
boot_page_table3:
.skip 4096
.global boot_page_table4
boot_page_table4:
.skip 4096

.align 32
.global boot_pdpt
boot_pdpt:
.skip 32

.align 4096
.global boot_pd_low
boot_pd_low:
.skip 4096
.global boot_pd_high
boot_pd_high:
.skip 4096

.align 4096
.global boot_pt_0
boot_pt_0:
.skip 4096
.global boot_pt_1
boot_pt_1:
.skip 4096
.global boot_pt_2
boot_pt_2:
.skip 4096
.global boot_pt_3
boot_pt_3:
.skip 4096

.global multiboot_magic
multiboot_magic: .skip 4
.global multiboot_ptr
multiboot_ptr: .skip 4

.global pae_enabled
pae_enabled: .skip 4

.section .multiboot.text, "a"
.global _start
_start:
    movl %eax, multiboot_magic_low
    movl %ebx, multiboot_ptr_low

    movl $1, %eax
    cpuid
    testl $(1 << 6), %edx
    jnz setup_pae
    jmp setup_legacy

setup_pae:
    movl $1, (pae_enabled - 0xC0000000)

    movl $(boot_pt_0 - 0xC0000000), %edi
    movl $0x00000000, %esi
    movl $512, %ecx
pae_fill_pt0:
    movl %esi, %eax
    orl $0x003, %eax
    movl %eax, (%edi)
    movl $0, 4(%edi)
    addl $4096, %esi
    addl $8, %edi
    loop pae_fill_pt0

    movl $(boot_pt_1 - 0xC0000000), %edi
    movl $0x00200000, %esi
    movl $512, %ecx
pae_fill_pt1:
    movl %esi, %eax
    orl $0x003, %eax
    movl %eax, (%edi)
    movl $0, 4(%edi)
    addl $4096, %esi
    addl $8, %edi
    loop pae_fill_pt1

    movl $(boot_pt_2 - 0xC0000000), %edi
    movl $0x00400000, %esi
    movl $512, %ecx
pae_fill_pt2:
    movl %esi, %eax
    orl $0x003, %eax
    movl %eax, (%edi)
    movl $0, 4(%edi)
    addl $4096, %esi
    addl $8, %edi
    loop pae_fill_pt2

    movl $(boot_pt_3 - 0xC0000000), %edi
    movl $0x00600000, %esi
    movl $512, %ecx
pae_fill_pt3:
    movl %esi, %eax
    orl $0x003, %eax
    movl %eax, (%edi)
    movl $0, 4(%edi)
    addl $4096, %esi
    addl $8, %edi
    loop pae_fill_pt3

    movl $(boot_pd_low - 0xC0000000), %edi
    xorl %eax, %eax
    movl $512, %ecx
pae_clear_pd_low:
    movl %eax, (%edi)
    movl %eax, 4(%edi)
    addl $8, %edi
    loop pae_clear_pd_low

    movl $(boot_pd_high - 0xC0000000), %edi
    xorl %eax, %eax
    movl $512, %ecx
pae_clear_pd_high:
    movl %eax, (%edi)
    movl %eax, 4(%edi)
    addl $8, %edi
    loop pae_clear_pd_high

    movl $(boot_pt_0 - 0xC0000000 + 0x003), %eax
    movl %eax, (boot_pd_low - 0xC0000000 + 0*8)
    movl $0, (boot_pd_low - 0xC0000000 + 0*8 + 4)
    movl $(boot_pt_1 - 0xC0000000 + 0x003), %eax
    movl %eax, (boot_pd_low - 0xC0000000 + 1*8)
    movl $0, (boot_pd_low - 0xC0000000 + 1*8 + 4)
    movl $(boot_pt_2 - 0xC0000000 + 0x003), %eax
    movl %eax, (boot_pd_low - 0xC0000000 + 2*8)
    movl $0, (boot_pd_low - 0xC0000000 + 2*8 + 4)
    movl $(boot_pt_3 - 0xC0000000 + 0x003), %eax
    movl %eax, (boot_pd_low - 0xC0000000 + 3*8)
    movl $0, (boot_pd_low - 0xC0000000 + 3*8 + 4)

    movl $(boot_pt_0 - 0xC0000000 + 0x003), %eax
    movl %eax, (boot_pd_high - 0xC0000000 + 0*8)
    movl $0, (boot_pd_high - 0xC0000000 + 0*8 + 4)
    movl $(boot_pt_1 - 0xC0000000 + 0x003), %eax
    movl %eax, (boot_pd_high - 0xC0000000 + 1*8)
    movl $0, (boot_pd_high - 0xC0000000 + 1*8 + 4)
    movl $(boot_pt_2 - 0xC0000000 + 0x003), %eax
    movl %eax, (boot_pd_high - 0xC0000000 + 2*8)
    movl $0, (boot_pd_high - 0xC0000000 + 2*8 + 4)
    movl $(boot_pt_3 - 0xC0000000 + 0x003), %eax
    movl %eax, (boot_pd_high - 0xC0000000 + 3*8)
    movl $0, (boot_pd_high - 0xC0000000 + 3*8 + 4)

    movl $(boot_pd_low - 0xC0000000 + 0x001), %eax
    movl %eax, (boot_pdpt - 0xC0000000 + 0*8)
    movl $0, (boot_pdpt - 0xC0000000 + 0*8 + 4)
    movl $0, (boot_pdpt - 0xC0000000 + 1*8)
    movl $0, (boot_pdpt - 0xC0000000 + 1*8 + 4)
    movl $0, (boot_pdpt - 0xC0000000 + 2*8)
    movl $0, (boot_pdpt - 0xC0000000 + 2*8 + 4)
    movl $(boot_pd_high - 0xC0000000 + 0x001), %eax
    movl %eax, (boot_pdpt - 0xC0000000 + 3*8)
    movl $0, (boot_pdpt - 0xC0000000 + 3*8 + 4)

    movl $(boot_pdpt - 0xC0000000), %eax
    movl %eax, %cr3

    movl %cr4, %eax
    orl $(1 << 5), %eax
    movl %eax, %cr4

    movl %cr0, %eax
    orl $0x80010000, %eax
    movl %eax, %cr0

    lea pae_higher, %ecx
    jmp *%ecx

setup_legacy:
    movl $0, (pae_enabled - 0xC0000000)

    movl $(boot_page_table1 - 0xC0000000), %edi
    movl $0x00000000, %esi
    movl $1024, %ecx
1:
    movl %esi, %edx
    orl $0x003, %edx
    movl %edx, (%edi)
    addl $4096, %esi
    addl $4, %edi
    loop 1b

    movl $(boot_page_table2 - 0xC0000000), %edi
    movl $0x00400000, %esi
    movl $1024, %ecx
2:
    movl %esi, %edx
    orl $0x003, %edx
    movl %edx, (%edi)
    addl $4096, %esi
    addl $4, %edi
    loop 2b

    movl $(boot_page_table3 - 0xC0000000), %edi
    movl $0x00800000, %esi
    movl $1024, %ecx
3:
    movl %esi, %edx
    orl $0x003, %edx
    movl %edx, (%edi)
    addl $4096, %esi
    addl $4, %edi
    loop 3b

    movl $(boot_page_table4 - 0xC0000000), %edi
    movl $0x00C00000, %esi
    movl $1024, %ecx
4:
    movl %esi, %edx
    orl $0x003, %edx
    movl %edx, (%edi)
    addl $4096, %esi
    addl $4, %edi
    loop 4b

    movl $(boot_page_table1 - 0xC0000000 + 0x003), %eax
    movl %eax, boot_page_directory - 0xC0000000 + 0*4
    movl %eax, boot_page_directory - 0xC0000000 + 768*4

    movl $(boot_page_table2 - 0xC0000000 + 0x003), %eax
    movl %eax, boot_page_directory - 0xC0000000 + 1*4
    movl %eax, boot_page_directory - 0xC0000000 + 769*4

    movl $(boot_page_table3 - 0xC0000000 + 0x003), %eax
    movl %eax, boot_page_directory - 0xC0000000 + 2*4
    movl %eax, boot_page_directory - 0xC0000000 + 770*4

    movl $(boot_page_table4 - 0xC0000000 + 0x003), %eax
    movl %eax, boot_page_directory - 0xC0000000 + 3*4
    movl %eax, boot_page_directory - 0xC0000000 + 771*4
   
    movl $(boot_page_directory - 0xC0000000 + 0x003), %eax
    movl %eax, boot_page_directory - 0xC0000000 + 1023*4
    movl $(boot_page_directory - 0xC0000000), %ecx
    movl %ecx, %cr3
   
    movl %cr0, %ecx
    orl $0x80010000, %ecx
    movl %ecx, %cr0
   
    lea legacy_higher, %ecx
    jmp *%ecx

.section .text

pae_higher:
    movl multiboot_magic_low, %eax
    movl %eax, multiboot_magic
    movl multiboot_ptr_low, %eax
    movl %eax, multiboot_ptr

    movl %cr3, %ecx
    movl %ecx, %cr3

    mov $stack_top, %esp
    call kernel_main
    cli
pae_hang:
    hlt
    jmp pae_hang

legacy_higher:
    movl multiboot_magic_low, %eax
    movl %eax, multiboot_magic
    movl multiboot_ptr_low, %eax
    movl %eax, multiboot_ptr
    
    movl %cr3, %ecx
    movl %ecx, %cr3
   
    mov $stack_top, %esp
    call kernel_main
cli
hang:
    hlt
    jmp hang