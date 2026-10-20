/* Desktop tests for components/fw_text. Run: py tests/run_host_tests.py */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fw_text.h"

#define W 128
#define H 64
static unsigned char fb[H][W];
static int writes, checks;

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } ++checks; } while (0)

static void put(void *ctx, int x, int y, FwRgb c){
    (void)ctx;
    if (x < 0 || x >= W || y < 0 || y >= H) { fprintf(stderr, "out of bounds pixel %d,%d\n", x, y); exit(1); }
    fb[y][x] = (c.r || c.g || c.b) ? '#' : '.';
    ++writes;
}
static const FwSurface S = {NULL, W, H, put, NULL};
static const FwRgb WHITE = {255, 255, 255};

static void clear(void){ memset(fb, '.', sizeof fb); writes = 0; }

/* Compares rows [y, y+n) columns [x, x+len) with expected art ('#' lit, '.' dark). */
static int region_is(int x, int y, const char *const *rows, int n){
    for (int r = 0; r < n; ++r)
        for (int c = 0; rows[r][c]; ++c)
            if (fb[y + r][x + c] != rows[r][c]) {
                fprintf(stderr, "mismatch at %d,%d\n", x + c, y + r);
                for (int k = 0; k < n; ++k) fprintf(stderr, "  got %.*s  want %s\n", (int)strlen(rows[k]), &fb[y + k][x], rows[k]);
                return 0;
            }
    return 1;
}

static int glyph_of(const char *s){ const char *p = s; return fw_text_next_glyph(&p, NULL); }

