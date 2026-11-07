/* (hookshot, Linux) The GDI the Insert menu draws its text with (gpu/vk/vk_menu.cpp): 32-bit top-down bitmaps, solid
   fills and lines of text, rendered by FreeType (libfreetype.so.6, loaded when the menu first draws). The font is the
   system's sans-serif at the weight asked (fontconfig's fc-match), or one of the usual files. Without FreeType or a
   font the menu still opens, without its letters (logged once). */
#define _GNU_SOURCE
#include "windows.h"
#include <dlfcn.h>
#include <pthread.h>
#include <ft2build.h>
#include FT_FREETYPE_H

typedef struct { uint32_t kind; } GdiObj;
enum { G_DC = 0x47444331, G_BITMAP, G_FONT, G_BRUSH };
typedef struct {
    GdiObj o;
    int w, h;
    uint8_t *px;   /* B, G, R, X */
} Bitmap;
typedef struct {
    GdiObj o;
    int px, weight, mono;
} Font;
typedef struct {
    GdiObj o;
    COLORREF c;
} Brush;
typedef struct {
    GdiObj o;
    Bitmap *bmp;
    Font *font;
    COLORREF text;
    int bk;
    UINT align;
} Dc;

/* ---- FreeType, from the system ---- */
static struct {
    FT_Error (*Init)(FT_Library *);
    FT_Error (*NewFace)(FT_Library, const char *, FT_Long, FT_Face *);
    FT_Error (*SetPixelSizes)(FT_Face, FT_UInt, FT_UInt);
    FT_Error (*LoadChar)(FT_Face, FT_ULong, FT_Int32);
} ft;
static FT_Library lib;
static FT_Face faces[2];   /* regular, bold */
static pthread_mutex_t ft_mu = PTHREAD_MUTEX_INITIALIZER;
static int ft_state;   /* 0 not tried, 1 ready, -1 none */

static int font_file(int bold, char *out, size_t cap) {
    char cmd[128];
    snprintf(cmd, sizeof cmd, "fc-match -f '%%{file}' 'sans-serif:%s' 2>/dev/null", bold ? "bold" : "regular");
    FILE *p = popen(cmd, "r");
    if (p) {
        const int ok = fgets(out, (int)cap, p) != NULL;
        pclose(p);
        if (ok && out[0] == '/') return 1;
    }
    static const char *const regular[] = {"/usr/share/fonts/noto/NotoSans-Regular.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf",
                                          "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/dejavu/DejaVuSans.ttf",
                                          "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf"};
    static const char *const boldf[] = {"/usr/share/fonts/noto/NotoSans-Bold.ttf", "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
                                        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", "/usr/share/fonts/dejavu/DejaVuSans-Bold.ttf",
                                        "/usr/share/fonts/truetype/noto/NotoSans-Bold.ttf"};
    const char *const *list = bold ? boldf : regular;
    for (int i = 0; i < 5; i++)
        if (access(list[i], R_OK) == 0) {
            snprintf(out, cap, "%s", list[i]);
            return 1;
        }
    return 0;
}
static int ft_ready(void) {
    if (ft_state) return ft_state > 0;
    ft_state = -1;
    void *m = dlopen("libfreetype.so.6", RTLD_NOW | RTLD_LOCAL);
    if (!m) {
        fprintf(stderr, "[gdi] no libfreetype.so.6: the menu has no text\n");
        return 0;
    }
    *(void **)&ft.Init = dlsym(m, "FT_Init_FreeType");
    *(void **)&ft.NewFace = dlsym(m, "FT_New_Face");
    *(void **)&ft.SetPixelSizes = dlsym(m, "FT_Set_Pixel_Sizes");
    *(void **)&ft.LoadChar = dlsym(m, "FT_Load_Char");
    if (!ft.Init || !ft.NewFace || !ft.SetPixelSizes || !ft.LoadChar || ft.Init(&lib)) return 0;
    for (int b = 0; b < 2; b++) {
        char path[1024];
        if (!font_file(b, path, sizeof path) || ft.NewFace(lib, path, 0, &faces[b])) faces[b] = NULL;
    }
    if (!faces[0]) faces[0] = faces[1];
    if (!faces[1]) faces[1] = faces[0];
    if (!faces[0]) {
        fprintf(stderr, "[gdi] no sans-serif font found: the menu has no text\n");
        return 0;
    }
    ft_state = 1;
    return 1;
}
static FT_Face face_for(const Font *f) {
    FT_Face face = faces[f->weight >= FW_SEMIBOLD];
    ft.SetPixelSizes(face, 0, (FT_UInt)(f->px > 0 ? f->px : 16));
    return face;
}

