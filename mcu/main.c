#include <avr/interrupt.h>
#include <avr/io.h>
#include <avr/sleep.h>
#include <stdint.h>
#define F_CPU 8000000UL
#include <util/delay.h>
#include "font.h"

/* address is in the high 7 bits */
#define DS1307_ADDR 0b11010000

static volatile enum : uint8_t {
	INTR_IGNORE = 0,
	INTR_SET_PRESSED,
	INTR_INC_PRESSED,
	INTR_DEC_PRESSED,
	INTR_GET_TIME,
} interrupt;
static enum : uint8_t {
	MODE_CLOCK = 0,
	MODE_SET_HRS,
	MODE_SET_MINS_TENS,
	MODE_SET_MINS_ONES,
} mode;
static uint8_t displaybuf[144];
/* Index 0 is BCD tens digit, index 1 is the ones digit. */
static uint8_t hours[2];
static uint8_t minutes[2];
/* Scratchpad for time currently being set by user. Hours aren't set using BCD
 * since there's only 12 to cycle through. */
static uint8_t setting_hours;
static uint8_t setting_minutes[2];
/* Bit patterns to display a digit on a single display, indexed by digit. */
static const uint8_t digits[] = {
	0x5F, 0x06, 0x3B, 0x2F, 0x66,
	0x6D, 0x7D, 0x07, 0x7F, 0x6F,
};
static _Bool clock_needs_render;

static void tm1640_dat1(void) {
	PORTB |= 1 << PB6;
}

static void tm1640_dat0(void) {
	PORTB &= ~(1 << PB6);
}

/* To conserve GPIOs, the TM1640 data lines are all wired together to PB6
 * while their clocks each get a distinct GPIO. The `module` parameter of the
 * tm1640_* functions specifies which clock line to use for the operation and
 * thus which TM1640 the data will go to: `module == 0` will use PB3 for clock,
 * and any nonzero `module` value is treated as a bitflag directly applied to
 * PORTA. (The 8 bits of port A are the clock lines of module 1-8. Module 9 is
 * on PB3). */

static void tm1640_clk1(uint8_t module) {
	if (module == 0) {
		PORTB |= 1 << PB3;
	} else {
		PORTA |= module;
	}
}

