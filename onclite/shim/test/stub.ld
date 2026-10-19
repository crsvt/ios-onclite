ENTRY(_stub_entry)
SECTIONS
{
	. = 0;
	.text : { *(.text.head) *(.text*) }
	.rodata : { *(.rodata*) }
	.data : { *(.data*) }
	.bss : { *(.bss*) *(COMMON) }
	. = ALIGN(16);
	. += 0x4000;
	_stack_top = .;
	/DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
