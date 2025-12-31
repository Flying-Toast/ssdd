FONT = /usr/share/fonts/jetbrains-mono/JetBrainsMonoNL-ExtraBoldItalic.ttf

main.elf: main.c font.h
	avr-gcc -Wall -Wextra -mmcu=attiny861 -o main.elf -O3 main.c

flash: main.elf
	avrdude -p t861 -c usbasp -U flash:w:main.elf:e

bakefont: bakefont.c
	cc -Wall -Wextra -Wpedantic -std=c99 \
		`pkg-config --cflags freetype2` \
		-o bakefont bakefont.c \
		`pkg-config --libs freetype2`

font.h: bakefont $(FONT)
	./bakefont $(FONT) >font.h

clean:
	rm -f *.elf font.h bakefont
