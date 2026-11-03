# Serial protocol

All burst firmware speaks the same line-based protocol over native USB or
UART0 (2 Mbaud, 8N1, no flow control). Send ASCII commands terminated by
`\n`; every command gets exactly one text reply line, and some replies are
followed by binary data. Errors start with `ERR`.

## Session

| Command | Reply | Notes |
| --- | --- | --- |
| `SYNC <nonce>` | `SYNC <nonce>` | Resynchronize after an interrupted transfer. |
| `INFO` | e.g. `S3SDR 6 burst 16380` | Family, protocol version, mode, maximum capture length. |
| `CAPS` | `CAPS <flags...>` | Optional features this firmware supports. |
| `VERSION?` | `VERSION {json}` | Git revision and build time, see [firmware-version.md](firmware-version.md). |
| `TRANSPORT?` | `TRANSPORT USB 0` or `TRANSPORT UART <baud>` | The interface this command arrived on. |
| `RELEASE` | `OK` | Give up the receiver lease. |

Only one client controls the radio at a time. The first command takes the
lease; a client on the other interface gets `ERR busy` until `RELEASE` or
five seconds of silence. Read each reply completely before sending the next
command.

Opening some ports resets the board (UART bridges with DTR/RTS wired to
EN/BOOT, the S2's USB-OTG port, the S31's USB Serial/JTAG). Clients should
send `SYNC` and discard everything up to the echoed nonce before talking to
the firmware.

## Receiver settings

| Command | Notes |
| --- | --- |
| `FREQ <MHz>` | Whole MHz, 100–6000 (`RANGE?`). Accepting a frequency does not guarantee PLL lock. |
| `LIMITS?` | `LIMITS {"gain":[min,max,step],"bandwidth":[min,max,step,default]\|null,"rates":[...],"bits":[...]}` |
| `GAIN HARDWARE` / `GAIN MANUAL <index>` / `GAIN?` | Hardware AGC is the default. Indices are PHY table entries, not dB. |
| `BANDWIDTH <MHz>` | Approximate analog bandwidth; `0` selects the widest setting. |
| `GPIO?` / `GPIO <pin> <Z\|0\|1>` | Spare GPIO outputs, e.g. for antenna switches. The status LED pin is not in the pool, and a pin a sounding buzzer drives rejects writes. See [rx-controls.md](rx-controls.md#gpio-outputs). |

Details on gain, bandwidth, calibration and extended tuning are in
[rx-controls.md](rx-controls.md).

## Status LED and buzzer

Board extras advertised as `LED` and `BEEP` in `CAPS`. The burst firmware of the
C5, C6 and C61 implements them; other targets do not. `LED` further needs
`CONFIG_ESP_SDR_LED_PIN` (27 by default on the C5, `-1` disables the feature and
drops `LED` from `CAPS`).

| Command | Reply | Notes |
| --- | --- | --- |
| `LED <r> <g> <b>` | `OK LED` | 0–255 per channel, on an addressable (WS2812-class) RGB LED. |
| `LED 0` | `OK LED` | Blank the LED. |
| `LED?` | `LED <r> <g> <b>` | Last value written, `LED 0 0 0` after `LED 0`. |
| `BEEP <pin> <freq> <duty>` | `OK BEEP` | `pin` 0–48, `freq` 100–20000 Hz, `duty` 0–100 as a percentage. |
| `BEEP 0` | `OK BEEP 0` | Stop the tone and release the pin. |
| `BEEP?` | `BEEP <pin> <freq> <duty>`, or `BEEP 0` when silent | The tone currently sounding. |

The LED is driven by an RMT channel that produces the WS2812 pulse timing, so a
colour change costs no CPU time and does not disturb captures; while the pin is
configured for the LED it is removed from the `GPIO` pool. The buzzer loops one
two-phase RMT symbol in hardware, so a steady tone also costs no CPU time:
`duty` sets the loudness, `freq` is the tone for a passive buzzer (2.7 kHz is
typical) or a carrier above the audible range (20 kHz) for an active one. While a
tone sounds, `GPIO` writes to that pin return `ERR gpio_busy`.

Both settings survive captures and retuning and are not saved to flash. A tone
stops on `BEEP 0`, when another pin is selected, on `RELEASE`, and when the lease
lapses (five seconds without a command), so a client that disappears cannot leave
the buzzer running. The LED keeps its last colour across client changes and is
blank after a reboot. Arguments are matched exactly, so `BEEP 0 0 0` is
`ERR beep_args` rather than "silent" — use `BEEP 0`. Malformed arguments return
`ERR led_args` / `ERR beep_args`, a driver failure `ERR led_io` / `ERR beep_io`.

## Captures

`CAP16 <samples> <rate>` and `CAP20 <samples> <rate>` return samples as
`(I, -Q)` pairs. Both components are signed two's complement integers.

**Q is negated in the transmitted samples.** Negate the second component
when decoding to obtain standard `(I, Q)` samples.

| Format | Bits per sample | I | -Q |
| --- | --- | --- | --- |
| `CAP16` | 16 | First byte, signed 8-bit | Second byte, signed 8-bit |
| `CAP20` | 20 | Bits 0–9, signed 10-bit | Bits 10–19, signed 10-bit |

`CAP20` samples are packed consecutively, LSB first, with no padding between
samples. The reply is

```text
DATA <samples> <crc32-hex> <capture-microseconds>
```

followed by exactly `ceil(samples * bits * 2 / 8)` bytes. Check the CRC32
(zlib polynomial) before using the data. Only use rates listed in `LIMITS?`:

| Rate code | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| MS/s | 80 | 40 | 20 | 10 | 8 | 4 | 16 | 32 | 10.667 | 6.4 |

A capture is a single burst; there are gaps between captures.

The [S3 continuous stream](iq-stream.md) also sends `(I, -Q)` pairs.
The separate [S31 streaming firmware](s31-streaming.md#iq-packets) sends
`(I, Q)` pairs.

## Streaming modes

- On-chip spectra (`SPEC`, all chips): [spectrum.md](spectrum.md)
- Continuous decimated I/Q on the S3 (`IQS`): [iq-stream.md](iq-stream.md)
- Continuous I/Q over Ethernet/USB on the S31 (separate firmware): [s31-streaming.md](s31-streaming.md)
