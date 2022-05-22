#define CRT_LOWORD(x) dword ptr [x+0]
#define CRT_HIWORD(x) dword ptr [x+4]

__declspec(naked) void _aulldvrm()
    {
        #define DVND    esp + 8       // stack address of dividend (a)
        #define DVSR    esp + 16      // stack address of divisor (b)

        __asm
        {
        push    esi

;
; Now do the divide.  First look to see if the divisor is less than 4194304K.
; If so, then we can use a simple algorithm with word divides, otherwise
; things get a little more complex.
;

        mov     eax,CRT_HIWORD(DVSR) ; check to see if divisor < 4194304K
        or      eax,eax
        jnz     short L1        ; nope, gotta do this the hard way
        mov     ecx,CRT_LOWORD(DVSR) ; load divisor
        mov     eax,CRT_HIWORD(DVND) ; load high word of dividend
        xor     edx,edx
        div     ecx             ; get high order bits of quotient
        mov     ebx,eax         ; save high bits of quotient
        mov     eax,CRT_LOWORD(DVND) ; edx:eax <- remainder:lo word of dividend
        div     ecx             ; get low order bits of quotient
        mov     esi,eax         ; ebx:esi <- quotient

;
; Now we need to do a multiply so that we can compute the remainder.
;
        mov     eax,ebx         ; set up high word of quotient
        mul     CRT_LOWORD(DVSR) ; CRT_HIWORD(QUOT) * DVSR
        mov     ecx,eax         ; save the result in ecx
        mov     eax,esi         ; set up low word of quotient
        mul     CRT_LOWORD(DVSR) ; CRT_LOWORD(QUOT) * DVSR
        add     edx,ecx         ; EDX:EAX = QUOT * DVSR
        jmp     short L2        ; complete remainder calculation

;
; Here we do it the hard way.  Remember, eax contains DVSRHI
;

L1:
        mov     ecx,eax         ; ecx:ebx <- divisor
        mov     ebx,CRT_LOWORD(DVSR)
        mov     edx,CRT_HIWORD(DVND) ; edx:eax <- dividend
        mov     eax,CRT_LOWORD(DVND)
L3:
        shr     ecx,1           ; shift divisor right one bit; hi bit <- 0
        rcr     ebx,1
        shr     edx,1           ; shift dividend right one bit; hi bit <- 0
        rcr     eax,1
        or      ecx,ecx
        jnz     short L3        ; loop until divisor < 4194304K
        div     ebx             ; now divide, ignore remainder
        mov     esi,eax         ; save quotient

;
; We may be off by one, so to check, we will multiply the quotient
; by the divisor and check the result against the orignal dividend
; Note that we must also check for overflow, which can occur if the
; dividend is close to 2**64 and the quotient is off by 1.
;

        mul     CRT_HIWORD(DVSR) ; QUOT * CRT_HIWORD(DVSR)
        mov     ecx,eax
        mov     eax,CRT_LOWORD(DVSR)
        mul     esi             ; QUOT * CRT_LOWORD(DVSR)
        add     edx,ecx         ; EDX:EAX = QUOT * DVSR
        jc      short L4        ; carry means Quotient is off by 1

;
; do long compare here between original dividend and the result of the
; multiply in edx:eax.  If original is larger or equal, we are ok, otherwise
; subtract one (1) from the quotient.
;

        cmp     edx,CRT_HIWORD(DVND) ; compare hi words of result and original
        ja      short L4        ; if result > original, do subtract
        jb      short L5        ; if result < original, we are ok
        cmp     eax,CRT_LOWORD(DVND) ; hi words are equal, compare lo words
        jbe     short L5        ; if less or equal we are ok, else subtract
L4:
        dec     esi             ; subtract 1 from quotient
        sub     eax,CRT_LOWORD(DVSR) ; subtract divisor from result
        sbb     edx,CRT_HIWORD(DVSR)
L5:
        xor     ebx,ebx         ; ebx:esi <- quotient

L2:
;
; Calculate remainder by subtracting the result from the original dividend.
; Since the result is already in a register, we will do the subtract in the
; opposite direction and negate the result.
;

        sub     eax,CRT_LOWORD(DVND) ; subtract dividend from result
        sbb     edx,CRT_HIWORD(DVND)
        neg     edx             ; otherwise, negate the result
        neg     eax
        sbb     edx,0

;
; Now we need to get the quotient into edx:eax and the remainder into ebx:ecx.
;
        mov     ecx,edx
        mov     edx,ebx
        mov     ebx,ecx
        mov     ecx,eax
        mov     eax,esi
;
; Just the cleanup left to do.  edx:eax contains the quotient.
; Restore the saved registers and return.
;

        pop     esi

        ret     16
        }

        #undef DVND
        #undef DVSR
    }

    __declspec(naked) void _aullrem()
    {
        #define DVND    esp + 8       // stack address of dividend (a)
        #define DVSR    esp + 16      // stack address of divisor (b)

        __asm
        {
        push    ebx

; Now do the divide.  First look to see if the divisor is less than 4194304K.
; If so, then we can use a simple algorithm with word divides, otherwise
; things get a little more complex.
;

        mov     eax,CRT_HIWORD(DVSR) ; check to see if divisor < 4194304K
        or      eax,eax
        jnz     short L1        ; nope, gotta do this the hard way
        mov     ecx,CRT_LOWORD(DVSR) ; load divisor
        mov     eax,CRT_HIWORD(DVND) ; load high word of dividend
        xor     edx,edx
        div     ecx             ; edx <- remainder, eax <- quotient
        mov     eax,CRT_LOWORD(DVND) ; edx:eax <- remainder:lo word of dividend
        div     ecx             ; edx <- final remainder
        mov     eax,edx         ; edx:eax <- remainder
        xor     edx,edx
        jmp     short L2        ; restore stack and return

;
; Here we do it the hard way.  Remember, eax contains DVSRHI
;

L1:
        mov     ecx,eax         ; ecx:ebx <- divisor
        mov     ebx,CRT_LOWORD(DVSR)
        mov     edx,CRT_HIWORD(DVND) ; edx:eax <- dividend
        mov     eax,CRT_LOWORD(DVND)
L3:
        shr     ecx,1           ; shift divisor right one bit; hi bit <- 0
        rcr     ebx,1
        shr     edx,1           ; shift dividend right one bit; hi bit <- 0
        rcr     eax,1
        or      ecx,ecx
        jnz     short L3        ; loop until divisor < 4194304K
        div     ebx             ; now divide, ignore remainder

;
; We may be off by one, so to check, we will multiply the quotient
; by the divisor and check the result against the orignal dividend
; Note that we must also check for overflow, which can occur if the
; dividend is close to 2**64 and the quotient is off by 1.
;

        mov     ecx,eax         ; save a copy of quotient in ECX
        mul     CRT_HIWORD(DVSR)
        xchg    ecx,eax         ; put partial product in ECX, get quotient in EAX
        mul     CRT_LOWORD(DVSR)
        add     edx,ecx         ; EDX:EAX = QUOT * DVSR
        jc      short L4        ; carry means Quotient is off by 1

;
; do long compare here between original dividend and the result of the
; multiply in edx:eax.  If original is larger or equal, we're ok, otherwise
; subtract the original divisor from the result.
;
        cmp     edx,CRT_HIWORD(DVND) ; compare hi words of result and original
        ja      short L4        ; if result > original, do subtract
        jb      short L5        ; if result < original, we're ok
        cmp     eax,CRT_LOWORD(DVND) ; hi words are equal, compare lo words
        jbe     short L5        ; if less or equal we're ok, else subtract
L4:
        sub     eax,CRT_LOWORD(DVSR) ; subtract divisor from result
        sbb     edx,CRT_HIWORD(DVSR)
L5:
;
; Calculate remainder by subtracting the result from the original dividend.
; Since the result is already in a register, we will perform the subtract in
; the opposite direction and negate the result to make it positive.
;
        sub     eax,CRT_LOWORD(DVND) ; subtract original dividend from result
        sbb     edx,CRT_HIWORD(DVND)
        neg     edx             ; and negate it
        neg     eax
        sbb     edx,0
;
; Just the cleanup left to do.  dx:ax contains the remainder.
; Restore the saved registers and return.
;
L2:
        pop     ebx
        ret     16
        }
        #undef DVND
        #undef DVSR
    }
    __declspec(naked) void _aullshr()
    {
        __asm
        {
        cmp     cl,64
        jae     short RETZERO
;
; Handle shifts of between 0 and 31 bits
;
        cmp     cl, 32
        jae     short MORE32
        shrd    eax,edx,cl
        shr     edx,cl
        ret
;
; Handle shifts of between 32 and 63 bits
;
MORE32:
        mov     eax,edx
        xor     edx,edx
        and     cl,31
        shr     eax,cl
        ret
;
; return 0 in edx:eax
;
RETZERO:
        xor     eax,eax
        xor     edx,edx
        ret
        }
    }
