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
.equ porta = 0x1b
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

; write the display buffer to the displays
.macro flushbuffer
	; TODO
.endmacro

.macro dispdat_0
	cbi portb, 6
.endmacro

.macro dispdat_1
	sbi portb, 6
.endmacro

.macro incdig ; @0=hi, @1=lo, @2=maxhi, @3=maxlo, @4=minlo
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
	ldi @1, @4
end%:
.endmacro

.macro decdig ; @0=hi, @1=lo, @2=maxhi, @3=maxlo, @4=minlo
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
.if @4 != 0
	cpi @0, 0
	brne donemin%
	cpi @1, @4
	brsh donemin%
	ldi @1, @4
donemin%:
.endif
.endmacro

; store [rhrs|rmins]_[lo|hi] to the rtc
.macro settime
	; set 12 flag
	ori rhrs_hi, (1<<6)
	; TODO
	; clear 12 flag and am/pm flag
	andi rhrs_hi, 0b00011111
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

	; TODO have builtin timer interrupt after [60-cursecs] seconds with cause = update_time

	; TODO: set pulse width????

	; send "display on" command to each tm1640
	ldi r30, 0b10001000
	clr r31 ; module number (0-indexed)
__displayon_loop:
	rcall tm1640_start
	rcall tm1640_outb
	rcall tm1640_end
	inc r31
	cpi r31, 9
	brne __displayon_loop

tick:
	clear_displaybuf

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

	; TODO: render clock

	rjmp endtick

tick_set_hrs:
	sbrc rcause, causebit_set
	ldi rstate, exp2(statebit_set_mins)

	bst rcause, causebit_inc
	brtc __hrs_not_inc
	incdig rhrs_hi, rhrs_lo, 1, 2, 1
__hrs_not_inc:

	bst rcause, causebit_dec
	brtc __hrs_not_dec
	decdig rhrs_hi, rhrs_lo, 1, 2, 1
__hrs_not_dec:

	; TODO: render set_hrs

	rjmp endtick

tick_set_mins:
	sbrc rcause, causebit_set
	ldi rstate, exp2(statebit_confirm_yes)

	bst rcause, causebit_inc
	brtc __mins_not_inc
	incdig rmins_hi, rmins_lo, 5, 9, 0
__mins_not_inc:

	bst rcause, causebit_dec
	brtc __mins_not_dec
	decdig rmins_hi, rmins_lo, 5, 9, 0
__mins_not_dec:

	; TODO: render set_mins

	rjmp endtick

tick_confirm_yes:
	bst rcause, causebit_set
	brtc __cause_isnt_set
	settime
	; back to clock mode
	ldi rstate, exp2(statebit_clock)
__cause_isnt_set:

	sbrc rcause, causebit_inc
	ldi rstate, exp2(statebit_confirm_no)

	sbrc rcause, causebit_dec
	ldi rstate, exp2(statebit_confirm_no)

	; TODO: render confirm_yes

	rjmp endtick

tick_confirm_no:
	cpi rcause, exp2(causebit_set)
	brne __not_set
	rcall gettime
	ldi rstate, exp2(statebit_clock)
__not_set:

	sbrc rcause, causebit_inc
	ldi rstate, exp2(statebit_confirm_yes)

	sbrc rcause, causebit_dec
	ldi rstate, exp2(statebit_confirm_yes)

	; TODO: render confirm_no

	rjmp endtick

pcint:
	; no need to save registers in here because interrupts are only enabled
	; during sleep in `endtick`.
	; TODO: debounce all pcints
	;;;;;;;;;;;;;;;;;;;;;;;;;
	; use ret instead of reti because we don't want to reenable interrupts
	; until we tick to process this one.
	ret

gettime:
	; TODO
	; clear 12 flag and am/pm flag
	andi rhrs_hi, 0b00011111
	ret

; do a start condition on a module.
; r31=module_num (0-indexed)
tm1640_start:
	dispdat_1
	rcall clock_1
	dispdat_0
	rcall clock_0
	ret

; do an end condition on a module.
; r31=module_num (0-indexed)
tm1640_end:
	dispdat_0
	rcall clock_1
	dispdat_1
	rcall clock_0
	ret

; write a byte to a module.
; r30=byte, r31=module_num (0-indexed)
tm1640_outb:
.macro onebit
	dispdat_1
	sbrs r30, 0
	dispdat_0
	rcall clock_1
	rcall clock_0
	; 8 `ror`s restores r30 to original value
	ror r30
.endmacro
	onebit
	onebit
	onebit
	onebit
	onebit
	onebit
	onebit
	onebit
	ret

; set a module's clock LOW
; r31=module_num (0-indexed)
clock_0:
	cpi r31, 8
	breq __0_m8
	push r1
	push r30
	push r31
	ldi r30, 1
__0_loop:
	cpi r31, 0
	breq __0_shifted
	dec r31
	lsl r30
	rjmp __0_loop
__0_shifted:
	in r1, porta
	or r1, r30
	out porta, r1
	pop r31
	pop r30
	pop r1
	ret
__0_m8:
	cbi portb, 3
	ret

; set a module's clock HIGH
; r31=module_num (0-indexed)
clock_1:
	cpi r31, 8
	breq __1_m8
	push r1
	push r30
	push r31
	ldi r30, 1
__1_loop:
	cpi r31, 0
	breq __1_shifted
	dec r31
	lsl r30
	rjmp __1_loop
__1_shifted:
	in r1, porta
	com r30
	and r1, r30
	out porta, r1
	pop r31
	pop r30
	pop r1
	ret
__1_m8:
	sbi portb, 3
	ret

;;;;; data ;;;;;
.dseg
displaybuf: .byte 144
displaybuf_end:
