/* pad.c — опрос геймпада.
   Вынесен отдельно, потому что нужен и главному циклу, и проигрывателю STR
   (там свой цикл, и он должен уметь ловить «пропустить ролик»). */

#include <sys/types.h>
#include <psxapi.h>
#include <psxetc.h>
#include <psxpad.h>
#include "vn.h"

static u_char padbuf[2][34];
static u_int  prev;

void pad_init(void)
{
    /* Приведение к char* тут только мешало: буфер и так u_char, а InitPAD
       ждёт uint8_t*. Лишний каст менял знаковость и давал предупреждение. */
    InitPAD(padbuf[0], 34, padbuf[1], 34);
    StartPAD();
    ChangeClearPAD(0);
    prev = 0;
}

u_int pad_now(void)
{
    PADTYPE *p = (PADTYPE *)padbuf[0];

    if (p->stat == 0 &&
        (p->type == PAD_ID_DIGITAL ||
         p->type == PAD_ID_ANALOG  ||
         p->type == PAD_ID_ANALOG_STICK))
        return ~p->btn;              /* в железе биты инвертированы */

    return 0;
}

/* Только фронт нажатия. Вызывать ровно раз за кадр. */
u_int pad_pressed(void)
{
    u_int now = pad_now();
    u_int hit = now & ~prev;
    prev = now;
    return hit;
}