/* ---- objects ---- */
HDC w32_CreateCompatibleDC(HDC dc) {
    (void)dc;
    Dc *d = (Dc *)calloc(1, sizeof *d);
    d->o.kind = G_DC;
    d->bk = OPAQUE;
    return d;
}
HBITMAP w32_CreateDIBSection(HDC dc, const BITMAPINFO *bi, UINT usage, void **bits, HANDLE section, DWORD offset) {
    (void)dc, (void)usage, (void)section, (void)offset;
    const int w = bi->bmiHeader.biWidth, h = bi->bmiHeader.biHeight < 0 ? -bi->bmiHeader.biHeight : bi->bmiHeader.biHeight;
    if (w <= 0 || h <= 0 || bi->bmiHeader.biBitCount != 32) return NULL;
    Bitmap *b = (Bitmap *)calloc(1, sizeof *b);
    b->o.kind = G_BITMAP;
    b->w = w, b->h = h;
    b->px = (uint8_t *)calloc((size_t)w * h, 4);
    if (bits) *bits = b->px;
    return b;
}
HFONT w32_CreateFontA(int height, int width, int esc, int orient, int weight, DWORD italic, DWORD underline, DWORD strike,
                      DWORD charset, DWORD out_prec, DWORD clip_prec, DWORD quality, DWORD pitch, LPCSTR face) {
    (void)width, (void)esc, (void)orient, (void)italic, (void)underline, (void)strike, (void)charset, (void)out_prec,
        (void)clip_prec, (void)pitch, (void)face;
    Font *f = (Font *)calloc(1, sizeof *f);
    f->o.kind = G_FONT;
    f->px = height < 0 ? -height : height;
    f->weight = weight;
    f->mono = quality == NONANTIALIASED_QUALITY;
    return f;
}
HBRUSH w32_CreateSolidBrush(COLORREF c) {
    Brush *b = (Brush *)calloc(1, sizeof *b);
    b->o.kind = G_BRUSH;
    b->c = c;
    return b;
}
HGDIOBJ w32_SelectObject(HDC dc, HGDIOBJ o) {
    Dc *d = (Dc *)dc;
    GdiObj *g = (GdiObj *)o;
    if (!d || !g) return NULL;
    if (g->kind == G_BITMAP) {
        void *old = d->bmp;
        d->bmp = (Bitmap *)g;
        return old;
    }
    if (g->kind == G_FONT) {
        void *old = d->font;
        d->font = (Font *)g;
        return old;
    }
    return NULL;
}
BOOL w32_DeleteObject(HGDIOBJ o) {
    GdiObj *g = (GdiObj *)o;
    if (!g) return FALSE;
    if (g->kind == G_BITMAP) free(((Bitmap *)g)->px);
    g->kind = 0;
    free(g);
    return TRUE;
}
BOOL w32_DeleteDC(HDC dc) {
    free(dc);
    return TRUE;
}
int w32_SetBkMode(HDC dc, int mode) {
    Dc *d = (Dc *)dc;
    const int old = d->bk;
    d->bk = mode;
    return old;
}
COLORREF w32_SetTextColor(HDC dc, COLORREF c) {
    Dc *d = (Dc *)dc;
    const COLORREF old = d->text;
    d->text = c;
    return old;
}
UINT w32_SetTextAlign(HDC dc, UINT align) {
    Dc *d = (Dc *)dc;
    const UINT old = d->align;
    d->align = align;
    return old;
}

