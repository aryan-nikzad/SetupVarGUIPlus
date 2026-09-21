#include "gfx.h"

UINTN gScreenW = 0, gScreenH = 0;

static EFI_GRAPHICS_OUTPUT_PROTOCOL *gGop = NULL;
static RGBA *gBackBuffer = NULL;

EFI_STATUS GfxInit(VOID)
{
    EFI_STATUS Status;
    UINTN bestMode = (UINTN)-1;
    UINT64 bestPixels = 0;
    UINTN i;

    Status = uefi_call_wrapper(BS->LocateProtocol, 3, &GraphicsOutputProtocol,
                                NULL, (VOID **)&gGop);
    if (EFI_ERROR(Status) || !gGop) return EFI_UNSUPPORTED;

    /* Pick the highest-resolution mode available (capped so the back
     * buffer stays a sane size), rather than trusting whatever mode the
     * firmware happened to boot into. */
    for (i = 0; i < gGop->Mode->MaxMode; i++) {
        UINTN infoSize;
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info = NULL;
        Status = uefi_call_wrapper(gGop->QueryMode, 4, gGop, i, &infoSize, &info);
        if (EFI_ERROR(Status) || !info) continue;

        if (info->PixelFormat == PixelRedGreenBlueReserved8BitPerColor ||
            info->PixelFormat == PixelBlueGreenRedReserved8BitPerColor) {
            UINT64 pixels = (UINT64)info->HorizontalResolution * info->VerticalResolution;
            if (info->HorizontalResolution <= 3840 && info->VerticalResolution <= 2160 &&
                pixels > bestPixels) {
                bestPixels = pixels;
                bestMode = i;
            }
        }
        FreePool(info);
    }

    if (bestMode != (UINTN)-1 && bestMode != gGop->Mode->Mode) {
        uefi_call_wrapper(gGop->SetMode, 2, gGop, bestMode);
    }

    gScreenW = gGop->Mode->Info->HorizontalResolution;
    gScreenH = gGop->Mode->Info->VerticalResolution;
    if (gScreenW == 0 || gScreenH == 0) return EFI_UNSUPPORTED;

    gBackBuffer = AllocateZeroPool(gScreenW * gScreenH * sizeof(RGBA));
    if (!gBackBuffer) return EFI_OUT_OF_RESOURCES;

    return EFI_SUCCESS;
}

VOID GfxPresent(VOID)
{
    if (!gGop || !gBackBuffer) return;
    uefi_call_wrapper(gGop->Blt, 10, gGop, (EFI_GRAPHICS_OUTPUT_BLT_PIXEL *)gBackBuffer,
                       EfiBltBufferToVideo, 0, 0, 0, 0, gScreenW, gScreenH, 0);
}

static inline VOID PutPixel(INTN x, INTN y, RGBA c)
{
    if (x < 0 || y < 0 || (UINTN)x >= gScreenW || (UINTN)y >= gScreenH) return;
    gBackBuffer[(UINTN)y * gScreenW + (UINTN)x] = c;
}

static inline RGBA GetPixel(INTN x, INTN y)
{
    RGBA z; z.r = z.g = z.b = z.a = 0;
    if (x < 0 || y < 0 || (UINTN)x >= gScreenW || (UINTN)y >= gScreenH) return z;
    return gBackBuffer[(UINTN)y * gScreenW + (UINTN)x];
}

/* alpha: 0..255 coverage of `c` over whatever is already at (x,y) */
static inline VOID BlendPixel(INTN x, INTN y, RGBA c, UINTN alpha)
{
    RGBA dst, out;
    UINTN inv;
    if (alpha == 0) return;
    if (alpha >= 255) { PutPixel(x, y, c); return; }
    dst = GetPixel(x, y);
    inv = 255 - alpha;
    out.r = (UINT8)((c.r * alpha + dst.r * inv) / 255);
    out.g = (UINT8)((c.g * alpha + dst.g * inv) / 255);
    out.b = (UINT8)((c.b * alpha + dst.b * inv) / 255);
    out.a = 0xFF;
    PutPixel(x, y, out);
}

VOID GfxClear(RGBA color)
{
    UINTN i, n = gScreenW * gScreenH;
    for (i = 0; i < n; i++) gBackBuffer[i] = color;
}

VOID GfxFillRect(INTN x, INTN y, INTN w, INTN h, RGBA color)
{
    INTN row, col;
    if (w <= 0 || h <= 0) return;
    for (row = 0; row < h; row++) {
        INTN yy = y + row;
        if (yy < 0 || (UINTN)yy >= gScreenH) continue;
        for (col = 0; col < w; col++) {
            INTN xx = x + col;
            if (xx < 0 || (UINTN)xx >= gScreenW) continue;
            gBackBuffer[(UINTN)yy * gScreenW + (UINTN)xx] = color;
        }
    }
}

VOID GfxHLine(INTN x, INTN y, INTN w, INTN thickness, RGBA color)
{
    GfxFillRect(x, y, w, thickness, color);
}

VOID GfxStrokeRect(INTN x, INTN y, INTN w, INTN h, INTN thickness, RGBA color)
{
    GfxFillRect(x, y, w, thickness, color);
    GfxFillRect(x, y + h - thickness, w, thickness, color);
    GfxFillRect(x, y, thickness, h, color);
    GfxFillRect(x + w - thickness, y, thickness, h, color);
}

/* Coverage (0..4) of a 2x2 supersampled point against a rounded-rect
 * outline, used to softly antialias the corners. */
