# Heltec LoRa modem

Turns a Heltec WiFi LoRa 32 V3 into the radio for the PC emulator:

    PC cybiko-emu --radio /dev/ttyUSB0  →  USB (CP2102)  →  Heltec SX1262  →  LoRa  →  T-Deck

The SX1262 driver is `../cybiko-c-emulator/src/radio/sx1262_phy.cpp`, the same one the T-Deck uses, so both ends share one profile: 915 MHz, SF7, 125 kHz, CR 4/5, sync word 0x1424. This build transmits at 0 dBm (`CYBIKO_LORA_POWER_DBM` in `platformio.ini`), enough for a desk. The serial protocol is in `../cybiko-c-emulator/src/radio/modem_proto.h`.

The modem does not decide what to send. Every packet comes from the PC.

## Flash

    pio run -t upload        # /dev/ttyUSB0

## Check it without CyOS

    cd ../cybiko-c-emulator && make build/modem_tool
    ./build/modem_tool /dev/ttyUSB0 --seconds 20             # listen, print status
    ./build/modem_tool /dev/ttyUSB0 --seconds 20 --ping 1000 # also send PING every second

A T-Deck running the emulator counts the pings on its serial log (`radio rx N ... msgs N`).

## Run the emulator on it

    cd ../cybiko-c-emulator && make build/cybiko-emu
    ./build/cybiko-emu ../roms/cyrom150.bin ../roms/cyos_v1508.bin --nvram pc.nvram --radio /dev/ttyUSB0

`--cyid` sets the PC's CyID (hex, default `4C000001`). The T-Deck takes its CyID from its MAC, so the two are always different.
