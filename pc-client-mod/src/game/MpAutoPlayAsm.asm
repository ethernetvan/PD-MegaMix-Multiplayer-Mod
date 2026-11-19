; MpAutoPlayAsm.asm — register-preserving detours for the PV key getters.
;
; The arcade ctrl keeps rcx (handler), r8, r9 and r11 live across
; GetPvKeyState{Down,Tapped} calls; the originals happen to clobber none
; of them. A plain C++ detour trashes the volatile set, so the detour
; entry saves every volatile register plus xmm0-5 around the C++ impl.
;
; int64 ApKey*Detour(void* handler /*rcx*/, int32_t key /*edx*/)

PUBLIC ApKeyDownDetour
PUBLIC ApKeyTappedDetour
EXTERN ApKeyDownImpl:PROC
EXTERN ApKeyTappedImpl:PROC

.CODE

SAVE_FRAME MACRO impl
    push rcx
    push rdx
    push r8
    push r9
    push r10
    push r11
    sub rsp, 088h                ; shadow 0-1F, xmm saves 20-7F, keeps 16-align
    movaps xmmword ptr [rsp+20h], xmm0
    movaps xmmword ptr [rsp+30h], xmm1
    movaps xmmword ptr [rsp+40h], xmm2
    movaps xmmword ptr [rsp+50h], xmm3
    movaps xmmword ptr [rsp+60h], xmm4
    movaps xmmword ptr [rsp+70h], xmm5
    call impl
    movaps xmm0, xmmword ptr [rsp+20h]
    movaps xmm1, xmmword ptr [rsp+30h]
    movaps xmm2, xmmword ptr [rsp+40h]
    movaps xmm3, xmmword ptr [rsp+50h]
    movaps xmm4, xmmword ptr [rsp+60h]
    movaps xmm5, xmmword ptr [rsp+70h]
    add rsp, 088h
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdx
    pop rcx
    ret
ENDM

ApKeyDownDetour PROC
    SAVE_FRAME ApKeyDownImpl
ApKeyDownDetour ENDP

ApKeyTappedDetour PROC
    SAVE_FRAME ApKeyTappedImpl
ApKeyTappedDetour ENDP

END
