#pragma once

/* Drives the SC-55mk1 gate-array "LCD ready" interrupt, which the mk1 firmware
 * waits on.  Called once per emulated instruction, mk1 romsets only. */
void LcdStub_Tick(void);
