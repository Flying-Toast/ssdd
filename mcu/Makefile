.POSIX:

main.elf: main.c
	avr-gcc -Wall -Wextra -mmcu=attiny861 -o main.elf -O3 main.c

flash: main.elf
	avrdude -p t861 -c usbasp -U flash:w:main.elf:e

clean:
	rm -f *.elf
