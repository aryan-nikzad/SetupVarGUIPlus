#ifndef GFX_H
#define GFX_H

#include <efi.h>
#include <efilib.h>
#include "font_data.h"

/* Matches EFI_GRAPHICS_OUTPUT_BLT_PIXEL layout exactly (Blue,Green,Red,Reserved)
 * so the back buffer can be Blt() straight to video with no conversion. */
typedef struct {
    UINT8 b, g, r, a;
} RGBA;

static inline RGBA RGB(UINT8 r, UINT8 g, UINT8 b)
{
    RGBA c; c.r = r; c.g = g; c.b = b; c.a = 0xFF; return c;
}

extern UINTN gScreenW, gScreenH;

EFI_STATUS GfxInit(VOID);
VOID GfxPresent(VOID);
VOID GfxClear(RGBA color);
VOID GfxFillRect(INTN x, INTN y, INTN w, INTN h, RGBA color);
VOID GfxFillRoundedRect(INTN x, INTN y, INTN w, INTN h, INTN radius, RGBA color);
VOID GfxStrokeRect(INTN x, INTN y, INTN w, INTN h, INTN thickness, RGBA color);
VOID GfxHLine(INTN x, INTN y, INTN w, INTN thickness, RGBA color);

UINTN GfxTextWidth(CONST FONT_FACE *face, CONST CHAR16 *text);
VOID GfxDrawText(CONST FONT_FACE *face, INTN x, INTN y, CONST CHAR16 *text, RGBA color);
VOID GfxDrawTextClipped(CONST FONT_FACE *face, INTN x, INTN y, INTN maxW,
                         CONST CHAR16 *text, RGBA color);
/* draws text vertically centered within a box of given height, at given x */
VOID GfxDrawTextVCenter(CONST FONT_FACE *face, INTN x, INTN boxY, INTN boxH,
                         CONST CHAR16 *text, RGBA color);

#endif /* GFX_H */
