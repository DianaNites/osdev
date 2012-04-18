// ------------------------------------------------------------------------------------------------
// keyboard.c
// ------------------------------------------------------------------------------------------------

#include "keyboard.h"
#include "idt.h"
#include "io.h"

// ------------------------------------------------------------------------------------------------
extern void keyboard_interrupt();

// ------------------------------------------------------------------------------------------------
void keyboard_init()
{
    idt_set_handler(0x21, INTERRUPT_GATE, keyboard_interrupt);
    outb(0x0021, inb(0x0021) & ~0x2);
}
