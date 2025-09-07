.device attiny861a

.def rhrs_lo = r20
.def rhrs_hi = r21
.def rmins_lo = r22
.def rmins_hi = r23
.def rcause = r24
.def rstate = r25
.def xl = r26
.def xh = r27
.def yl = r28
.def yh = r29

.equ ddrb = 0x17
.equ portb = 0x18
.equ ddra = 0x1a
.equ pcmsk1 = 0x22
.equ mcucr = 0x35
.equ gimsk = 0x3b

.equ statebit_clock       = 0
.equ statebit_set_hrs     = 1
.equ statebit_set_mins    = 2
.equ statebit_confirm_yes = 3
.equ statebit_confirm_no  = 4

.equ causebit_update_time = 0
.equ causebit_set         = 1
.equ causebit_inc         = 2
.equ causebit_dec         = 3

; interrupt vectors
.org 0x00
rjmp reset
.org 0x02
rjmp pcint

.macro clear_displaybuf
	ldi xl, low(displaybuf)
	ldi xh, high(displaybuf)
	ldi yl, low(displaybuf_end)
	ldi yh, high(displaybuf_end)
	clr r16
zero_at_x%:
	st x+, r16
	cp xl, yl
	brne zero_at_x%
	cp xh, yh
	brne zero_at_x%
.endmacro

; render the current state to displaybuf
.macro render
	clear_displaybuf
	; TODO
.endmacro

; write out the display buffer to the TM1640s
.macro flushbuffer
	; TODO
.endmacro

reset:
	; initial state
	ldi rstate, exp2(statebit_clock)
	ldi rcause, exp2(causebit_update_time)

	; port a is all outputs to the clocks of modules 1-8
	ldi r16, 0xff
	out ddra, r16
	; see schematic for port b
	ldi r16, 0b01111000
	out ddrb, r16
	; pull-ups on buttons
	ldi r16, 0b00000111
	out portb, r16
	; TODO NOTE NOTE NOTE: switch SDA between output low and input (hi-z) beacuse
	; TODO NOTE NOTE NOTE: there is a pullup on it.

	; enable sleep
	ldi r16, (1<<5) ; r16 (MCUCR) = SE
	out mcucr, r16

	; unmask PCINT 11:8
	ldi r16, (1<<4) ; r16 (GIMSK) = PCIE0
	out gimsk, r16

	; enable PCINTs 8 ("set"), 9 ("inc"), 10 ("dec")
	ldi r16, 0b111 ; r16 (PCMSK1) = PCINT8|PCINT9|PCINT10
	out pcmsk1, r16

	; TODO: clear tm1640 ram by writing 0 to all tm1640 registers
	; TODO: send "display on" commands to all tm1640S
	; TODO have builtin timer interrupt after [60-cursecs] seconds with cause = update_time
tick:
	sbrc rstate, statebit_clock
	rjmp tick_clock

	sbrc rstate, statebit_set_hrs
	rjmp tick_set_hrs

	sbrc rstate, statebit_set_mins
	rjmp tick_set_mins

	sbrc rstate, statebit_confirm_yes
	rjmp tick_confirm_yes

	sbrc rstate, statebit_confirm_no
	rjmp tick_confirm_no

; state processing functions return by jumping here
endtick:
	render
	flushbuffer
	; there won't be an interrupt between `sei` and `sleep` because AVR
	; guarantes "The instruction following SEI will be executed before
	; any pending interrupts".
	sei
	sleep
	rjmp tick

tick_clock:
	sbrc rcause, causebit_update_time
	rcall gettime

	sbrc rcause, causebit_set
	ldi rstate, exp2(statebit_set_hrs)

	rjmp endtick

.macro incdig ; @0=hi, @1=lo, @2=maxhi, @3=maxlo
	inc @1
	cpi @1, 10
	brne clamp%
	clr @1
	inc @0
clamp%:
	cpi @0, @2
	brlo end%
	brne wrap%
	cpi @1, @3+1
	brne end%
wrap%:
	clr @0
	clr @1
end%:
.endmacro

.macro decdig ; @0=hi, @1=lo, @2=maxhi, @3=maxlo
	subi @1, 1
	brcc done%
	ldi @1, 9
	subi @0, 1
	brcc done%
	ldi @0, @2
.if @3 != 9
	ldi @1, @3
.endif
done%:
.endmacro

; hours are always set and stored using 24-hour time, only converted to 12-hour
; when rendering.
tick_set_hrs:
	sbrc rcause, causebit_set
	ldi rstate, exp2(statebit_set_mins)

	bst rcause, causebit_inc
	brtc __hrs_not_inc
	incdig rhrs_hi, rhrs_lo, 2, 3
__hrs_not_inc:

	bst rcause, causebit_dec
	brtc __hrs_not_dec
	decdig rhrs_hi, rhrs_lo, 2, 3
__hrs_not_dec:

	rjmp endtick

tick_set_mins:
	sbrc rcause, causebit_set
	ldi rstate, exp2(statebit_confirm_yes)

	bst rcause, causebit_inc
	brtc __mins_not_inc
	incdig rmins_hi, rmins_lo, 5, 9
__mins_not_inc:

	bst rcause, causebit_dec
	brtc __mins_not_dec
	decdig rmins_hi, rmins_lo, 5, 9
__mins_not_dec:

	rjmp endtick

tick_confirm_yes:
	bst rcause, causebit_set
	brtc __cause_isnt_set
	; TODO: store setting_hrs/mins back to RTC
	; back to clock mode
	rcall gettime
	ldi rstate, exp2(statebit_clock)
__cause_isnt_set:

	sbrc rcause, causebit_inc
	ldi rstate, exp2(statebit_confirm_no)

	sbrc rcause, causebit_dec
	ldi rstate, exp2(statebit_confirm_no)

	rjmp endtick

tick_confirm_no:
	sbrc rcause, causebit_set
	ldi rstate, exp2(statebit_clock)

	sbrc rcause, causebit_inc
	ldi rstate, exp2(statebit_confirm_yes)

	sbrc rcause, causebit_dec
	ldi rstate, exp2(statebit_confirm_yes)

	rjmp endtick

gettime:
	; TODO
	ret

pcint:
	; no need to save registers in here because interrupts are only enabled
	; during sleep in `endtick`.
	; TODO: debounce all pcints
	;;;;;;;;;;;;;;;;;;;;;;;;;
	; use ret instead of reti because we don't want to reenable interrupts
	; until we tick to process this one.
	ret

;;;;; data ;;;;;
.dseg
displaybuf: .byte 144
displaybuf_end:
