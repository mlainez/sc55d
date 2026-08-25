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
client 16: 'SerialMIDI-0' [type=kernel,card=0]
    0 'Serial MIDI 0-0 '
client 128: 'sc55d' [type=user]
    0 'midi in'
$ aconnect 16:0 128:0
```

`aconnect` takes client names too, which survive the numbers moving around:

```bash
aconnect 'SerialMIDI-0':0 'sc55d':0
```

Check it took with `aconnect -l` — the sc55d port should now list
`Connecting From: 16:0`. `--client-name` renames sc55d's own client if you are
running more than one of them.

## USB MIDI

Nothing to configure. A class-compliant USB MIDI interface or keyboard appears
as its own sequencer client as soon as it is plugged in; route it with
`aconnect` exactly as above. This is the least troublesome input by a wide
margin — if you are only trying to establish that sc55d works at all, start
here.

## A real MIDI DIN socket: serial MIDI on the UART

A MIDI DIN input wired to the Pi's UART needs the UART freed from the console,
the port running at MIDI baud, and something presenting it to the ALSA
sequencer. The kernel will do the last two itself.

### Use the kernel's driver, not a userspace bridge

`snd-serial-generic` (`CONFIG_SND_SERIAL_GENERIC`, mainline since 6.0) binds to
a UART through serdev and exposes it as an ALSA rawmidi device. With
`snd-seq-midi` it appears on the sequencer with no daemon running at all.

This supersedes the older advice in this file, which was to install
[ttymidi][ttymidi] and run it at 38400 while a `midi-uart0` overlay retuned the
UART base clock so that 38400 landed on 31250 on the wire. That trick exists
because the PL011 cannot divide to 31250 from its stock clock. It is no longer
needed: bind the driver with `current-speed = <31250>` and the wire runs at MIDI
baud directly, with no daemon to supervise.

Add an overlay attaching the driver to whichever UART you have wired:

```dts
/dts-v1/;
/plugin/;

/ {
    compatible = "brcm,bcm2835";

    fragment@0 {
        target = <&uart0>;
        __overlay__ {
            status = "okay";
            midi {
                compatible = "serial-midi";
                current-speed = <31250>;
            };
        };
    };
};
```

Build it with `dtc -@ -I dts -O dtb -o midi-uart0-custom.dtbo <file>.dts`, put
it in `/boot/firmware/overlays/`, and in `/boot/firmware/config.txt`:

```
enable_uart=1
dtoverlay=disable-bt
dtoverlay=midi-uart0-custom
```

Remove `console=serial0,115200` from `/boot/firmware/cmdline.txt` so the console
stops holding the port, then reboot. `disable-bt` moves the PL011 off Bluetooth
and onto the GPIO header pins.

Verified this way on both a Pi 3 and a Pi 4 — the port appears as its own card
and sequencer client with no process bridging it:

```bash
$ cat /proc/asound/cards
 0 [SerialMIDI0    ]: SerialMIDI - SerialMIDI-0
                      Serial MIDI device at serial0
```

An unconnected input generates nothing: the RX pin idles high through its
pull-up, which is MIDI's idle state.

### The opto-isolator is not optional

MIDI is a 5 mA current loop, not a voltage, and a MIDI input needs an
optocoupler. This is not spec pedantry — every machine you plug in is separately
mains-earthed, and the opto is what stops ground loops between them. Wire a DIN
socket straight to a GPIO pin and you get hum you cannot remove, plus a 3.3 V
pin exposed to whatever the other end decides to do.

Prefer an **H11L1** to the commonly cited 6N138: it has a Schmitt trigger built
in, where a 6N138 needs a pull-up and is slower. Their pinouts differ, so check
the datasheet rather than assuming.

### If you would rather use ttymidi

It still works, together with the old clock trick:

```bash
ttymidi -s /dev/ttyAMA0 -b 38400 &        # 38400 requested, 31250 on the wire
aconnect 'ttymidi':0 'sc55d':0
```

but it is another process to supervise, and a stall in it is a stall in your
MIDI. The kernel driver has neither problem.

[ttymidi]: https://github.com/cjbarnes18/ttymidi
