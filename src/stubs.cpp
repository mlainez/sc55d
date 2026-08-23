/*
 * No-op replacements for the frontend pieces sc55d leaves out of the build:
 * the SDL LCD panel (lcd.cpp) and the RtMidi input (midi_rtmidi.cpp).  The core
 * calls into both, and mcu.cpp's unused SDL main() names them too, so they have
 * to link -- but sc55d has no display, and its MIDI input is ALSA sequencer
 * based (see midi_in.cpp).
 *
 * One piece is not a no-op.  On the SC-55mk1 the gate array raises an interrupt
 * a while after each LCD write, and mcu.cpp arms that delay next to its
 * LCD_Write() calls but pulses the line from its own (SDL) run loop.  We rebuild
 * that here: LCD_Write() arms the counter, LcdStub_Tick() pulses the line.
 */
#include "stubs.h"

#include <stdint.h>
#include <string>

#include "lcd.h"
#include "mcu.h"
#include "midi.h"

int lcd_width = 741;
int lcd_height = 268;

uint32_t lcd_col1 = 0x000000;
uint32_t lcd_col2 = 0x0050c8;

namespace {

/* Same delay mcu.cpp uses for its own copy of this counter. */
const int kLcdIntDelay = 500;

int lcd_int_counter = 0;

} // namespace

void LcdStub_Tick(void)
{
    if (!lcd_int_counter)
        return;
    if (--lcd_int_counter == 0)
    {
        MCU_GA_SetGAInt(1, 0);
        MCU_GA_SetGAInt(1, 1);
    }
}

void LCD_SetBackPath(const std::string & /*path*/)
{
}

void LCD_Init(void)
{
}

void LCD_UnInit(void)
{
}

void LCD_Write(uint32_t /*address*/, uint8_t /*data*/)
{
    if (mcu_mk1)
        lcd_int_counter = kLcdIntDelay;
}

void LCD_Enable(uint32_t /*enable*/)
{
}

bool LCD_QuitRequested(void)
{
    return false;
}

void LCD_Sync(void)
{
}

void LCD_Update(void)
{
}

int MIDI_Init(int /*port*/)
{
    return 1;
}

void MIDI_Quit(void)
{
}