static void tm1640_clk0(uint8_t module) {
	if (module == 0) {
		PORTB &= ~(1 << PB3);
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

static void tm1640_tx_bit(uint8_t module, _Bool bit) {
	if (bit) {
		tm1640_dat1();
	} else {
		tm1640_dat0();
	}
	/* The Chinglish TM1640 datasheet is confusing about timing here,
	 * but anecdotally this works at our 8MHz ATtiny clock speed. */
	tm1640_clk1(module);
	tm1640_clk0(module);
}

static void tm1640_tx(uint8_t module, uint8_t byte) {
	for (uint8_t i = 0; i < 8; i++) {
		tm1640_tx_bit(module, byte & 1);
		byte >>= 1;
	}
}

static void tm1640_send_row(uint8_t module, const uint8_t *row) {
	/* TM1640 is designed for driving common cathode 7-segment displays,
	 * so we need to do some swizzling to adapt to our common anode setup.
	 * See schematic. */
	for (uint8_t i = 1; i != 0; i <<= 1) {
		tm1640_tx_bit(module, row[0] & i);
		tm1640_tx_bit(module, row[1] & i);
		tm1640_tx_bit(module, row[2] & i);
		tm1640_tx_bit(module, row[3] & i);
		tm1640_tx_bit(module, row[4] & i);
		tm1640_tx_bit(module, row[5] & i);
		tm1640_tx_bit(module, row[6] & i);
		tm1640_tx_bit(module, row[7] & i);
	}
}

static void tm1640_show_module(uint8_t module, const uint8_t *row1, const uint8_t *row2) {
	tm1640_start(module);
	tm1640_tx(module, 0x40); /* autoincrement */
	tm1640_stop(module);

	tm1640_start(module);
	tm1640_tx(module, 0xC0); /* address = 0 */
	tm1640_send_row(module, row1);
	tm1640_send_row(module, row2);
	tm1640_stop(module);
}

/* Present the state of displaybuf to the screen. */
static void show(void) {
/* offs: index of first byte of row 1 */
#define SM(module, offs) \
	tm1640_show_module(module, displaybuf + (offs), displaybuf + (offs) + DISPLAYS_X)

	SM(1 << 0,  0 +  0);
	SM(1 << 1,  0 +  8);
	SM(1 << 2,  0 + 16);

	SM(1 << 3, 48 +  0);
	SM(1 << 4, 48 +  8);
	SM(1 << 5, 48 + 16);

	SM(1 << 6, 96 +  0);
	SM(1 << 7, 96 +  8);
	SM( 0    , 96 + 16);

#undef SM
}

static void tm1640_all_on(void) {
	for (uint8_t module = 1;;) {
		tm1640_start(module);
		tm1640_tx(module, 0x88); /* display on */
		tm1640_stop(module);

		if (module == 0)
			break;
		module <<= 1;
	}
}

/* Tristate RTC_SDA (pulled-up externally). */
static void rtc_datz(void) {
	DDRB &= ~(1 << PB5);
}

static void rtc_dat0(void) {
	/* we always keep PORTB[5] = 0, so this sets the output low */
	DDRB |= 1 << PB5;
}

static void rtc_clk1(void) {
	PORTB |= 1 << PB4;
}

static void rtc_clk0(void) {
	PORTB &= ~(1 << PB4);
}

static void rtc_tx(uint8_t byte) {
	for (uint8_t i = 1 << 7; i; i >>= 1) {
		if (byte & i) {
			rtc_datz();
		} else {
			rtc_dat0();
		}
		rtc_clk1();
		rtc_clk0();
	}
	rtc_datz();
	rtc_clk1();
	/* ignore ACK */
	rtc_clk0();
}

enum acknack : uint8_t {
	ACK,
	NACK,
};
static uint8_t rtc_rx(enum acknack acknack) {
	uint8_t b = 0;
	rtc_datz();
	for (uint8_t i = 0; i < 8; i++) {
		rtc_clk1();
		if (PINB & (1 << PB5))
			b |= 1;
		rtc_clk0();
		if (i != 7)
			b <<= 1;
	}
	if (acknack == ACK) {
		rtc_dat0();
	} else {
		rtc_datz();
	}
	rtc_clk1();
	rtc_clk0();
	return b;
}

static void rtc_start(void) {
	rtc_clk1();
	rtc_dat0();
	rtc_clk0();
}

static void rtc_stop(void) {
	rtc_dat0();
	rtc_clk1();
	rtc_datz();
}

enum xfer_dir : uint8_t {
	WRITE = 0,
	READ = 1,
};
static void rtc_begin_xfer(enum xfer_dir dir, uint8_t reg_addr) {
	rtc_start();
	rtc_tx(DS1307_ADDR | WRITE);
	rtc_tx(reg_addr);
	if (dir == READ) {
		rtc_start();
		rtc_tx(DS1307_ADDR | READ);
	}
}

static void rtc_gettime(void) {
	rtc_begin_xfer(READ, 1);
	uint8_t reg1 = rtc_rx(ACK);
	uint8_t reg2 = rtc_rx(NACK);
	rtc_stop();

	uint8_t mins0 = reg1 >> 4;
	uint8_t mins1 = reg1 & 0xF;
	/* clear AM/PM and 12 hour flag */
	uint8_t hrs0 = (reg2 >> 4) & 1;
	uint8_t hrs1 = reg2 & 0xF;

	clock_needs_render = mins0 != minutes[0]
		|| mins1 != minutes[1]
		|| hrs0 != hours[0]
		|| hrs1 != hours[1];

	minutes[0] = mins0;
	minutes[1] = mins1;
	hours[0] = hrs0;
	hours[1] = hrs1;
}

static void rtc_settime(void) {
	/* 0x40 - 12-hour mode flag */
	uint8_t hours_reg = 0x40;
	if (setting_hours >= 10) {
		hours_reg |= 0x10 | (setting_hours - 10);
	} else {
		hours_reg |= setting_hours;
	}

	rtc_begin_xfer(WRITE, 0);
	/* clear seconds */
	rtc_tx(0);
	rtc_tx((setting_minutes[0] << 4) | setting_minutes[1]);
	rtc_tx(hours_reg);
	rtc_stop();
}

/* If RTC did a cold power-on (i.e. wasn't running off backup coin cell),
 * configure it to use 12-hour time and clear the clock halt (CH) bit. */
static void rtc_init(void) {
	rtc_begin_xfer(READ, 0);
	uint8_t reg0 = rtc_rx(NACK);
	if (reg0 & 0x80) {
		rtc_begin_xfer(WRITE, 0);
		/* seconds=0 (clear CH) */
		rtc_tx(0);
		/* minutes=0 */
		rtc_tx(0);
		/* hrs=12am, 12-hour mode */
		rtc_tx(0x52);
	}
	rtc_stop();

	rtc_gettime();
}

static void handle_set_pressed(void) {
	switch (mode) {
	case MODE_CLOCK:
		mode = MODE_SET_HRS;
		setting_minutes[0] = minutes[0];
		setting_minutes[1] = minutes[1];
		setting_hours = hours[0]*10 + hours[1];
		break;
	case MODE_SET_HRS:
		mode = MODE_SET_MINS_TENS;
		break;
	case MODE_SET_MINS_TENS:
		mode = MODE_SET_MINS_ONES;
		break;
	case MODE_SET_MINS_ONES:
		rtc_settime();
		rtc_gettime();
		mode = MODE_CLOCK;
		clock_needs_render = 1;
		break;
	}
}

static void handle_inc_pressed(void) {
	switch (mode) {
	case MODE_SET_HRS:
		if (setting_hours == 12) {
			setting_hours = 1;
		} else {
			setting_hours++;
		}
		break;
	case MODE_SET_MINS_TENS:
		if (setting_minutes[0] == 5) {
			setting_minutes[0] = 0;
		} else {
			setting_minutes[0]++;
		}
		break;
	case MODE_SET_MINS_ONES:
		if (setting_minutes[1] == 9) {
			setting_minutes[1] = 0;
		} else {
			setting_minutes[1]++;
		}
		break;
	default:
		break;
	}
}

static void handle_dec_pressed(void) {
	switch (mode) {
	case MODE_SET_HRS:
		if (setting_hours == 1) {
			setting_hours = 12;
		} else {
			setting_hours--;
		}
		break;
	case MODE_SET_MINS_TENS:
		if (setting_minutes[0] == 0) {
			setting_minutes[0] = 5;
		} else {
			setting_minutes[0]--;
		}
		break;
	case MODE_SET_MINS_ONES:
		if (setting_minutes[1] == 0) {
			setting_minutes[1] = 9;
		} else {
			setting_minutes[1]--;
		}
		break;
	default:
		break;
	}
}

static void clear_displaybuf(void) {
	for (uint8_t i = 0; i < sizeof(displaybuf); i++)
		displaybuf[i] = 0;
}

static void big_char(uint8_t chidx, uint8_t dx) {
	uint8_t cursor = dx;
	for (uint8_t y = 0; y < CHAR_DISPLAYS_Y; y++) {
		for (uint8_t x = 0; x < CHAR_DISPLAYS_X; x++) {
			displaybuf[cursor++] = pgm_read_byte(&font[chidx][y][x]);
		}
		cursor += DISPLAYS_X - CHAR_DISPLAYS_X;
	}
}

static void render_clock(void) {
	if (hours[0])
		big_char(hours[0], 0);
	big_char(hours[1], CHAR_DISPLAYS_X);

	/* colon */
	displaybuf[60] = 0x63;
	displaybuf[107] = 0x63;

	big_char(minutes[0], 2 * CHAR_DISPLAYS_X + COLON_DISPLAYS_X);
	big_char(minutes[1], 3 * CHAR_DISPLAYS_X + COLON_DISPLAYS_X);
}

static void render_setting_time(void) {
	displaybuf[56] = 0x6D; /* S */
	displaybuf[57] = 0x79; /* E */
	displaybuf[58] = 0x78; /* t */

	uint8_t hrs_ones;
	if (setting_hours < 10) {
		hrs_ones = setting_hours;
	} else {
		hrs_ones = setting_hours - 10;
		displaybuf[60] = digits[1];
	}

	/* use decimal point as hrs/mins separator */
	displaybuf[61] = digits[hrs_ones] | 0x80;
	displaybuf[62] = digits[setting_minutes[0]];
	displaybuf[63] = digits[setting_minutes[1]];

	/* underline the part being set */
	if (mode == MODE_SET_HRS) {
		if (setting_hours >= 10)
			displaybuf[84] = 1;
		displaybuf[85] = 1;
	} else if (mode == MODE_SET_MINS_TENS) {
		displaybuf[86] = 1;
	} else { /* mode == MODE_SET_MINS_ONES */
		displaybuf[87] = 1;
	}
}

static void testpattern(void) {
	for (uint8_t i = 0; i < sizeof(displaybuf); i++) {
		for (uint8_t b = 1; b; b <<= 1) {
			displaybuf[i] |= b;
			show();
		}
	}
	clear_displaybuf();
}

/* Draw pixels to displaybuf to be shown on the screen. */
static void render(void) {
	switch (mode) {
	case MODE_CLOCK:
		render_clock();
		break;
	case MODE_SET_HRS:
	case MODE_SET_MINS_TENS:
	case MODE_SET_MINS_ONES:
		render_setting_time();
		break;
	}
}

static void process_interrupt(void) {
	switch (interrupt) {
	case INTR_IGNORE:
		break;
	case INTR_SET_PRESSED:
		handle_set_pressed();
		break;
	case INTR_INC_PRESSED:
		handle_inc_pressed();
		break;
	case INTR_DEC_PRESSED:
		handle_dec_pressed();
		break;
	case INTR_GET_TIME:
		rtc_gettime();
		break;
	}
}

ISR(PCINT_vect) {
	/* wait for bouncing to stabilize */
	_delay_ms(10);

	uint8_t pinb = PINB;

	if ((pinb & (1 << PB0)) == 0) {
		interrupt = INTR_SET_PRESSED;
	} else if ((pinb & (1 << PB1)) == 0) {
		interrupt = INTR_INC_PRESSED;
	} else if ((pinb & (1 << PB2)) == 0) {
		interrupt = INTR_DEC_PRESSED;
	} else {
		interrupt = INTR_IGNORE;
	}
}

ISR(TIMER0_OVF_vect) {
	interrupt = INTR_GET_TIME;
}

int main(void) {
	DDRA  = 0xFF;
	PORTA = 0xFF;
	DDRB  = 0b01011000;
	PORTB = 0b01011111;

	PCMSK0 = 0;
	PCMSK1 = (1 << PCINT8) | (1 << PCINT9) | (1 << PCINT10);
	GIMSK = 1 << PCIE0;

	TIMSK = 1 << TOIE0;
	TCCR0A = 1 << TCW0;
	TCCR0B = 1 << CS01;

	MCUCR = 1 << SE;
	ACSRA = 1 << ACD;

	rtc_init();

	show();
	tm1640_all_on();

	if ((PINB & (1 << PB0)) == 0)
		testpattern();

	for (;;) {
		if (mode != MODE_CLOCK || clock_needs_render) {
			render();
			show();
		}
		clear_displaybuf();

		/* This guarantees we'll see exactly one interrupt per iteration of
		 * the loop. Excerpts from ATtiny861 datasheet:
		 *
		 * "When using the SEI instruction to enable interrupts, the
		 * instruction following SEI will be executed before any pending
		 * interrupts"
		 *
		 * "When the AVR exits from an interrupt, it will always return to
		 * the main program and execute one more instruction before any
		 * pending interrupt is served."
		 *
		 * "When using the CLI instruction to disable interrupts, the
		 * interrupts will be immediately disabled. No interrupt will be
		 * executed after the CLI instruction, even if it occurs
		 * simultaneously with the CLI instruction." */
		asm volatile (
			"sei\n"
			"sleep\n"
			"cli\n"
		);

		process_interrupt();
	}
}