static UINTN RoundedRectCoverage(INTN px, INTN py, INTN w, INTN h, INTN radius)
{
    UINTN hits = 0, s;
    static const INTN offs[4][2] = { {0, 0}, {1, 0}, {0, 1}, {1, 1} };
    for (s = 0; s < 4; s++) {
        /* sample at quarter-pixel offsets within this pixel */
        INTN sx = px * 2 + offs[s][0];
        INTN sy = py * 2 + offs[s][1];
        INTN fx = sx, fy = sy; /* in half-pixel units */
        INTN cx, cy, dx, dy;
        BOOLEAN inside = TRUE;

        /* work in half-pixel space: full rect is [0,2w) x [0,2h) */
        if (fx < 0 || fy < 0 || fx >= w * 2 || fy >= h * 2) { continue; }

        cx = radius * 2; cy = radius * 2;
        if (fx < cx && fy < cy) {                /* top-left corner */
            dx = cx - fx; dy = cy - fy;
            inside = (dx * dx + dy * dy) <= (radius * 2) * (radius * 2);
        } else if (fx >= w * 2 - cx && fy < cy) { /* top-right */
            dx = fx - (w * 2 - cx); dy = cy - fy;
            inside = (dx * dx + dy * dy) <= (radius * 2) * (radius * 2);
        } else if (fx < cx && fy >= h * 2 - cy) { /* bottom-left */
            dx = cx - fx; dy = fy - (h * 2 - cy);
            inside = (dx * dx + dy * dy) <= (radius * 2) * (radius * 2);
        } else if (fx >= w * 2 - cx && fy >= h * 2 - cy) { /* bottom-right */
            dx = fx - (w * 2 - cx); dy = fy - (h * 2 - cy);
            inside = (dx * dx + dy * dy) <= (radius * 2) * (radius * 2);
        }
        if (inside) hits++;
    }
    return hits;
}

VOID GfxFillRoundedRect(INTN x, INTN y, INTN w, INTN h, INTN radius, RGBA color)
{
    INTN row, col;
    if (w <= 0 || h <= 0) return;
    if (radius * 2 > w) radius = w / 2;
    if (radius * 2 > h) radius = h / 2;
    if (radius < 0) radius = 0;

    if (radius == 0) { GfxFillRect(x, y, w, h, color); return; }

    for (row = 0; row < h; row++) {
        for (col = 0; col < w; col++) {
            UINTN cov = RoundedRectCoverage(col, row, w, h, radius);
            if (cov == 0) continue;
            if (cov == 4) PutPixel(x + col, y + row, color);
            else BlendPixel(x + col, y + row, color, cov * 255 / 4);
        }
    }
}

/* ---- text ------------------------------------------------------------ */

static inline CONST GLYPH_INFO *FaceGlyph(CONST FONT_FACE *face, CHAR16 ch)
{
    if (ch < FONT_FIRST_CHAR || ch > FONT_LAST_CHAR) return NULL;
    return &face->glyphs[ch - FONT_FIRST_CHAR];
}

UINTN GfxTextWidth(CONST FONT_FACE *face, CONST CHAR16 *text)
{
    UINTN w = 0;
    for (; *text; text++) {
        CONST GLYPH_INFO *g = FaceGlyph(face, *text);
        w += g ? g->advance : (face->lineHeight / 2);
    }
    return w;
}

static VOID DrawGlyph(CONST FONT_FACE *face, CONST GLYPH_INFO *g, INTN penX,
                       INTN penY, RGBA color)
{
    INTN row, col;
    CONST UINT8 *bm;
    if (g->w == 0 || g->h == 0) return;
    bm = &face->data[g->dataOffset];
    for (row = 0; row < g->h; row++) {
        for (col = 0; col < g->w; col++) {
            UINT8 cov = bm[row * g->w + col];
            if (cov == 0) continue;
            BlendPixel(penX + g->xoff + col, penY + g->yoff + row, color, cov);
        }
    }
}

VOID GfxDrawText(CONST FONT_FACE *face, INTN x, INTN y, CONST CHAR16 *text, RGBA color)
{
    INTN penX = x;
    for (; *text; text++) {
        CONST GLYPH_INFO *g = FaceGlyph(face, *text);
        if (!g) { penX += face->lineHeight / 2; continue; }
        DrawGlyph(face, g, penX, y, color);
        penX += g->advance;
    }
}

VOID GfxDrawTextClipped(CONST FONT_FACE *face, INTN x, INTN y, INTN maxW,
                         CONST CHAR16 *text, RGBA color)
{
    INTN penX = x;
    CONST CHAR16 ellipsis[] = L"...";
    UINTN ellW = GfxTextWidth(face, ellipsis);

    /* fast path: fits fully */
    if (GfxTextWidth(face, text) <= (UINTN)maxW) {
        GfxDrawText(face, x, y, text, color);
        return;
    }

    for (; *text; text++) {
        CONST GLYPH_INFO *g = FaceGlyph(face, *text);
        UINTN adv = g ? g->advance : (face->lineHeight / 2);
        if ((UINTN)(penX - x) + adv + ellW > (UINTN)maxW) break;
        if (g) DrawGlyph(face, g, penX, y, color);
        penX += adv;
    }
    GfxDrawText(face, penX, y, ellipsis, color);
}

VOID GfxDrawTextVCenter(CONST FONT_FACE *face, INTN x, INTN boxY, INTN boxH,
                         CONST CHAR16 *text, RGBA color)
{
    INTN y = boxY + (boxH - face->lineHeight) / 2;
    GfxDrawText(face, x, y, text, color);
}
