// ------------------------------------------------------------------------------------------------
// kernel.c
// ------------------------------------------------------------------------------------------------

#include "console.h"
#include "idt.h"
#include "keyboard.h"
#include "pic.h"
#include "vga.h"
#include "vm.h"

// ------------------------------------------------------------------------------------------------
int kmain()
{
    vga_text_init();
    console_init();
    console_print("Welcome!\n");

    idt_init();
    vm_init();
    pic_init();
    keyboard_init();

    for (;;) {}
    return 0;
}
