.POSIX:

main.hex: main.asm
	avra -l main.list -m main.map main.asm

flash: main.hex
	avrdude -p t861 -c usbasp -U flash:w:main.hex:i

clean:
	rm -f *.hex *.obj *.list *.map