/* ---- drawing ---- */
int w32_FillRect(HDC dc, const RECT *r, HBRUSH br) {
    Dc *d = (Dc *)dc;
    const Brush *b = (const Brush *)br;
    if (!d || !d->bmp || !b) return 0;
    Bitmap *bm = d->bmp;
    const int x0 = r->left < 0 ? 0 : r->left, y0 = r->top < 0 ? 0 : r->top;
    const int x1 = r->right > bm->w ? bm->w : r->right, y1 = r->bottom > bm->h ? bm->h : r->bottom;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            uint8_t *p = bm->px + ((size_t)y * bm->w + x) * 4;
            p[0] = GetBValue(b->c), p[1] = GetGValue(b->c), p[2] = GetRValue(b->c), p[3] = 0;
        }
    return 1;
}
/* the advance of a line of text, and its face's ascent and height (pixels) */
static int measure(FT_Face face, int mono, const char *s, int n, int *ascent, int *height) {
    int w = 0;
    for (int i = 0; i < n; i++)
        if (!ft.LoadChar(face, (unsigned char)s[i], FT_LOAD_DEFAULT | (mono ? FT_LOAD_TARGET_MONO : 0)))
            w += (int)(face->glyph->advance.x >> 6);
    if (ascent) *ascent = (int)(face->size->metrics.ascender >> 6);
    if (height) *height = (int)((face->size->metrics.ascender - face->size->metrics.descender) >> 6);
    return w;
}
BOOL w32_GetTextExtentPoint32A(HDC dc, LPCSTR s, int n, SIZE *out) {
    Dc *d = (Dc *)dc;
    out->cx = out->cy = 0;
    if (!d || !d->font) return FALSE;
    pthread_mutex_lock(&ft_mu);
    if (ft_ready()) {
        int h = 0;
        out->cx = measure(face_for(d->font), d->font->mono, s, n, NULL, &h);
        out->cy = h;
    }
    pthread_mutex_unlock(&ft_mu);
    return TRUE;
}
BOOL w32_TextOutA(HDC dc, int x, int y, LPCSTR s, int n) {
    Dc *d = (Dc *)dc;
    if (!d || !d->bmp || !d->font) return FALSE;
    pthread_mutex_lock(&ft_mu);
    if (!ft_ready()) {
        pthread_mutex_unlock(&ft_mu);
        return FALSE;
    }
    FT_Face face = face_for(d->font);
    const int mono = d->font->mono;
    int ascent = 0, height = 0;
    const int width = measure(face, mono, s, n, &ascent, &height);
    if ((d->align & TA_CENTER) == TA_CENTER) x -= width / 2;
    else if (d->align & TA_RIGHT) x -= width;
    int base = y + ascent;
    if ((d->align & TA_BASELINE) == TA_BASELINE) base = y;
    else if (d->align & TA_BOTTOM) base = y - height + ascent;
    Bitmap *bm = d->bmp;
    const uint8_t tr = GetRValue(d->text), tg = GetGValue(d->text), tb = GetBValue(d->text);
    int pen = x;
    for (int i = 0; i < n; i++) {
        if (ft.LoadChar(face, (unsigned char)s[i], FT_LOAD_RENDER | (mono ? FT_LOAD_TARGET_MONO : 0))) continue;
        const FT_GlyphSlot g = face->glyph;
        const FT_Bitmap *gb = &g->bitmap;
        const int gx = pen + g->bitmap_left, gy = base - g->bitmap_top;
        for (unsigned row = 0; row < gb->rows; row++) {
            const int py = gy + (int)row;
            if (py < 0 || py >= bm->h) continue;
            for (unsigned col = 0; col < gb->width; col++) {
                const int px = gx + (int)col;
                if (px < 0 || px >= bm->w) continue;
                unsigned a;
                if (gb->pixel_mode == FT_PIXEL_MODE_MONO)
                    a = (gb->buffer[row * gb->pitch + (col >> 3)] >> (7 - (col & 7)) & 1) ? 255 : 0;
                else
                    a = gb->buffer[row * gb->pitch + col];
                if (!a) continue;
                uint8_t *p = bm->px + ((size_t)py * bm->w + px) * 4;
                p[0] = (uint8_t)((tb * a + p[0] * (255 - a)) / 255);
                p[1] = (uint8_t)((tg * a + p[1] * (255 - a)) / 255);
                p[2] = (uint8_t)((tr * a + p[2] * (255 - a)) / 255);
            }
        }
        pen += (int)(g->advance.x >> 6);
    }
    pthread_mutex_unlock(&ft_mu);
    return TRUE;
}
