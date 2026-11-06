# Firmware for the GenMDM interface's Teensy 2.0: make -> midi32x.hex
# Needs avr-gcc and avr-libc (apt: gcc-avr avr-libc).
MCU   := atmega32u4
CFLAGS := -mmcu=$(MCU) -DF_CPU=16000000L -DUSB_MIDI -Os -std=gnu99 -Wall -Wextra \
          -ffunction-sections -fdata-sections -Ipjrc
LDFLAGS := -mmcu=$(MCU) -Wl,--gc-sections

midi32x.hex: midi32x.elf
	avr-objcopy -O ihex -R .eeprom $< $@
	avr-size --mcu=$(MCU) -C $< | grep -E "Program|Data"
midi32x.elf: midi32x.c pjrc/usb.c pjrc/usb_private.h pjrc/usb_common.h
	avr-gcc $(CFLAGS) $(LDFLAGS) -o $@ midi32x.c pjrc/usb.c
clean:
	rm -f midi32x.elf midi32x.hex
