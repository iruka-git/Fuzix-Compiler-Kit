;
;		True if TOS < HL
;
		.export __ccltu

		.setcpu 8085

		.code
;
;	FIXME: flags as well as hl should be set up
;
__ccltu:
		xchg
		pop	h
		shld	__retaddr
		pop	h
		mov	a,l
		sub	e
		mov	a,h
		sbb	d
		lxi	h,0
		rnc
		inr	l
		ret
