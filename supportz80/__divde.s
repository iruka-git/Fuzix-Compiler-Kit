;
;	Signed division. Do this via unsigned
;
		.export __div
		.export __divde
		.export __rem
		.export __remde

__div:
		ex	de,hl
		pop	hl
		ex	(sp),hl
__divde:
		push	bc
		ld	c,0
		call	signfix
		ex	de,hl
		call	signfix
		ex	de,hl
		;	C tells us if we need to negate

		call	__divdeu

		mov	a,c
		rra
		call	c,negate
		pop	bc
		ret

__rem:
		ex	de,hl
		pop	hl
		ex	(sp),hl
__remde:
		push	bc
		ex	de,hl
		call	signfix
		ex	de,hl
		ld	c,0
		call	signfix
		;	C tells us if we need to negate

		call	__remdeu

		mov	a,c
		rra
		call	c,negate
		pop	b
		ret

;	Turn HL positive, xor a with one if was
signfix:
		mov	a,h
		or	a
		ret	p
negate:
		cpl
		mov	h,a
		mov	a,l
		cpl
		mov	l,a
		inc	hl
		inc	c
		ret
