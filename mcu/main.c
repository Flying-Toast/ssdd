#include <avr/interrupt.h>
#include <avr/io.h>
#include <avr/sleep.h>
#include <stdint.h>
#define F_CPU 8000000UL
#include <util/delay.h>

/* address is in the high 7 bits */
#define DS1307_ADDR 0b11010000

static uint8_t displaybuf[144];
static uint8_t hrs;
static uint8_t mins;

/* To conserve GPIOs, the TM1640 data lines are all wired together to PB6
 * while their clocks each get a distinct GPIO. The `module` parameter of the
 * tm1640_* functions specifies which clock to use for the operation:
 * `module == 0` will act on PB3, and any nonzero `module` value is treated as
 * a bitflag(s) of clock lines in port A (the 8 bits of port A are the clock
 * lines of module 1-8; module 9 is on PB3). */

static void tm1640_dat1(void) {
	PORTB |= (1<<PB6);
}

static void tm1640_dat0(void) {
	PORTB &= ~(1<<PB6);
}

static void tm1640_clk1(uint8_t module) {
	if (module == 0) {
		PORTB |= 1<<PB3;
	} else {
		PORTA |= module;
	}
}

static void tm1640_clk0(uint8_t module) {
	if (module == 0) {
		PORTB &= ~(1<<PB3);
	} else {
		PORTA &= ~module;
	}
}

static void tm1640_start(uint8_t module) {
	tm1640_dat0();
	tm1640_clk0(module);
}

static void tm1640_stop(uint8_t module) {
	tm1640_dat0();
	tm1640_clk1(module);
	tm1640_dat1();
}

static void tm1640_bit(uint8_t module, _Bool b) {
	if (b) {
		tm1640_dat1();
	} else {
		tm1640_dat0();
	}
	/* The Chinglish TM1640 datasheet is confusing about timing here,
	 * but anecdotally this works at our 8MHz ATtiny clock speed. */
	tm1640_clk1(module);
	tm1640_clk0(module);
}

static void tm1640_byte(uint8_t module, uint8_t b) {
	for (uint8_t i = 0; i < 8; i++) {
		if (b & 1) {
			tm1640_dat1();
		} else {
			tm1640_dat0();
		}
		tm1640_clk1(module);
		tm1640_clk0(module);
		b >>= 1;
	}
}

static void sendrow(uint8_t module, uint8_t *row) {
	/* TM1640 is designed for driving common cathode 7-segment displays,
	 * so we need to do some swizzling to adapt to our common anode setup.
	 * See schematic. */
	for (uint8_t i = 1; i != 0; i <<= 1) {
		tm1640_bit(module, row[0] & i);
		tm1640_bit(module, row[1] & i);
		tm1640_bit(module, row[2] & i);
		tm1640_bit(module, row[3] & i);
		tm1640_bit(module, row[4] & i);
		tm1640_bit(module, row[5] & i);
		tm1640_bit(module, row[6] & i);
		tm1640_bit(module, row[7] & i);
	}
}

static void show_module(uint8_t module, uint8_t *row1, uint8_t *row2) {
	tm1640_start(module);
	tm1640_byte(module, 0x40); /* autoincrement */
	tm1640_stop(module);

	tm1640_start(module);
	tm1640_byte(module, 0xC0); /* address = 0 */
	sendrow(module, row1);
	sendrow(module, row2);
	tm1640_stop(module);
}

static void show(void) {
/* offs: index of first byte of row 1 */
#define X(module, offs) \
	show_module(module, displaybuf + (offs), displaybuf + (offs) + 24)

	X(1<<0,  0 +  0);
	X(1<<1,  0 +  8);
	X(1<<2,  0 + 16);

	X(1<<3, 48 +  0);
	X(1<<4, 48 +  8);
	X(1<<5, 48 + 16);

	X(1<<6, 96 +  0);
	X(1<<7, 96 +  8);
	X( 0  , 96 + 16);

#undef X
}

static void displays_on(void) {
	for (uint8_t module = 1;;) {
		tm1640_start(module);
		tm1640_byte(module, 0x88); /* display on */
		tm1640_stop(module);

		if (module == 0)
			break;
		module <<= 1;
	}
}

