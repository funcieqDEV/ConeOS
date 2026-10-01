ENTRY(kmain)

PHDRS
{
    text PT_LOAD FLAGS(5);
    rodata PT_LOAD FLAGS(4);
    data PT_LOAD FLAGS(6);
}

SECTIONS
{
    . = 0xffffffff80100000;
    __kernel_text_start = .;
    .text : { *(.text .text.*) } :text
    . = ALIGN(4096);
    __kernel_text_end = .;
    __kernel_rodata_start = .;
    .rodata : {
        *(.rodata .rodata.*)
        *(.eh_frame .eh_frame.*)
    } :rodata
    . = ALIGN(4096);
    __kernel_rodata_end = .;
    .data : {
        *(.data .data.*)
        KEEP(*(.limine_requests_start))
        KEEP(*(.limine_requests .limine_reqs*))
        KEEP(*(.limine_requests_end))
    } :data
    . = ALIGN(4096);
    .bss : { *(.bss .bss.*) *(COMMON) } :data
    /DISCARD/ : { *(.note .note.*) *(.comment) }
}
