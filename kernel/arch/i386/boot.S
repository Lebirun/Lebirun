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
.skip 16384
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
.global multiboot_magic
multiboot_magic: .skip 4
.global multiboot_ptr
multiboot_ptr: .skip 4

.section .multiboot.text, "a"
.global _start
_start:
    movl %eax, multiboot_magic_low
    movl %ebx, multiboot_ptr_low

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
   
    lea 5f, %ecx
    jmp *%ecx
.section .text
5:
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