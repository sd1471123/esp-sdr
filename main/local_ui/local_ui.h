#pragma once

/* Start the local LCD front panel. Must be called after NVS is initialized
 * and after burst_serial_init() has reserved the transport pins. */
void local_ui_start(void);