/* Tristate RTC_SDA (pulled-up externally). */
static void rtc_datz(void) {
	DDRB &= ~(1<<PB5);
}

static void rtc_dat0(void) {
	/* we always keep PORTB[5] = 0, so this sets the output low */
	DDRB |= 1<<PB5;
}

static void rtc_clk1(void) {
	PORTB |= 1<<PB4;
}

static void rtc_clk0(void) {
	PORTB &= ~(1<<PB4);
}

/* names from timing diagram in DS1307 datasheet */
#define rtc_delay_high() _delay_us(4)
#define rtc_delay_low() _delay_us(5)
#define rtc_delay_su_dat() asm volatile ("nop\nnop\nnop") /* ~375ns on 8MHz clock */
#define rtc_delay_hd_sta() _delay_us(4)
#define rtc_delay_su_sto() _delay_us(5)

////////////////////////////////////////////////////////////////////////////////////
static void putch(char ch) {static int cursor = 0;uint8_t byte;switch (ch) { case '0':
byte = 0b01011111; break; case '1': byte = 0b00000110; break; case '2':
byte = 0b00111011; break; case '3': byte = 0b00101111; break; case '4':
byte = 0b01100110; break; case '5': byte = 0b01101101; break; case '6':
byte = 0b01111101; break; case '7': byte = 0b00000111; break; case '8':
byte = 0b01111111; break; case '9': byte = 0b01101111; break; case '\n':
cursor += 24 - (cursor % 24); return; } displaybuf[cursor++] = byte; show(); }
////////////////////////////////////////////////////////////////////////////////////

static void rtc_wbyte(uint8_t b) {
	for (uint8_t i = 1<<7; i; i >>= 1) {
		if (b & i) {
			rtc_datz();
		} else {
			rtc_dat0();
		}
		rtc_delay_su_dat();
		rtc_clk1();
		rtc_delay_high();
		rtc_clk0();
	}
	rtc_delay_su_dat();
	rtc_clk1();
	rtc_delay_high();
	/* ignore ACK */
	rtc_clk0();
}

#define NACK 1
#define ACK 0
static uint8_t rtc_rbyte(_Bool nack) {
	uint8_t b = 0;
	rtc_datz();
	for (uint8_t i = 0; i < 8; i++) {
		rtc_clk1();
		rtc_delay_high();
		if (PINB & (1<<PB5))
			b |= 1;
		rtc_clk0();
		if (i != 7)
			b <<= 1;
	}
	if (nack) {
		rtc_datz();
	} else {
		rtc_dat0();
	}
	rtc_delay_su_dat();
	rtc_clk1();
	rtc_delay_high();
	rtc_clk0();
	return b;
}

static void rtc_start(void) {
	rtc_dat0();
	rtc_delay_hd_sta();
	rtc_clk0();
	rtc_delay_low();
}

static void rtc_stop(void) {
	rtc_dat0();
	rtc_clk1();
	rtc_delay_su_sto();
	rtc_datz();
}

////////////////////////////////
static void printnum(uint8_t n) {
	if (n == 0) {
		putch('0');
	} else {
		while (n) {
			putch('0' + (n%10));
			n /= 10;
		}
	}
	putch('\n');
}
////////////////////////////////

static void rtc_gettime(void) {
	rtc_start();
	rtc_wbyte(DS1307_ADDR | 0);
	rtc_wbyte(0); /* register address */
	rtc_start();
	rtc_wbyte(DS1307_ADDR | 1);
	////////////////////////////////
	uint8_t foo = rtc_rbyte(NACK);
	rtc_stop();
	printnum(foo);
	show();
	////////////////////////////////
}

int main(void) {
	DDRA  = 0xFF;
	PORTA = 0xFF;
	DDRB  = 0b01011000;
	PORTB = 0b01011111;

	MCUCR |= 1 << SE;
	ACSRA |= 1 << ACD;

	show();
	displays_on();

	for (;;)
		rtc_gettime();

	sei();
	for (;;)
		sleep_cpu();
}
