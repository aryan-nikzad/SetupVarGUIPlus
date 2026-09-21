/*
 * SetupVarGUI - native UEFI application, graphical (GOP) front-end for
 * reading/writing "Setup"-style hidden BIOS/UEFI NVRAM variables.
 *
 * *** DANGER ***
 * Writing wrong values to firmware Setup variables can permanently brick
 * your motherboard. Only use offsets you have verified (e.g. via IFR
 * extraction) for your EXACT firmware version. You use this at your own
 * risk.
 */

#include <efi.h>
#include <efilib.h>
#include "gfx.h"
#include "offsets_data.h"

/* ==================== palette ==================== */
#define COL_BG          RGB(0xFA, 0xFA, 0xFB)
#define COL_CARD        RGB(0xFF, 0xFF, 0xFF)
#define COL_CARD_SEL    RGB(0x2E, 0x5C, 0xFF)
#define COL_TEXT        RGB(0x1A, 0x1A, 0x1E)
#define COL_TEXT_DIM    RGB(0x6B, 0x6E, 0x76)
#define COL_TEXT_ON_SEL RGB(0xFF, 0xFF, 0xFF)
#define COL_SUB_ON_SEL  RGB(0xDA, 0xE2, 0xFF)
#define COL_BORDER      RGB(0xE4, 0xE5, 0xE8)
#define COL_ACCENT      RGB(0x2E, 0x5C, 0xFF)
#define COL_WARN        RGB(0xE0, 0x5A, 0x2B)
#define COL_WARN_BG     RGB(0xFD, 0xEF, 0xE9)
#define COL_OK          RGB(0x1E, 0x9E, 0x5C)
#define COL_OK_BG       RGB(0xE9, 0xF9, 0xF0)

/* ==================== layout ==================== */
#define TOPBAR_H    76
#define TABBAR_Y    96
#define TABBAR_H    44
#define SEARCH_Y    156
#define SEARCH_H    56
#define LIST_Y      232
#define CARD_H      74
#define CARD_GAP    14
#define MARGIN      40
#define FOOTER_H    48

typedef enum { TAB_AIO = 0, TAB_MENU = 1, TAB_MORE = 2 } TAB_ID;

/* ==================== combined entry table ==================== */
#define TOTAL_ENTRIES_RAW (KNOWN_OFFSETS_COUNT + CUSTOM_OFFSETS_COUNT)
#define TOTAL_ENTRIES (TOTAL_ENTRIES_RAW > 0 ? TOTAL_ENTRIES_RAW : 1)

static CONST PREFILLED_ENTRY *AllEntries[TOTAL_ENTRIES];
static UINTN AllEntriesCount = 0;

static VOID BuildAllEntries(VOID)
{
    UINTN i;
    AllEntriesCount = 0;
    for (i = 0; i < KNOWN_OFFSETS_COUNT; i++)
        AllEntries[AllEntriesCount++] = &KNOWN_OFFSETS[i];
    for (i = 0; i < CUSTOM_OFFSETS_COUNT; i++)
        AllEntries[AllEntriesCount++] = &CUSTOM_OFFSETS[i];
}

/* ==================== small string helpers ==================== */

static BOOLEAN CharEqFold(CHAR16 a, CHAR16 b)
{
    if (a >= L'A' && a <= L'Z') a = a - L'A' + L'a';
    if (b >= L'A' && b <= L'Z') b = b - L'A' + L'a';
    return a == b;
}

/* case-insensitive substring test */
static BOOLEAN StriStrContains(CONST CHAR16 *hay, CONST CHAR16 *needle)
{
    UINTN hlen, nlen, i, j;
    if (!needle || needle[0] == 0) return TRUE;
    if (!hay) return FALSE;
    hlen = StrLen(hay);
    nlen = StrLen(needle);
    if (nlen > hlen) return FALSE;
    for (i = 0; i + nlen <= hlen; i++) {
        BOOLEAN match = TRUE;
        for (j = 0; j < nlen; j++) {
            if (!CharEqFold(hay[i + j], needle[j])) { match = FALSE; break; }
        }
        if (match) return TRUE;
    }
    return FALSE;
}

static VOID FormatHex(CHAR16 *buf, UINTN bufLen, UINTN val)
{
    SPrint(buf, bufLen * sizeof(CHAR16), L"0x%x", val);
}

/* ==================== NVRAM backend (unchanged logic) ==================== */

static EFI_STATUS FindVariableGuid(CONST CHAR16 *wanted, EFI_GUID *outGuid)
{
    EFI_STATUS Status;
    CHAR16 *VarName;
    EFI_GUID VendorGuid;

    VarName = AllocateZeroPool(512 * sizeof(CHAR16));
    if (!VarName) return EFI_OUT_OF_RESOURCES;
    VarName[0] = 0;
    ZeroMem(&VendorGuid, sizeof(VendorGuid));

    for (;;) {
        UINTN Size = 512 * sizeof(CHAR16);
        Status = uefi_call_wrapper(RT->GetNextVariableName, 3, &Size, VarName, &VendorGuid);
        if (Status == EFI_NOT_FOUND) break;
        if (Status == EFI_BUFFER_TOO_SMALL) {
            FreePool(VarName);
            VarName = AllocateZeroPool(Size);
            if (!VarName) return EFI_OUT_OF_RESOURCES;
            Status = uefi_call_wrapper(RT->GetNextVariableName, 3, &Size, VarName, &VendorGuid);
        }
        if (EFI_ERROR(Status)) break;

        if (StrCmp(VarName, (CHAR16 *)wanted) == 0) {
            CopyMem(outGuid, &VendorGuid, sizeof(EFI_GUID));
            FreePool(VarName);
            return EFI_SUCCESS;
        }
    }
    FreePool(VarName);
    return EFI_NOT_FOUND;
}

static EFI_STATUS ReadVarBuffer(CONST CHAR16 *name, EFI_GUID *guid,
                                 VOID **data, UINTN *size, UINT32 *attr)
{
    EFI_STATUS Status;
    UINTN Size = 0;

    Status = uefi_call_wrapper(RT->GetVariable, 5, (CHAR16 *)name, guid, attr, &Size, NULL);
    if (Status != EFI_BUFFER_TOO_SMALL) return Status;

    *data = AllocateZeroPool(Size);
    if (!*data) return EFI_OUT_OF_RESOURCES;

    Status = uefi_call_wrapper(RT->GetVariable, 5, (CHAR16 *)name, guid, attr, &Size, *data);
    if (EFI_ERROR(Status)) { FreePool(*data); *data = NULL; return Status; }

    *size = Size;
    return EFI_SUCCESS;
}

static UINTN ParseHex(CONST CHAR16 *s)
{
    UINTN val = 0;
    if (s[0] == L'0' && (s[1] == L'x' || s[1] == L'X')) s += 2;
    while (*s) {
        CHAR16 c = *s++;
        UINTN d;
        if (c >= L'0' && c <= L'9') d = c - L'0';
        else if (c >= L'a' && c <= L'f') d = c - L'a' + 10;
        else if (c >= L'A' && c <= L'F') d = c - L'A' + 10;
        else break;
        val = (val << 4) | d;
    }
    return val;
}

/* ==================== input ==================== */

static EFI_INPUT_KEY WaitKey(VOID)
{
    EFI_INPUT_KEY Key;
    UINTN Index;
    EFI_STATUS Status;
    do {
        uefi_call_wrapper(BS->WaitForEvent, 3, 1, &ST->ConIn->WaitForKey, &Index);
        Status = uefi_call_wrapper(ST->ConIn->ReadKeyStroke, 2, ST->ConIn, &Key);
    } while (EFI_ERROR(Status));
    return Key;
}

/* ==================== chrome (top bar / tabs / footer) ==================== */