int main(void){
    /* --- the font is the classic table: 'A' and '.' ---------------------- */
    static const char *const A[] = {
        "..#..", ".#.#.", "#...#", "#...#", "#####", "#...#", "#...#", ".....",
    };
    clear();
    CHECK(fw_text_draw(&S, 10, 5, "A", WHITE, 1) == 16);
    CHECK(region_is(10, 5, A, 8));
    CHECK(writes == 16);

    /* --- widths --------------------------------------------------------- */
    CHECK(fw_text_width("", 1) == 0);
    CHECK(fw_text_width("A", 1) == 5);
    CHECK(fw_text_width("CLT-PBI", 1) == 41);
    CHECK(fw_text_width("Charlotte Douglas", 1) == 101);   /* matches the stock photo */
    CHECK(fw_text_width("AB", 2) == 22);
    CHECK(fw_text_width("Zürich", 1) == fw_text_width("Zurich", 1));
    CHECK(fw_text_width(NULL, 1) == 0);

    /* --- scale 2 is pixel-doubled --------------------------------------- */
    clear();
    fw_text_draw(&S, 0, 0, "A", WHITE, 2);
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 10; ++x)
            CHECK(fb[y][x] == ((fw_font5x7['A' * 5 + x / 2] >> (y / 2)) & 1 ? '#' : '.'));

    /* --- clipping never writes outside, at any position ----------------- */
    for (int x = -20; x <= W + 5; x += 3)
        for (int y = -10; y <= H + 2; y += 3) { clear(); fw_text_draw(&S, x, y, "Wg|", WHITE, 2); }
    clear();
    fw_text_draw(&S, -3, -2, "A", WHITE, 1);                /* partially visible */
    CHECK(writes > 0 && writes < 16);

    /* --- UTF-8 -> glyphs, through the stock tables ------------------------ */
    CHECK(glyph_of("\xC3\xBC") == 0x81);  /* ü */
    CHECK(glyph_of("\xC3\xA9") == 0x82);  /* é */
    CHECK(glyph_of("\xC3\xB1") == 0xA4);  /* ñ */
    CHECK(glyph_of("\xC3\x87") == 0x80);  /* Ç */
    CHECK(glyph_of("\xC3\x9F") == 0xE1);  /* ß */
    CHECK(glyph_of("\xC2\xB0") == 0xF8);  /* ° */
    CHECK(glyph_of("\xC2\xA0") == 0xFF);  /* NBSP: stock maps it to font glyph 0xFF */
    CHECK(glyph_of("\xE2\x86\x92") == 0x1A);  /* → */
    CHECK(glyph_of("\x7F") == 0x7F && glyph_of("\x01") == 0x01);
    CHECK(glyph_of("\xC3\xA3") >= FW_GLYPH_EXTRA_FIRST);  /* ã: vendor's own glyph */
    CHECK(glyph_of("\xC5\x81") >= FW_GLYPH_EXTRA_FIRST);  /* Ł */
    CHECK(glyph_of("\xE2\x82\xAC") >= FW_GLYPH_EXTRA_FIRST);  /* € */
    CHECK(glyph_of("\xE2\x9C\x88") >= FW_GLYPH_EXTRA_FIRST);  /* ✈ */
    CHECK(glyph_of("\xC3\xA4") == 0x84);  /* ä stays a font glyph */
    CHECK(glyph_of("\xC4\x9F") == '?');   /* ğ: in neither table */
    CHECK(glyph_of("\xE2\x80\x99") == '?');   /* ’: stock has no punctuation folding */
    CHECK(glyph_of("\xE2\x80\xA6") == '?');   /* … */
    CHECK(glyph_of("\xE6\x9D\xB1") == '?');   /* CJK */
    CHECK(glyph_of("\xF0\x9F\x9B\xAB") == '?'); /* emoji */
    for (uint32_t cp = 0; cp < 0x80; ++cp) CHECK(fw_text_glyph_for(cp) == (int)cp);
    {
        int mapped = 0, extra = 0;
        for (uint32_t cp = 0x80; cp < 0x30000; ++cp) {
            int g = fw_text_glyph_for(cp);
            if (g >= FW_GLYPH_EXTRA_FIRST) ++extra;
            else if (g != '?') { CHECK(g < 256); ++mapped; }
        }
        CHECK(mapped == 134 && extra == 63);   /* exactly the vendor's two tables */
    }
    {   /* an extra glyph draws its rows: ã */
        static const char *const a_tilde[] = {
            ".#.##", "##.#.", ".##..", "...#.", ".###.", "#..#.", ".####", ".....",
        };
        clear();
        CHECK(fw_text_draw(&S, 3, 4, "\xC3\xA3", WHITE, 1) == 9);
        CHECK(region_is(3, 4, a_tilde, 8));
    }

    /* --- malformed UTF-8: decoded exactly as stock, never past the end --- */
    {
        static const struct { const char *s; size_t len; const char *glyphs; } bad[] = {
            {"\xFF", 1, "?"}, {"\x80\x80", 2, "??"}, {"\xC3", 1, "?"}, {"\xE2\x80", 2, "?"},
            {"\xC0\xAF", 2, "/"}, {"\xED\xA0\x80", 3, "?"}, {"\xF5\x80\x80\x80", 4, "?"},
            {"\xC3" "A", 2, "?A"}, {"\xF0\x9F\x9B", 3, "?"}, {"\xE2" "A", 2, "?"},
            {"\xE2\x80" "AB", 4, "??AB"},   /* bad third byte: the lead is one '?', the stray 80 another */
        };
        for (size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
            char buf[8] = {0}, got[8] = {0};
            const char *p = buf, *end = buf + bad[i].len;
            int n = 0, g;
            memcpy(buf, bad[i].s, bad[i].len);
            buf[bad[i].len] = 'Z';                     /* must never be consumed when end is given */
            while ((g = fw_text_next_glyph(&p, end)) >= 0 && n < 7) got[n++] = (char)g;
            if (strcmp(got, bad[i].glyphs)) fprintf(stderr, "case %u: got '%s'\n", (unsigned)i, got);
            CHECK(strcmp(got, bad[i].glyphs) == 0);
            CHECK(p <= end);
            p = buf; buf[bad[i].len] = 0; n = 0;      /* and the same when the text simply ends */
            memset(got, 0, sizeof got);
            while ((g = fw_text_next_glyph(&p, NULL)) >= 0 && n < 7) got[n++] = (char)g;
            CHECK(strcmp(got, bad[i].glyphs) == 0);
        }
    }

    /* --- fitting: ellipsis and clip -------------------------------------- */
    {
        bool cut;
        const char *name = "Sao Paulo-Guarulhos International";
        clear();
        int w = fw_text_draw_fit(&S, 0, 0, 128, "Palm Beach Intl", WHITE, 1, FW_TEXT_ELLIPSIS, &cut);
        CHECK(!cut && w == fw_text_width("Palm Beach Intl", 1));
        clear();
        w = fw_text_draw_fit(&S, 0, 0, 128, name, WHITE, 1, FW_TEXT_ELLIPSIS, &cut);
        /* 20 glyphs + ellipsis would fit, but glyph 20 is the space after
         * "Guarulhos", which is dropped: "Sao Paulo-Guarulhos…" = 20 cells. */
        CHECK(cut && w <= 128 && w == 20 * 6 - 1);
        for (int x = w; x < W; ++x) for (int y = 0; y < 8; ++y) CHECK(fb[y][x] == '.');
        /* ellipsis = dots at row 6 in cols 0,2,4 of the last cell (x 114..118) */
        CHECK(fb[6][114] == '#' && fb[6][115] == '.' && fb[6][116] == '#' && fb[6][118] == '#');
        for (int y = 0; y < 8; ++y) if (y != 6) CHECK(fb[y][114] == '.');
        clear();
        w = fw_text_draw_fit(&S, 0, 0, 40, "Palm Beach Intl", WHITE, 1, FW_TEXT_ELLIPSIS, &cut);
        CHECK(cut && w == 6 * 5 - 1);                   /* "Palm" + ellipsis: the space before it is dropped */
        clear();
        w = fw_text_draw_fit(&S, 0, 0, 40, "Palm Beach Intl", WHITE, 1, FW_TEXT_CLIP, &cut);
        CHECK(cut && w == 6 * 6 - 1);                   /* six whole glyphs */
        CHECK(fw_text_draw_fit(&S, 0, 0, 3, "Palm", WHITE, 1, FW_TEXT_ELLIPSIS, &cut) == 0 && cut);
        CHECK(fw_text_draw_fit(&S, 0, 0, 0, "Palm", WHITE, 1, FW_TEXT_CLIP, &cut) == 0);
        clear();
        w = fw_text_draw_fit(&S, 0, 0, 64, "ABCDEFGHIJ", WHITE, 2, FW_TEXT_ELLIPSIS, &cut);
        CHECK(cut && w <= 64 && w == 5 * 12 - 2);
    }

    /* --- centring ---------------------------------------------------------- */
    CHECK(fw_text_center_x(0, 128, fw_text_width("CLT-PBI", 1)) == 43);

    printf("PASS: %d text checks\n", checks);
    return 0;
}
