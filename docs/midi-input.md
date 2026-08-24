# Getting MIDI into sc55d

sc55d listens on the ALSA sequencer, so anything that can send MIDI on Linux can
play it: a MIDI file player, a DAW, a USB keyboard, or a real MIDI DIN socket
wired to the Pi's serial port.

## Routing with aconnect

sc55d registers an ALSA sequencer client called `sc55d` with one writable port,
so anything can be routed to it with `aconnect`:

```bash
$ aconnect -l
client 14: 'Midi Through' [type=kernel]
    0 'Midi Through Port-0'
client 128: 'sc55d' [type=user]
    0 'midi in'
client 129: 'ttymidi' [type=user]
    0 'MIDI in'
    1 'MIDI out'
$ aconnect 129:0 128:0
```

`aconnect` takes client names too, which survive the numbers moving around:

```bash
aconnect 'ttymidi':0 'sc55d':0
```

Check it took with `aconnect -l` — the sc55d port should now list
`Connecting From: 129:0`.

## A real MIDI DIN socket: serial MIDI on ttyAMA0 at 31250 baud

A MIDI DIN input wired to the Pi's UART (through the usual 6N138-style
opto-isolator) needs three things: the UART freed from the console, the port
running at MIDI baud, and something bridging the tty to the ALSA sequencer.

1. Free `ttyAMA0`. In `/boot/firmware/cmdline.txt` remove
   `console=serial0,115200`, and in `/boot/firmware/config.txt` add:

   ```
   enable_uart=1
   dtoverlay=disable-bt
   dtoverlay=midi-uart0
   ```

   `disable-bt` moves the PL011 off Bluetooth and onto the GPIO header pins.
   `midi-uart0` retunes the UART base clock so that asking for 38400 baud puts
   exactly 31250 baud on the wire — the Pi cannot divide down to 31250 from the
   stock clock, which is why the overlay exists. Reboot afterwards.

2. Bridge the tty to the sequencer. [ttymidi](https://github.com/cjbarnes18/ttymidi)
   is the usual choice and creates an ALSA sequencer client of its own:

   ```bash
   ttymidi -s /dev/ttyAMA0 -b 38400 &
   ```

   38400 here is what you ask the driver for; the overlay makes it 31250 on the
   wire. A bridge that can set a non-standard baud rate directly (`BOTHER`
   termios) can skip the overlay and use `-b 31250`.

3. Connect it, as above:

   ```bash
   aconnect 'ttymidi':0 'sc55d':0
   ```