static VOID DrawTopBar(CONST CHAR16 *subtitle)
{
    GfxFillRect(0, 0, gScreenW, TOPBAR_H, COL_CARD);
    GfxHLine(0, TOPBAR_H, gScreenW, 1, COL_BORDER);
    GfxDrawTextVCenter(&FONT_TITLE, MARGIN, 0, TOPBAR_H, L"SetupVar GUI", COL_TEXT);
    if (subtitle && subtitle[0]) {
        UINTN tw = GfxTextWidth(&FONT_TITLE, L"SetupVar GUI");
        GfxDrawTextVCenter(&FONT_SMALL, MARGIN + tw + 16, 0, TOPBAR_H, subtitle, COL_TEXT_DIM);
    }
}

static CONST CHAR16 *TabLabels[3] = { L"AIO", L"Menu", L"More" };

static VOID DrawTabBar(TAB_ID active)
{
    INTN tx = MARGIN;
    UINTN i;
    for (i = 0; i < 3; i++) {
        UINTN tw = GfxTextWidth(&FONT_BODY, TabLabels[i]) + 40;
        BOOLEAN sel = ((UINTN)active == i);
        GfxFillRoundedRect(tx, TABBAR_Y, tw, TABBAR_H, 10, sel ? COL_ACCENT : COL_CARD);
        if (!sel) GfxStrokeRect(tx, TABBAR_Y, tw, TABBAR_H, 1, COL_BORDER);
        GfxDrawTextVCenter(&FONT_BODY, tx + 20, TABBAR_Y, TABBAR_H, TabLabels[i],
                            sel ? COL_TEXT_ON_SEL : COL_TEXT);
        tx += tw + 12;
    }
}

static VOID DrawFooterHint(CONST CHAR16 *hint)
{
    GfxFillRect(0, gScreenH - FOOTER_H, gScreenW, FOOTER_H, COL_CARD);
    GfxHLine(0, gScreenH - FOOTER_H, gScreenW, 1, COL_BORDER);
    GfxDrawTextVCenter(&FONT_SMALL, MARGIN, gScreenH - FOOTER_H, FOOTER_H, hint, COL_TEXT_DIM);
}

static VOID DrawSearchBox(CONST CHAR16 *query, CONST CHAR16 *placeholder)
{
    INTN sx = MARGIN, sy = SEARCH_Y, sw = gScreenW - MARGIN * 2, sh = SEARCH_H;
    GfxFillRoundedRect(sx, sy, sw, sh, 12, COL_CARD);
    GfxStrokeRect(sx, sy, sw, sh, 1, COL_BORDER);
    if (query && query[0]) {
        GfxDrawTextVCenter(&FONT_BODY, sx + 24, sy, sh, query, COL_TEXT);
        {
            UINTN qw = GfxTextWidth(&FONT_BODY, query);
            GfxFillRect(sx + 24 + qw + 2, sy + 14, 2, sh - 28, COL_ACCENT);
        }
    } else {
        GfxDrawTextVCenter(&FONT_BODY, sx + 24, sy, sh, placeholder, COL_TEXT_DIM);
    }
}

/* generic list card: title + subtitle, selectable */
static VOID DrawListCard(INTN y, CONST CHAR16 *title, CONST CHAR16 *subtitle, BOOLEAN sel)
{
    INTN x = MARGIN, w = gScreenW - MARGIN * 2, h = CARD_H;
    RGBA cardColor = sel ? COL_CARD_SEL : COL_CARD;
    RGBA titleColor = sel ? COL_TEXT_ON_SEL : COL_TEXT;
    RGBA subColor = sel ? COL_SUB_ON_SEL : COL_TEXT_DIM;

    GfxFillRoundedRect(x, y, w, h, 12, cardColor);
    if (!sel) GfxStrokeRect(x, y, w, h, 1, COL_BORDER);

    GfxDrawTextClipped(&FONT_BODY, x + 24, y + 14, w - 48, title, titleColor);
    if (subtitle && subtitle[0])
        GfxDrawTextClipped(&FONT_SMALL, x + 24, y + 42, w - 48, subtitle, subColor);
}

/* how many cards fit in the list area above the footer */
static UINTN VisibleCardCount(VOID)
{
    INTN avail = (INTN)gScreenH - FOOTER_H - LIST_Y;
    INTN per = CARD_H + CARD_GAP;
    if (avail < per) return 1;
    return (UINTN)(avail / per);
}

/* ==================== value editor screen ==================== */

/* Renders the current-value / new-value editor for one NVRAM byte/word/
 * dword. If label is non-NULL, it's shown as context (came from a list
 * entry); offset/size are always prefilled here (the "manual" entry
 * screen builds these itself and calls this same renderer). */
static VOID ScreenEditValue(CONST CHAR16 *storeName, UINTN offset, UINTN size,
                             CONST CHAR16 *label, CONST CHAR16 *source)
{
    EFI_GUID guid;
    EFI_STATUS Status;
    VOID *data = NULL;
    UINTN dataSize = 0;
    UINT32 attr = 0;
    UINT32 curVal = 0;
    CHAR16 valBuf[20] = L"";
    UINTN valLen = 0;
    EFI_INPUT_KEY Key;
    BOOLEAN done = FALSE;
    BOOLEAN found = TRUE;
    CHAR16 line[160];

    GfxClear(COL_BG);
    DrawTopBar(L"Edit Value");

    Status = FindVariableGuid(storeName, &guid);
    if (EFI_ERROR(Status)) {
        found = FALSE;
    } else {
        Status = ReadVarBuffer(storeName, &guid, &data, &dataSize, &attr);
        if (EFI_ERROR(Status)) found = FALSE;
    }

    if (found && offset + size > dataSize) found = FALSE;

    if (found) {
        curVal = 0;
        CopyMem(&curVal, (UINT8 *)data + offset, size);
    }

    for (;;) {
        INTN cardX = MARGIN, cardY = 116;
        INTN cardW = gScreenW - MARGIN * 2;
        INTN infoH = 190;

        GfxClear(COL_BG);
        DrawTopBar(L"Edit Value");

        GfxFillRoundedRect(cardX, cardY, cardW, infoH, 14, COL_CARD);
        GfxStrokeRect(cardX, cardY, cardW, infoH, 1, COL_BORDER);

        if (label && label[0]) {
            GfxDrawTextClipped(&FONT_HEADING, cardX + 28, cardY + 20, cardW - 56, label, COL_TEXT);
        } else {
            GfxDrawText(&FONT_HEADING, cardX + 28, cardY + 20, L"Manual edit", COL_TEXT);
        }

        SPrint(line, sizeof(line), L"Variable: %s     Offset: 0x%x     Size: %d byte(s)",
               storeName, offset, size);
        GfxDrawText(&FONT_SMALL, cardX + 28, cardY + 60, line, COL_TEXT_DIM);

        if (source && source[0]) {
            SPrint(line, sizeof(line), L"Source: %s", source);
            GfxDrawText(&FONT_SMALL, cardX + 28, cardY + 84, line, COL_TEXT_DIM);
        }

        if (!found) {
            GfxFillRoundedRect(cardX + 28, cardY + 116, cardW - 56, 44, 8, COL_WARN_BG);
            GfxDrawTextVCenter(&FONT_SMALL, cardX + 44, cardY + 116, 44,
                                L"Could not read this variable/offset on this machine.", COL_WARN);
        } else {
            SPrint(line, sizeof(line), L"Current value: 0x%x", curVal);
            GfxDrawText(&FONT_BODY, cardX + 28, cardY + 120, line, COL_TEXT);
        }

        if (found) {
            INTN inY = cardY + infoH + 24;
            SPrint(line, sizeof(line), L"New value (hex):");
            GfxDrawText(&FONT_BODY, cardX, inY, line, COL_TEXT);

            {
                INTN bx = cardX, by = inY + 36, bw = 260, bh = 52;
                GfxFillRoundedRect(bx, by, bw, bh, 10, COL_CARD);
                GfxStrokeRect(bx, by, bw, bh, 1, COL_ACCENT);
                GfxDrawTextVCenter(&FONT_BODY, bx + 18, by, bh,
                                    valLen ? valBuf : L"type hex value...",
                                    valLen ? COL_TEXT : COL_TEXT_DIM);
                if (valLen) {
                    UINTN vw = GfxTextWidth(&FONT_BODY, valBuf);
                    GfxFillRect(bx + 18 + vw + 2, by + 12, 2, bh - 24, COL_ACCENT);
                }
            }
        }

        if (found) {
            DrawFooterHint(L"Type hex digits    Enter Write    Esc Back    Backspace Delete");
        } else {
            DrawFooterHint(L"Esc Back");
        }

        GfxPresent();

        if (!found) { WaitKey(); if (data) FreePool(data); return; }

        Key = WaitKey();
        if (Key.ScanCode == SCAN_ESC) { done = TRUE; }
        else if (Key.UnicodeChar == CHAR_BACKSPACE) {
            if (valLen > 0) { valLen--; valBuf[valLen] = 0; }
        } else if (Key.UnicodeChar == CHAR_CARRIAGE_RETURN) {
            if (valLen > 0) {
                UINT32 newVal = (UINT32)ParseHex(valBuf);
                BOOLEAN confirmed = FALSE;

                /* confirmation overlay */
                {
                    INTN mw = 640, mh = 220;
                    INTN mx = (gScreenW - mw) / 2, my = (gScreenH - mh) / 2;
                    EFI_INPUT_KEY ck;

                    GfxFillRect(0, 0, gScreenW, gScreenH, RGB(0, 0, 0)); /* dim backdrop approx */
                    /* redraw a translucent-ish backdrop by just using a soft gray instead
                       of true alpha (keeps this simple and fast) */
                    GfxClear(RGB(0x33, 0x34, 0x38));
                    GfxFillRoundedRect(mx, my, mw, mh, 16, COL_CARD);
                    GfxStrokeRect(mx, my, mw, mh, 1, COL_BORDER);
                    GfxDrawText(&FONT_HEADING, mx + 28, my + 22, L"Write to NVRAM?", COL_TEXT);
                    SPrint(line, sizeof(line), L"%s:0x%x -> 0x%x  (was 0x%x)",
                           storeName, offset, newVal, curVal);
                    GfxDrawText(&FONT_BODY, mx + 28, my + 66, line, COL_TEXT_DIM);
                    GfxDrawText(&FONT_SMALL, mx + 28, my + 100,
                                L"A wrong value at a wrong offset can brick your firmware.",
                                COL_WARN);
                    GfxDrawText(&FONT_SMALL, mx + 28, my + 122,
                                L"Only continue if you have verified this setting.", COL_WARN);
                    GfxDrawTextVCenter(&FONT_BODY, mx + 28, my + mh - 56, 40,
                                        L"Enter = Write        Esc = Cancel", COL_TEXT_DIM);
                    GfxPresent();

                    ck = WaitKey();
                    confirmed = (ck.UnicodeChar == CHAR_CARRIAGE_RETURN);
                }

                if (confirmed) {
                    CopyMem((UINT8 *)data + offset, &newVal, size);
                    Status = uefi_call_wrapper(RT->SetVariable, 5, (CHAR16 *)storeName, &guid,
                                                attr, dataSize, data);
                    /* result banner */
                    GfxClear(COL_BG);
                    DrawTopBar(L"Edit Value");
                    {
                        INTN bx = MARGIN, by = 140, bw = gScreenW - MARGIN * 2, bh = 90;
                        BOOLEAN ok = !EFI_ERROR(Status);
                        GfxFillRoundedRect(bx, by, bw, bh, 14, ok ? COL_OK_BG : COL_WARN_BG);
                        GfxDrawTextVCenter(&FONT_BODY, bx + 24, by, bh,
                                            ok ? L"Write succeeded." : L"Write failed.",
                                            ok ? COL_OK : COL_WARN);
                    }
                    DrawFooterHint(L"Press any key to continue");
                    GfxPresent();
                    WaitKey();
                    curVal = newVal;
                }
                valLen = 0; valBuf[0] = 0;
            }
        } else if (Key.UnicodeChar) {
            CHAR16 c = Key.UnicodeChar;
            BOOLEAN hex = (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') ||
                          (c >= L'A' && c <= L'F') || (c == L'x') || (c == L'X');
            if (hex && valLen + 1 < 20) { valBuf[valLen++] = c; valBuf[valLen] = 0; }
        }

        if (done) break;
    }

    if (data) FreePool(data);
}

/* ==================== AIO tab ==================== */

typedef struct {
    CHAR16 query[64];
    UINTN filtered[TOTAL_ENTRIES];
    UINTN filteredCount;
    UINTN sel, top;
} LIST_STATE;

static VOID AioRefilter(LIST_STATE *st)
{
    UINTN i;
    st->filteredCount = 0;
    for (i = 0; i < AllEntriesCount; i++) {
        CONST PREFILLED_ENTRY *e = AllEntries[i];
        BOOLEAN match;
        if (st->query[0] == 0) {
            match = TRUE;
        } else {
            CHAR16 offbuf[24];
            FormatHex(offbuf, 24, e->Offset);
            match = StriStrContains(e->Desc, st->query) ||
                    StriStrContains(e->Source, st->query) ||
                    StriStrContains(offbuf, st->query) ||
                    StriStrContains(e->StoreName, st->query);
        }
        if (match) st->filtered[st->filteredCount++] = i;
    }
    if (st->filteredCount == 0) st->sel = 0;
    else if (st->sel >= st->filteredCount) st->sel = st->filteredCount - 1;
    if (st->top > st->sel) st->top = st->sel;
}

/* returns TRUE if Tab was pressed (caller should switch tab) */
static BOOLEAN RunAioTab(LIST_STATE *st)
{
    UINTN visible = VisibleCardCount();
    EFI_INPUT_KEY Key;

    AioRefilter(st);

    for (;;) {
        UINTN i;
        CHAR16 countLine[64];

        GfxClear(COL_BG);
        DrawTopBar(NULL);
        DrawTabBar(TAB_AIO);
        DrawSearchBox(st->query, L"Search settings by name, offset, or driver...");

        SPrint(countLine, sizeof(countLine), L"%d of %d settings", st->filteredCount, AllEntriesCount);
        GfxDrawText(&FONT_SMALL, MARGIN, SEARCH_Y + SEARCH_H + 10, countLine, COL_TEXT_DIM);

        if (st->filteredCount == 0) {
            GfxDrawText(&FONT_BODY, MARGIN, LIST_Y + 20, L"No matches.", COL_TEXT_DIM);
        } else {
            if (st->sel < st->top) st->top = st->sel;
            if (st->sel >= st->top + visible) st->top = st->sel - visible + 1;

            for (i = 0; i < visible && (st->top + i) < st->filteredCount; i++) {
                UINTN idx = st->filtered[st->top + i];
                CONST PREFILLED_ENTRY *e = AllEntries[idx];
                CHAR16 sub[128];
                SPrint(sub, sizeof(sub), L"%s : 0x%x  |  %d byte%s  |  %s",
                       e->StoreName, e->Offset, e->Size, e->Size == 1 ? L"" : L"s", e->Source);
                DrawListCard(LIST_Y + (INTN)i * (CARD_H + CARD_GAP), e->Desc, sub,
                             (st->top + i) == st->sel);
            }
        }

        DrawFooterHint(L"Type to search    Up/Down Move    Enter Open    Esc Clear search    Tab Switch mode");
        GfxPresent();

        Key = WaitKey();
        if (Key.UnicodeChar == 0x09) return TRUE;
        if (Key.ScanCode == SCAN_UP) { if (st->sel > 0) st->sel--; }
        else if (Key.ScanCode == SCAN_DOWN) { if (st->sel + 1 < st->filteredCount) st->sel++; }
        else if (Key.ScanCode == SCAN_PAGE_UP) {
            st->sel = (st->sel > visible) ? st->sel - visible : 0;
        } else if (Key.ScanCode == SCAN_PAGE_DOWN) {
            st->sel = (st->sel + visible < st->filteredCount) ? st->sel + visible :
                      (st->filteredCount ? st->filteredCount - 1 : 0);
        } else if (Key.ScanCode == SCAN_ESC) {
            if (st->query[0]) { st->query[0] = 0; AioRefilter(st); }
        } else if (Key.UnicodeChar == CHAR_BACKSPACE) {
            UINTN len = StrLen(st->query);
            if (len > 0) { st->query[len - 1] = 0; AioRefilter(st); }
        } else if (Key.UnicodeChar == CHAR_CARRIAGE_RETURN) {
            if (st->filteredCount > 0) {
                CONST PREFILLED_ENTRY *e = AllEntries[st->filtered[st->sel]];
                ScreenEditValue(e->StoreName, e->Offset, e->Size, e->Desc, e->Source);
            }
        } else if (Key.UnicodeChar >= 0x20 && Key.UnicodeChar < 0x7F) {
            UINTN len = StrLen(st->query);
            if (len + 1 < 64) {
                st->query[len] = Key.UnicodeChar;
                st->query[len + 1] = 0;
                AioRefilter(st);
            }
        }
    }
}

/* ==================== Menu tab (grouped by source driver) ==================== */

#define MAX_CATEGORIES 512
typedef struct {
    CONST CHAR16 *Name;
    UINTN Count;
} CATEGORY;

static CATEGORY Categories[MAX_CATEGORIES];
static UINTN CategoryCount = 0;
static BOOLEAN CategoriesBuilt = FALSE;

static VOID BuildCategories(VOID)
{
    UINTN i, j;
    if (CategoriesBuilt) return;
    CategoryCount = 0;
    for (i = 0; i < AllEntriesCount; i++) {
        CONST CHAR16 *src = AllEntries[i]->Source;
        BOOLEAN foundCat = FALSE;
        for (j = 0; j < CategoryCount; j++) {
            if (StrCmp(Categories[j].Name, src) == 0) { Categories[j].Count++; foundCat = TRUE; break; }
        }
        if (!foundCat && CategoryCount < MAX_CATEGORIES) {
            Categories[CategoryCount].Name = src;
            Categories[CategoryCount].Count = 1;
            CategoryCount++;
        }
    }
    CategoriesBuilt = TRUE;
}

typedef struct {
    BOOLEAN drilled;
    UINTN catSel, catTop;
    CONST CHAR16 *activeCategory;
    LIST_STATE inner;
} MENU_STATE;

static VOID MenuRefilterInner(MENU_STATE *st)
{
    UINTN i;
    st->inner.filteredCount = 0;
    for (i = 0; i < AllEntriesCount; i++) {
        CONST PREFILLED_ENTRY *e = AllEntries[i];
        if (StrCmp(e->Source, st->activeCategory) != 0) continue;
        if (st->inner.query[0] != 0) {
            CHAR16 offbuf[24];
            FormatHex(offbuf, 24, e->Offset);
            if (!StriStrContains(e->Desc, st->inner.query) &&
                !StriStrContains(offbuf, st->inner.query))
                continue;
        }
        st->inner.filtered[st->inner.filteredCount++] = i;
    }
    if (st->inner.filteredCount == 0) st->inner.sel = 0;
    else if (st->inner.sel >= st->inner.filteredCount) st->inner.sel = st->inner.filteredCount - 1;
    if (st->inner.top > st->inner.sel) st->inner.top = st->inner.sel;
}

static BOOLEAN RunMenuTab(MENU_STATE *st)
{
    UINTN visible = VisibleCardCount();
    EFI_INPUT_KEY Key;
    BuildCategories();

    for (;;) {
        if (!st->drilled) {
            UINTN i;
            CHAR16 countLine[64];

            GfxClear(COL_BG);
            DrawTopBar(NULL);
            DrawTabBar(TAB_MENU);

            SPrint(countLine, sizeof(countLine), L"%d categories", CategoryCount);
            GfxDrawText(&FONT_SMALL, MARGIN, TABBAR_Y + TABBAR_H + 14, countLine, COL_TEXT_DIM);

            if (CategoryCount == 0) {
                GfxDrawText(&FONT_BODY, MARGIN, LIST_Y, L"No entries baked into this binary yet.", COL_TEXT_DIM);
                GfxDrawText(&FONT_SMALL, MARGIN, LIST_Y + 32,
                            L"Run build_all.sh against a BIOS image to populate this.", COL_TEXT_DIM);
            } else {
                INTN catListY = SEARCH_Y;
                if (st->catSel < st->catTop) st->catTop = st->catSel;
                if (st->catSel >= st->catTop + visible) st->catTop = st->catSel - visible + 1;

                for (i = 0; i < visible && (st->catTop + i) < CategoryCount; i++) {
                    UINTN idx = st->catTop + i;
                    CHAR16 sub[64];
                    SPrint(sub, sizeof(sub), L"%d setting%s", Categories[idx].Count,
                           Categories[idx].Count == 1 ? L"" : L"s");
                    DrawListCard(catListY + (INTN)i * (CARD_H + CARD_GAP), Categories[idx].Name,
                                 sub, (st->catTop + i) == st->catSel);
                }
            }

            DrawFooterHint(L"Up/Down Move    Enter Open category    Tab Switch mode");
            GfxPresent();

            Key = WaitKey();
            if (Key.UnicodeChar == 0x09) return TRUE;
            if (Key.ScanCode == SCAN_UP) { if (st->catSel > 0) st->catSel--; }
            else if (Key.ScanCode == SCAN_DOWN) { if (st->catSel + 1 < CategoryCount) st->catSel++; }
            else if (Key.ScanCode == SCAN_PAGE_UP) {
                st->catSel = (st->catSel > visible) ? st->catSel - visible : 0;
            } else if (Key.ScanCode == SCAN_PAGE_DOWN) {
                st->catSel = (st->catSel + visible < CategoryCount) ? st->catSel + visible :
                             (CategoryCount ? CategoryCount - 1 : 0);
            } else if (Key.UnicodeChar == CHAR_CARRIAGE_RETURN && CategoryCount > 0) {
                st->drilled = TRUE;
                st->activeCategory = Categories[st->catSel].Name;
                st->inner.query[0] = 0;
                st->inner.sel = st->inner.top = 0;
                MenuRefilterInner(st);
            }
        } else {
            UINTN i;
            CHAR16 header[128];

            GfxClear(COL_BG);
            DrawTopBar(NULL);
            DrawTabBar(TAB_MENU);

            SPrint(header, sizeof(header), L"%s  (%d of %d)", st->activeCategory,
                   st->inner.filteredCount, Categories[st->catSel].Count);
            GfxDrawTextClipped(&FONT_BODY, MARGIN, TABBAR_Y + TABBAR_H + 16,
                                gScreenW - MARGIN * 2, header, COL_TEXT);

            {
                INTN sx = MARGIN, sy = TABBAR_Y + TABBAR_H + 16 + 30, sw = gScreenW - MARGIN * 2, sh = SEARCH_H;
                GfxFillRoundedRect(sx, sy, sw, sh, 12, COL_CARD);
                GfxStrokeRect(sx, sy, sw, sh, 1, COL_BORDER);
                if (st->inner.query[0]) {
                    GfxDrawTextVCenter(&FONT_BODY, sx + 24, sy, sh, st->inner.query, COL_TEXT);
                    {
                        UINTN qw = GfxTextWidth(&FONT_BODY, st->inner.query);
                        GfxFillRect(sx + 24 + qw + 2, sy + 14, 2, sh - 28, COL_ACCENT);
                    }
                } else {
                    GfxDrawTextVCenter(&FONT_BODY, sx + 24, sy, sh,
                                        L"Filter within this category...", COL_TEXT_DIM);
                }
            }

            if (st->inner.filteredCount == 0) {
                GfxDrawText(&FONT_BODY, MARGIN, LIST_Y + 20 + 30, L"No matches.", COL_TEXT_DIM);
            } else {
                INTN menuListY = LIST_Y + 30;
                if (st->inner.sel < st->inner.top) st->inner.top = st->inner.sel;
                if (st->inner.sel >= st->inner.top + visible) st->inner.top = st->inner.sel - visible + 1;

                for (i = 0; i < visible && (st->inner.top + i) < st->inner.filteredCount; i++) {
                    UINTN idx = st->inner.filtered[st->inner.top + i];
                    CONST PREFILLED_ENTRY *e = AllEntries[idx];
                    CHAR16 sub[96];
                    SPrint(sub, sizeof(sub), L"%s : 0x%x  |  %d byte%s",
                           e->StoreName, e->Offset, e->Size, e->Size == 1 ? L"" : L"s");
                    DrawListCard(menuListY + (INTN)i * (CARD_H + CARD_GAP), e->Desc, sub,
                                 (st->inner.top + i) == st->inner.sel);
                }
            }

            DrawFooterHint(L"Type to filter    Up/Down Move    Enter Open    Esc Back to categories");
            GfxPresent();

            Key = WaitKey();
            if (Key.UnicodeChar == 0x09) return TRUE;
            if (Key.ScanCode == SCAN_ESC) { st->drilled = FALSE; }
            else if (Key.ScanCode == SCAN_UP) { if (st->inner.sel > 0) st->inner.sel--; }
            else if (Key.ScanCode == SCAN_DOWN) {
                if (st->inner.sel + 1 < st->inner.filteredCount) st->inner.sel++;
            } else if (Key.ScanCode == SCAN_PAGE_UP) {
                st->inner.sel = (st->inner.sel > visible) ? st->inner.sel - visible : 0;
            } else if (Key.ScanCode == SCAN_PAGE_DOWN) {
                st->inner.sel = (st->inner.sel + visible < st->inner.filteredCount) ?
                                 st->inner.sel + visible :
                                 (st->inner.filteredCount ? st->inner.filteredCount - 1 : 0);
            } else if (Key.UnicodeChar == CHAR_BACKSPACE) {
                UINTN len = StrLen(st->inner.query);
                if (len > 0) { st->inner.query[len - 1] = 0; MenuRefilterInner(st); }
            } else if (Key.UnicodeChar == CHAR_CARRIAGE_RETURN) {
                if (st->inner.filteredCount > 0) {
                    CONST PREFILLED_ENTRY *e = AllEntries[st->inner.filtered[st->inner.sel]];
                    ScreenEditValue(e->StoreName, e->Offset, e->Size, e->Desc, e->Source);
                }
            } else if (Key.UnicodeChar >= 0x20 && Key.UnicodeChar < 0x7F) {
                UINTN len = StrLen(st->inner.query);
                if (len + 1 < 64) {
                    st->inner.query[len] = Key.UnicodeChar;
                    st->inner.query[len + 1] = 0;
                    MenuRefilterInner(st);
                }
            }
        }
    }
}

/* ==================== reusable text input box ==================== */

static BOOLEAN GfxTextInput(INTN x, INTN y, INTN w, INTN h, CHAR16 *buf, UINTN bufCap,
                             CONST CHAR16 *placeholder, BOOLEAN hexOnly)
{
    UINTN len = StrLen(buf);
    EFI_INPUT_KEY Key;
    for (;;) {
        GfxFillRoundedRect(x, y, w, h, 10, COL_CARD);
        GfxStrokeRect(x, y, w, h, 1, COL_ACCENT);
        if (len) {
            GfxDrawTextVCenter(&FONT_BODY, x + 18, y, h, buf, COL_TEXT);
            {
                UINTN tw = GfxTextWidth(&FONT_BODY, buf);
                GfxFillRect(x + 18 + tw + 2, y + 12, 2, h - 24, COL_ACCENT);
            }
        } else if (placeholder) {
            GfxDrawTextVCenter(&FONT_BODY, x + 18, y, h, placeholder, COL_TEXT_DIM);
        }
        GfxPresent();

        Key = WaitKey();
        if (Key.ScanCode == SCAN_ESC) return FALSE;
        if (Key.UnicodeChar == CHAR_CARRIAGE_RETURN) return TRUE;
        if (Key.UnicodeChar == CHAR_BACKSPACE) { if (len) { len--; buf[len] = 0; } continue; }
        if (Key.UnicodeChar == 0) continue;
        {
            CHAR16 c = Key.UnicodeChar;
            BOOLEAN ok = TRUE;
            if (hexOnly) {
                ok = (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') ||
                     (c >= L'A' && c <= L'F') || (c == L'x') || (c == L'X');
            } else {
                ok = (c >= 0x20 && c < 0x7F);
            }
            if (ok && len + 1 < bufCap) { buf[len++] = c; buf[len] = 0; }
        }
    }
}

/* ==================== More tab: manual edit ==================== */

static VOID ScreenEditByOffsetManual(VOID)
{
    CHAR16 storeBuf[64] = L"Setup";
    CHAR16 offBuf[32] = L"";
    CHAR16 sizeBuf[8] = L"1";
    UINTN sz;

    GfxClear(COL_BG);
    DrawTopBar(L"Manual Edit");
    GfxDrawText(&FONT_BODY, MARGIN, 120, L"Variable store name:", COL_TEXT);
    DrawFooterHint(L"Enter Next    Esc Cancel");
    if (!GfxTextInput(MARGIN, 156, 400, 52, storeBuf, 64, L"Setup", FALSE)) return;
    if (storeBuf[0] == 0) StrCpy(storeBuf, L"Setup");

    GfxClear(COL_BG);
    DrawTopBar(L"Manual Edit");
    GfxDrawText(&FONT_SMALL, MARGIN, 120, L"Store:", COL_TEXT_DIM);
    GfxDrawText(&FONT_BODY, MARGIN + 70, 116, storeBuf, COL_TEXT);
    GfxDrawText(&FONT_BODY, MARGIN, 176, L"Offset (hex, e.g. 0x1A):", COL_TEXT);
    DrawFooterHint(L"Enter Next    Esc Cancel");
    if (!GfxTextInput(MARGIN, 212, 300, 52, offBuf, 32, L"0x00", TRUE) || offBuf[0] == 0) return;

    GfxClear(COL_BG);
    DrawTopBar(L"Manual Edit");
    GfxDrawText(&FONT_SMALL, MARGIN, 120, L"Store:", COL_TEXT_DIM);
    GfxDrawText(&FONT_BODY, MARGIN + 70, 116, storeBuf, COL_TEXT);
    {
        CHAR16 line[64];
        SPrint(line, sizeof(line), L"Offset: 0x%x", ParseHex(offBuf));
        GfxDrawText(&FONT_BODY, MARGIN, 176, line, COL_TEXT);
    }
    GfxDrawText(&FONT_BODY, MARGIN, 232, L"Size in bytes (1, 2 or 4):", COL_TEXT);
    DrawFooterHint(L"Enter Confirm    Esc Cancel (defaults to 1)");
    GfxTextInput(MARGIN, 268, 160, 52, sizeBuf, 8, L"1", TRUE);

    sz = ParseHex(sizeBuf);
    if (sz != 1 && sz != 2 && sz != 4) sz = 1;
    ScreenEditValue(storeBuf, ParseHex(offBuf), sz, NULL, NULL);
}

/* ==================== More tab: browse all NVRAM variables ==================== */

static VOID ScreenEditByOffsetManualPrefillStore(CONST CHAR16 *storeName)
{
    CHAR16 offBuf[32] = L"";
    CHAR16 sizeBuf[8] = L"1";
    UINTN sz;

    GfxClear(COL_BG);
    DrawTopBar(L"Manual Edit");
    GfxDrawText(&FONT_SMALL, MARGIN, 120, L"Store:", COL_TEXT_DIM);
    GfxDrawTextClipped(&FONT_BODY, MARGIN + 70, 116, gScreenW - MARGIN * 2 - 70, storeName, COL_TEXT);
    GfxDrawText(&FONT_BODY, MARGIN, 176, L"Offset (hex, e.g. 0x1A):", COL_TEXT);
    DrawFooterHint(L"Enter Next    Esc Cancel");
    if (!GfxTextInput(MARGIN, 212, 300, 52, offBuf, 32, L"0x00", TRUE) || offBuf[0] == 0) return;

    GfxClear(COL_BG);
    DrawTopBar(L"Manual Edit");
    GfxDrawText(&FONT_SMALL, MARGIN, 120, L"Store:", COL_TEXT_DIM);
    GfxDrawTextClipped(&FONT_BODY, MARGIN + 70, 116, gScreenW - MARGIN * 2 - 70, storeName, COL_TEXT);
    {
        CHAR16 line[64];
        SPrint(line, sizeof(line), L"Offset: 0x%x", ParseHex(offBuf));
        GfxDrawText(&FONT_BODY, MARGIN, 176, line, COL_TEXT);
    }
    GfxDrawText(&FONT_BODY, MARGIN, 232, L"Size in bytes (1, 2 or 4):", COL_TEXT);
    DrawFooterHint(L"Enter Confirm    Esc Cancel (defaults to 1)");
    GfxTextInput(MARGIN, 268, 160, 52, sizeBuf, 8, L"1", TRUE);

    sz = ParseHex(sizeBuf);
    if (sz != 1 && sz != 2 && sz != 4) sz = 1;
    ScreenEditValue(storeName, ParseHex(offBuf), sz, NULL, NULL);
}

#define MAX_VARNAME 512
#define MAX_VARLIST 2048

typedef struct {
    CHAR16 Name[MAX_VARNAME];
    EFI_GUID Guid;
} VAR_ENTRY;

static UINTN EnumerateVariables(VAR_ENTRY *arr, UINTN maxCount)
{
    EFI_STATUS Status;
    CHAR16 *VarName;
    EFI_GUID VendorGuid;
    UINTN count = 0;

    VarName = AllocateZeroPool(MAX_VARNAME * sizeof(CHAR16));
    if (!VarName) return 0;
    VarName[0] = 0;
    ZeroMem(&VendorGuid, sizeof(VendorGuid));

    for (;;) {
        UINTN Size = MAX_VARNAME * sizeof(CHAR16);
        Status = uefi_call_wrapper(RT->GetNextVariableName, 3, &Size, VarName, &VendorGuid);
        if (Status == EFI_NOT_FOUND) break;
        if (Status == EFI_BUFFER_TOO_SMALL) {
            FreePool(VarName);
            VarName = AllocateZeroPool(Size);
            if (!VarName) break;
            Status = uefi_call_wrapper(RT->GetNextVariableName, 3, &Size, VarName, &VendorGuid);
        }
        if (EFI_ERROR(Status)) break;

        if (count < maxCount) {
            StrCpy(arr[count].Name, VarName);
            CopyMem(&arr[count].Guid, &VendorGuid, sizeof(EFI_GUID));
            count++;
        } else break;
    }
    FreePool(VarName);
    return count;
}

static VOID ScreenBrowseNvram(VOID)
{
    VAR_ENTRY *vars;
    UINTN count, sel = 0, top = 0;
    UINTN visible = VisibleCardCount();
    EFI_INPUT_KEY Key;

    vars = AllocateZeroPool(sizeof(VAR_ENTRY) * MAX_VARLIST);
    if (!vars) return;

    GfxClear(COL_BG);
    DrawTopBar(L"Browse NVRAM Variables");
    GfxDrawText(&FONT_BODY, MARGIN, 120, L"Enumerating NVRAM variables...", COL_TEXT_DIM);
    GfxPresent();

    count = EnumerateVariables(vars, MAX_VARLIST);

    if (count == 0) {
        GfxClear(COL_BG);
        DrawTopBar(L"Browse NVRAM Variables");
        GfxDrawText(&FONT_BODY, MARGIN, 120, L"No variables enumerated (or access denied).", COL_WARN);
        DrawFooterHint(L"Press any key to go back");
        GfxPresent();
        WaitKey();
        FreePool(vars);
        return;
    }

    for (;;) {
        UINTN i;
        CHAR16 line[64];

        GfxClear(COL_BG);
        DrawTopBar(L"Browse NVRAM Variables");
        SPrint(line, sizeof(line), L"%d variables on this machine", count);
        GfxDrawText(&FONT_SMALL, MARGIN, 110, line, COL_TEXT_DIM);

        if (sel < top) top = sel;
        if (sel >= top + visible) top = sel - visible + 1;

        for (i = 0; i < visible && (top + i) < count; i++) {
            UINTN idx = top + i;
            DrawListCard(SEARCH_Y + (INTN)i * (CARD_H + CARD_GAP), vars[idx].Name, NULL,
                         (top + i) == sel);
        }

        DrawFooterHint(L"Up/Down Move    Enter Edit    Esc Back");
        GfxPresent();

        Key = WaitKey();
        if (Key.ScanCode == SCAN_ESC) break;
        else if (Key.ScanCode == SCAN_UP) { if (sel > 0) sel--; }
        else if (Key.ScanCode == SCAN_DOWN) { if (sel + 1 < count) sel++; }
        else if (Key.ScanCode == SCAN_PAGE_UP) { sel = (sel > visible) ? sel - visible : 0; }
        else if (Key.ScanCode == SCAN_PAGE_DOWN) {
            sel = (sel + visible < count) ? sel + visible : count - 1;
        } else if (Key.UnicodeChar == CHAR_CARRIAGE_RETURN) {
            ScreenEditByOffsetManualPrefillStore(vars[sel].Name);
        }
    }

    FreePool(vars);
}

/* ==================== More tab: batch config file loader ==================== */

#define MAX_CFG_ENTRIES 256
#define MAX_LINE 256

typedef struct {
    CHAR16 StoreName[64];
    UINTN Offset;
    UINTN Size;
    UINT32 Value;
    CHAR16 Desc[128];
} CFG_ENTRY;

static EFI_FILE_HANDLE OpenAppDirRoot(EFI_HANDLE ImageHandle)
{
    EFI_STATUS Status;
    EFI_LOADED_IMAGE *LoadedImage = NULL;
    EFI_FILE_IO_INTERFACE *FioIface = NULL;
    EFI_FILE_HANDLE Root = NULL;

    Status = uefi_call_wrapper(BS->HandleProtocol, 3, ImageHandle,
                                &LoadedImageProtocol, (VOID **)&LoadedImage);
    if (EFI_ERROR(Status) || !LoadedImage) return NULL;

    Status = uefi_call_wrapper(BS->HandleProtocol, 3, LoadedImage->DeviceHandle,
                                &FileSystemProtocol, (VOID **)&FioIface);
    if (EFI_ERROR(Status) || !FioIface) return NULL;

    Status = uefi_call_wrapper(FioIface->OpenVolume, 2, FioIface, &Root);
    if (EFI_ERROR(Status)) return NULL;
    return Root;
}

static UINTN ParseConfigText(CHAR8 *text, UINTN textLen, CFG_ENTRY *out, UINTN maxOut)
{
    UINTN pos = 0, count = 0;

    while (pos < textLen && count < maxOut) {
        CHAR8 line[MAX_LINE];
        UINTN li = 0;
        UINTN i, j;
        CHAR16 store[64];
        UINTN si = 0;

        while (pos < textLen && text[pos] != '\n' && li < MAX_LINE - 1) {
            if (text[pos] != '\r') line[li++] = text[pos];
            pos++;
        }
        while (pos < textLen && text[pos] != '\n') pos++;
        if (pos < textLen) pos++;
        line[li] = 0;

        if (li == 0 || line[0] == ';' || line[0] == '#') continue;

        i = 0;
        while (line[i] && line[i] != ':' && si < 63) store[si++] = (CHAR16)line[i++];
        store[si] = 0;
        if (line[i] != ':') continue;
        i++;

        if (line[i] != '0' || (line[i + 1] != 'x' && line[i + 1] != 'X')) continue;
        i += 2;
        {
            UINTN off = 0;
            while ((line[i] >= '0' && line[i] <= '9') || (line[i] >= 'a' && line[i] <= 'f') ||
                   (line[i] >= 'A' && line[i] <= 'F')) {
                CHAR8 c = line[i];
                UINTN d = (c >= '0' && c <= '9') ? (UINTN)(c - '0') :
                          (c >= 'a' && c <= 'f') ? (UINTN)(c - 'a' + 10) : (UINTN)(c - 'A' + 10);
                off = (off << 4) | d;
                i++;
            }

            {
                UINTN sz = 1;
                if (line[i] == '(') {
                    i++;
                    sz = 0;
                    while (line[i] >= '0' && line[i] <= '9') { sz = sz * 10 + (UINTN)(line[i] - '0'); i++; }
                    if (line[i] == ')') i++;
                }

                if (line[i] != '=') continue;
                i++;
                if (line[i] != '0' || (line[i + 1] != 'x' && line[i + 1] != 'X')) continue;
                i += 2;
                {
                    UINT32 val = 0;
                    CHAR16 desc[128];
                    UINTN di = 0;
                    while ((line[i] >= '0' && line[i] <= '9') || (line[i] >= 'a' && line[i] <= 'f') ||
                           (line[i] >= 'A' && line[i] <= 'F')) {
                        CHAR8 c = line[i];
                        UINTN d = (c >= '0' && c <= '9') ? (UINTN)(c - '0') :
                                  (c >= 'a' && c <= 'f') ? (UINTN)(c - 'a' + 10) : (UINTN)(c - 'A' + 10);
                        val = (val << 4) | (UINT32)d;
                        i++;
                    }

                    desc[0] = 0;
                    while (line[i] && line[i] != ';') i++;
                    if (line[i] == ';') {
                        i++;
                        while (line[i] == ' ') i++;
                        while (line[i] && di < 127) desc[di++] = (CHAR16)line[i++];
                    }
                    desc[di] = 0;

                    for (j = 0; j < si; j++) out[count].StoreName[j] = store[j];
                    out[count].StoreName[si] = 0;
                    out[count].Offset = off;
                    out[count].Size = (sz == 2 || sz == 4) ? sz : 1;
                    out[count].Value = val;
                    StrCpy(out[count].Desc, desc);
                    count++;
                }
            }
        }
    }
    return count;
}

static VOID ScreenBatchConfig(EFI_HANDLE ImageHandle)
{
    EFI_FILE_HANDLE Root, File;
    EFI_STATUS Status;
    CFG_ENTRY *entries;
    BOOLEAN *selected;
    UINTN count = 0;
    CHAR8 *buf;
    UINTN bufSize;
    EFI_FILE_INFO *info;
    UINTN infoSize;
    UINTN sel = 0;
    EFI_INPUT_KEY Key;
    UINTN visible = VisibleCardCount();

    GfxClear(COL_BG);
    DrawTopBar(L"Batch Config File");
    GfxDrawText(&FONT_BODY, MARGIN, 120, L"Looking for setupvar.cfg next to this .efi file...", COL_TEXT_DIM);
    GfxPresent();

    Root = OpenAppDirRoot(ImageHandle);
    if (!Root) {
        GfxClear(COL_BG); DrawTopBar(L"Batch Config File");
        GfxDrawText(&FONT_BODY, MARGIN, 120, L"Could not open filesystem root.", COL_WARN);
        DrawFooterHint(L"Press any key to go back");
        GfxPresent(); WaitKey();
        return;
    }

    Status = uefi_call_wrapper(Root->Open, 5, Root, &File, L"setupvar.cfg", EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(Status)) {
        GfxClear(COL_BG); DrawTopBar(L"Batch Config File");
        GfxDrawText(&FONT_BODY, MARGIN, 120, L"setupvar.cfg not found next to this app.", COL_WARN);
        GfxDrawText(&FONT_SMALL, MARGIN, 156, L"Format, one entry per line:", COL_TEXT_DIM);
        GfxDrawText(&FONT_SMALL, MARGIN, 180, L"Setup:0x0CA9(1)=0x01 ; Intel C-State", COL_TEXT_DIM);
        DrawFooterHint(L"Press any key to go back");
        GfxPresent(); WaitKey();
        return;
    }

    infoSize = SIZE_OF_EFI_FILE_INFO + 256;
    info = AllocateZeroPool(infoSize);
    uefi_call_wrapper(File->GetInfo, 4, File, &GenericFileInfo, &infoSize, info);
    bufSize = (UINTN)info->FileSize;
    FreePool(info);

    buf = AllocateZeroPool(bufSize + 1);
    uefi_call_wrapper(File->Read, 3, File, &bufSize, buf);
    uefi_call_wrapper(File->Close, 1, File);

    entries = AllocateZeroPool(sizeof(CFG_ENTRY) * MAX_CFG_ENTRIES);
    selected = AllocateZeroPool(sizeof(BOOLEAN) * MAX_CFG_ENTRIES);
    count = ParseConfigText(buf, bufSize, entries, MAX_CFG_ENTRIES);
    FreePool(buf);

    if (count == 0) {
        GfxClear(COL_BG); DrawTopBar(L"Batch Config File");
        GfxDrawText(&FONT_BODY, MARGIN, 120, L"No valid entries parsed from setupvar.cfg.", COL_WARN);
        DrawFooterHint(L"Press any key to go back");
        GfxPresent(); WaitKey();
        FreePool(entries); FreePool(selected);
        return;
    }

    for (;;) {
        UINTN i;
        CHAR16 countLine[64];

        GfxClear(COL_BG);
        DrawTopBar(L"Batch Config File");
        SPrint(countLine, sizeof(countLine), L"%d entries", count);
        GfxDrawText(&FONT_SMALL, MARGIN, 110, countLine, COL_TEXT_DIM);

        for (i = 0; i < visible && i < count; i++) {
            CHAR16 title[160], sub[96];
            SPrint(title, sizeof(title), L"[%s] %s", selected[i] ? L"x" : L" ", entries[i].Desc);
            SPrint(sub, sizeof(sub), L"%s : 0x%x(%d) = 0x%x",
                   entries[i].StoreName, entries[i].Offset, entries[i].Size, entries[i].Value);
            DrawListCard(SEARCH_Y + (INTN)i * (CARD_H + CARD_GAP), title, sub, i == sel);
        }

        DrawFooterHint(L"Up/Down Move    Space Toggle    A Apply checked    Esc Back");
        GfxPresent();

        Key = WaitKey();
        if (Key.ScanCode == SCAN_ESC) break;
        else if (Key.ScanCode == SCAN_UP) { if (sel > 0) sel--; }
        else if (Key.ScanCode == SCAN_DOWN) { if (sel + 1 < count) sel++; }
        else if (Key.UnicodeChar == L' ') selected[sel] = !selected[sel];
        else if (Key.UnicodeChar == L'a' || Key.UnicodeChar == L'A') {
            UINTN n; BOOLEAN any = FALSE;
            for (n = 0; n < count; n++) if (selected[n]) any = TRUE;
            if (!any) continue;

            {
                INTN mw = 640, mh = 160;
                INTN mx = (gScreenW - mw) / 2, my = (gScreenH - mh) / 2;
                EFI_INPUT_KEY ck;
                GfxClear(RGB(0x33, 0x34, 0x38));
                GfxFillRoundedRect(mx, my, mw, mh, 16, COL_CARD);
                GfxStrokeRect(mx, my, mw, mh, 1, COL_BORDER);
                GfxDrawText(&FONT_HEADING, mx + 28, my + 20, L"Apply checked entries?", COL_TEXT);
                GfxDrawText(&FONT_SMALL, mx + 28, my + 64, L"This writes NVRAM now.", COL_WARN);
                GfxDrawTextVCenter(&FONT_BODY, mx + 28, my + mh - 48, 36,
                                    L"Enter = Apply        Esc = Cancel", COL_TEXT_DIM);
                GfxPresent();
                ck = WaitKey();
                if (ck.UnicodeChar != CHAR_CARRIAGE_RETURN) continue;
            }

            for (n = 0; n < count; n++) {
                EFI_GUID guid;
                VOID *data = NULL;
                UINTN dsize = 0;
                UINT32 attr = 0;
                if (!selected[n]) continue;
                if (EFI_ERROR(FindVariableGuid(entries[n].StoreName, &guid))) continue;
                if (EFI_ERROR(ReadVarBuffer(entries[n].StoreName, &guid, &data, &dsize, &attr))) continue;
                if (entries[n].Offset + entries[n].Size <= dsize) {
                    CopyMem((UINT8 *)data + entries[n].Offset, &entries[n].Value, entries[n].Size);
                    uefi_call_wrapper(RT->SetVariable, 5, entries[n].StoreName, &guid, attr, dsize, data);
                }
                FreePool(data);
            }

            GfxClear(COL_BG); DrawTopBar(L"Batch Config File");
            GfxFillRoundedRect(MARGIN, 140, gScreenW - MARGIN * 2, 80, 14, COL_OK_BG);
            GfxDrawTextVCenter(&FONT_BODY, MARGIN + 24, 140, 80, L"Batch apply finished.", COL_OK);
            DrawFooterHint(L"Press any key to continue");
            GfxPresent(); WaitKey();
        }
    }

    FreePool(entries);
    FreePool(selected);
}

/* ==================== More tab: about ==================== */

static VOID ScreenAbout(VOID)
{
    GfxClear(COL_BG);
    DrawTopBar(L"About");

    GfxDrawText(&FONT_BODY, MARGIN, 120,
                L"SetupVar GUI reads and writes raw bytes inside firmware NVRAM", COL_TEXT);
    GfxDrawText(&FONT_BODY, MARGIN, 148,
                L"\"Setup\"-style variables - the same variables the BIOS setup", COL_TEXT);
    GfxDrawText(&FONT_BODY, MARGIN, 176,
                L"menu itself edits. This lets you toggle hidden/greyed-out settings.", COL_TEXT);

    GfxFillRoundedRect(MARGIN, 220, gScreenW - MARGIN * 2, 100, 14, COL_WARN_BG);
    GfxDrawText(&FONT_BODY, MARGIN + 24, 236,
                L"Wrong offsets/values can permanently brick your board.", COL_WARN);
    GfxDrawText(&FONT_BODY, MARGIN + 24, 264,
                L"Only use offsets verified for your EXACT firmware version.", COL_WARN);
    GfxDrawText(&FONT_BODY, MARGIN + 24, 292,
                L"(e.g. via IFR extraction.) Use at your own risk.", COL_WARN);

    GfxDrawText(&FONT_SMALL, MARGIN, 356, L"Underlying operations:", COL_TEXT_DIM);
    GfxDrawText(&FONT_SMALL, MARGIN, 382,
                L"Read  : GetVariable(name, guid) -> raw byte buffer", COL_TEXT_DIM);
    GfxDrawText(&FONT_SMALL, MARGIN, 406,
                L"Write : patch byte/word/dword at offset -> SetVariable(...)", COL_TEXT_DIM);

    DrawFooterHint(L"Press any key to go back");
    GfxPresent();
    WaitKey();
}

/* ==================== More tab: action list ==================== */

static CONST CHAR16 *MoreItems[] = {
    L"Edit variable by name / offset (manual)",
    L"Browse all NVRAM variables",
    L"Load & apply config file (batch)",
    L"About / disclaimer",
    L"Exit",
};
#define MORE_COUNT 5
static CONST CHAR16 *MoreSubs[] = {
    L"Type in a store name, offset and size yourself",
    L"See every NVRAM variable on this machine",
    L"Apply a checklist of offset=value pairs from setupvar.cfg",
    L"What this tool does, and the risks",
    L"Quit SetupVar GUI",
};

/* returns TRUE if Tab was pressed, FALSE if Exit was chosen */
static BOOLEAN RunMoreTab(EFI_HANDLE ImageHandle, UINTN *sel)
{
    EFI_INPUT_KEY Key;

    for (;;) {
        UINTN i;

        GfxClear(COL_BG);
        DrawTopBar(NULL);
        DrawTabBar(TAB_MORE);

        for (i = 0; i < MORE_COUNT; i++) {
            DrawListCard(SEARCH_Y + (INTN)i * (CARD_H + CARD_GAP), MoreItems[i], MoreSubs[i],
                         i == *sel);
        }

        DrawFooterHint(L"Up/Down Move    Enter Select    Tab Switch mode");
        GfxPresent();

        Key = WaitKey();
        if (Key.UnicodeChar == 0x09) return TRUE;
        if (Key.ScanCode == SCAN_UP) { if (*sel > 0) (*sel)--; }
        else if (Key.ScanCode == SCAN_DOWN) { if (*sel + 1 < MORE_COUNT) (*sel)++; }
        else if (Key.UnicodeChar == CHAR_CARRIAGE_RETURN) {
            switch (*sel) {
                case 0: ScreenEditByOffsetManual(); break;
                case 1: ScreenBrowseNvram(); break;
                case 2: ScreenBatchConfig(ImageHandle); break;
                case 3: ScreenAbout(); break;
                case 4: return FALSE; /* signal exit */
            }
        }
    }
}

/* ==================== splash / disclaimer ==================== */

static BOOLEAN ScreenSplash(VOID)
{
    INTN mw = 760, mh = 320;
    INTN mx = (gScreenW - mw) / 2, my = (gScreenH - mh) / 2;
    EFI_INPUT_KEY Key;

    GfxClear(COL_BG);
    GfxFillRoundedRect(mx, my, mw, mh, 20, COL_CARD);
    GfxStrokeRect(mx, my, mw, mh, 1, COL_BORDER);

    GfxDrawText(&FONT_TITLE, mx + 36, my + 30, L"SetupVar GUI", COL_TEXT);

    GfxFillRoundedRect(mx + 36, my + 92, mw - 72, 90, 12, COL_WARN_BG);
    GfxDrawText(&FONT_BODY, mx + 56, my + 108,
                L"This tool edits raw firmware NVRAM (hidden BIOS settings).", COL_WARN);
    GfxDrawText(&FONT_BODY, mx + 56, my + 138,
                L"Incorrect use CAN PERMANENTLY BRICK your device.", COL_WARN);

    GfxDrawText(&FONT_BODY, mx + 36, my + 204,
                L"Proceed only if you understand the risk and know what", COL_TEXT);
    GfxDrawText(&FONT_BODY, mx + 36, my + 230,
                L"you are doing.", COL_TEXT);

    GfxDrawTextVCenter(&FONT_BODY, mx + 36, my + mh - 56, 40,
                        L"Enter = Continue        Esc = Exit", COL_TEXT_DIM);
    GfxPresent();

    Key = WaitKey();
    return (Key.UnicodeChar == CHAR_CARRIAGE_RETURN);
}

/* ==================== entry point ==================== */

EFI_STATUS EFIAPI efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
    TAB_ID tab = TAB_AIO;
    LIST_STATE aioState;
    MENU_STATE menuState;
    UINTN moreSel = 0;

    InitializeLib(ImageHandle, SystemTable);

    if (EFI_ERROR(GfxInit())) {
        Print(L"This firmware does not expose a Graphics Output Protocol;\r\n");
        Print(L"SetupVar GUI needs GOP to render its graphical interface.\r\n");
        return EFI_UNSUPPORTED;
    }

    BuildAllEntries();

    ZeroMem(&aioState, sizeof(aioState));
    ZeroMem(&menuState, sizeof(menuState));

    if (!ScreenSplash()) return EFI_SUCCESS;

    for (;;) {
        switch (tab) {
            case TAB_AIO:
                if (RunAioTab(&aioState)) tab = TAB_MENU;
                break;
            case TAB_MENU:
                if (RunMenuTab(&menuState)) tab = TAB_MORE;
                break;
            case TAB_MORE:
                if (RunMoreTab(ImageHandle, &moreSel)) tab = TAB_AIO;
                else { GfxClear(COL_BG); GfxPresent(); return EFI_SUCCESS; }
                break;
        }
    }
}
