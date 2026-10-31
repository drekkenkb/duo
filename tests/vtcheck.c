/* Terminal emulator tests: feed escape sequences, check the grid. */
#include "mouse.h"
#include "vterm.h"

#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures, checks;

#define FEED(vt, s) vt_feed((vt), (s), strlen(s))

#define CHECK(cond, ...)                                           \
    do {                                                           \
        checks++;                                                  \
        if (!(cond)) {                                             \
            failures++;                                            \
            printf("FAIL %s:%d: ", __func__, __LINE__);            \
            printf(__VA_ARGS__);                                   \
            printf("\n");                                          \
        }                                                          \
    } while (0)

/* Row y as UTF-8 with trailing blanks trimmed; wide halves skipped. */
static const char *row(VTerm *vt, int y)
{
    static char s[1024];
    char *p = s;
    const Cell *r = vt->screen + y * vt->cols;
    for (int x = 0; x < vt->cols; x++) {
        uint32_t c = r[x].ch;
        if (c == CH_WIDE_CONT)
            continue;
        if (c == 0)
            c = ' ';
        if (c < 0x80) {
            *p++ = (char)c;
        } else if (c < 0x800) {
            *p++ = (char)(0xC0 | c >> 6);
            *p++ = (char)(0x80 | (c & 0x3F));
        } else {
            *p++ = (char)(0xE0 | c >> 12);
            *p++ = (char)(0x80 | ((c >> 6) & 0x3F));
            *p++ = (char)(0x80 | (c & 0x3F));
        }
    }
    while (p > s && p[-1] == ' ')
        p--;
    *p = 0;
    return s;
}

#define ROW_IS(vt, y, want) \
    CHECK(strcmp(row(vt, y), want) == 0, "row %d = '%s', want '%s'", y, row(vt, y), want)

static void test_print_and_wrap(void)
{
    VTerm *vt = vt_new(4, 10);
    FEED(vt, "hello\r\nworld");
    ROW_IS(vt, 0, "hello");
    ROW_IS(vt, 1, "world");
    CHECK(vt->cx == 5 && vt->cy == 1, "cursor %d,%d", vt->cx, vt->cy);

    FEED(vt, "\033[H\033[2J0123456789ab");
    ROW_IS(vt, 0, "0123456789");
    ROW_IS(vt, 1, "ab");

    /* exactly filling a line leaves the wrap pending, not taken */
    FEED(vt, "\033[H\033[2J0123456789");
    CHECK(vt->cy == 0 && vt->cx == 9 && vt->wrap_pending, "pending wrap");
    FEED(vt, "\r\n");
    CHECK(vt->cy == 1 && vt->cx == 0, "CRLF after full line");
    vt_free(vt);
}

static void test_cursor_and_erase(void)
{
    VTerm *vt = vt_new(5, 20);
    FEED(vt, "aaaaaaaaaa\r\nbbbbbbbbbb\r\ncccccccccc");
    FEED(vt, "\033[2;5H\033[K");
    ROW_IS(vt, 1, "bbbb");
    FEED(vt, "\033[1;3H\033[1K");
    ROW_IS(vt, 0, "   aaaaaaa");
    FEED(vt, "\033[3;1H\033[2X");
    ROW_IS(vt, 2, "  cccccccc");
    FEED(vt, "\033[3;1H\033[3P");
    ROW_IS(vt, 2, "ccccccc");
    FEED(vt, "\033[3;1H\033[2@XY");
    ROW_IS(vt, 2, "XYccccccc");
    FEED(vt, "\033[10;30H");
    CHECK(vt->cx == 19 && vt->cy == 4, "CUP clamps: %d,%d", vt->cx, vt->cy);
    FEED(vt, "\033[2J");
    ROW_IS(vt, 0, "");
    vt_free(vt);
}

static void test_scrolling(void)
{
    VTerm *vt = vt_new(3, 10);
    FEED(vt, "1\r\n2\r\n3\r\n4\r\n5");
    ROW_IS(vt, 0, "3");
    ROW_IS(vt, 2, "5");
    CHECK(vt->sb_count == 2, "scrollback %d", vt->sb_count);

    Cell line[10];
    vt_history_line(vt, 0, line, 10);
    CHECK(line[0].ch == '1', "oldest history line");
    vt_history_line(vt, 2, line, 10);
    CHECK(line[0].ch == '3', "history continues into the screen");

    /* scroll region: lines outside it stay, nothing goes to scrollback */
    vt_free(vt);
    vt = vt_new(5, 10);
    FEED(vt, "top\r\na\r\nb\r\nc\r\nbottom");
    FEED(vt, "\033[2;4r\033[4;1H\n");
    ROW_IS(vt, 0, "top");
    ROW_IS(vt, 1, "b");
    ROW_IS(vt, 2, "c");
    ROW_IS(vt, 3, "");
    ROW_IS(vt, 4, "bottom");
    CHECK(vt->sb_count == 0, "region scroll kept out of history");

    FEED(vt, "\033[2;1H\033M");
    ROW_IS(vt, 1, "");
    ROW_IS(vt, 2, "b");

    FEED(vt, "\033[r\033[3;1H\033[L");
    ROW_IS(vt, 2, "");
    ROW_IS(vt, 3, "b");
    FEED(vt, "\033[M");
    ROW_IS(vt, 2, "b");
    vt_free(vt);
}

static void test_sgr(void)
{
    VTerm *vt = vt_new(2, 20);
    FEED(vt, "\033[1;31mA\033[38;5;196mB\033[38;2;1;2;3;48;5;17mC\033[0mD\033[7;94mE");
    const Cell *r = vt->screen;
    CHECK(r[0].attr == ATTR_BOLD && r[0].fg == COLOR_PAL(1), "bold red");
    CHECK(r[1].fg == COLOR_PAL(196), "256-colour");
    CHECK(r[2].fg == COLOR_RGB(1, 2, 3) && r[2].bg == COLOR_PAL(17), "truecolour");
    CHECK(r[3].attr == 0 && r[3].fg == COLOR_DEFAULT, "reset");
    CHECK(r[4].attr == ATTR_REVERSE && r[4].fg == COLOR_PAL(12), "bright + reverse");
    vt_free(vt);
}

static void test_alt_screen(void)
{
    VTerm *vt = vt_new(3, 10);
    FEED(vt, "shell$ ls");
    FEED(vt, "\033[?1049h\033[H\033[2Jvim stuff");
    CHECK(vt->alt_active, "alt active");
    ROW_IS(vt, 0, "vim stuff");
    FEED(vt, "\033[?1049l");
    CHECK(!vt->alt_active, "back on main");
    ROW_IS(vt, 0, "shell$ ls");
    CHECK(vt->cx == 9 && vt->cy == 0, "cursor restored: %d,%d", vt->cx, vt->cy);
    vt_free(vt);
}

static void test_replies(void)
{
    VTerm *vt = vt_new(5, 10);
    FEED(vt, "\033[3;4H\033[6n");
    CHECK(vt->reply_len == 6 && memcmp(vt->reply, "\033[3;4R", 6) == 0, "DSR");
    vt->reply_len = 0;
    FEED(vt, "\033[c");
    CHECK(vt->reply_len > 0, "DA");
    vt_free(vt);
}

static void test_unicode(void)
{
    VTerm *vt = vt_new(2, 6);
    FEED(vt, "a\xc3\xa9\xe6\x97\xa5z"); /* a é 日 z */
    const Cell *r = vt->screen;
    CHECK(r[1].ch == 0xE9, "2-byte UTF-8");
    CHECK(r[2].ch == 0x65E5 && r[3].ch == CH_WIDE_CONT, "wide char takes two cells");
    CHECK(r[4].ch == 'z' && vt->cx == 5, "after wide");

    /* overwriting the right half of a wide char clears the left half */
    FEED(vt, "\033[1;4HX");
    CHECK(r[2].ch == 0 && r[3].ch == 'X', "wide char split");

    /* wide char that doesn't fit wraps to the next line */
    FEED(vt, "\033[1;6H\xe6\x97\xa5");
    CHECK(vt->screen[6].ch == 0x65E5, "wide wraps");

    FEED(vt, "\033[2J\033[H\033(0lqk\033(B");
    CHECK(r[0].ch == 0x250C && r[1].ch == 0x2500 && r[2].ch == 0x2510, "DEC line drawing");
    vt_free(vt);
}

static void test_strings_ignored(void)
{
    VTerm *vt = vt_new(2, 20);
    FEED(vt, "\033]0;my title\007ok\033]2;x\033\\!\033P1$r\033\\?");
    ROW_IS(vt, 0, "ok!?");
    vt_free(vt);
}

static void test_split_sequences(void)
{
    VTerm *vt = vt_new(3, 20);
    const char *s = "\033[1;31mred\033[0m \xe6\x97\xa5\033[2;3Hx";
    for (size_t i = 0; s[i]; i++)
        vt_feed(vt, s + i, 1);
    ROW_IS(vt, 0, "red 日");
    CHECK(vt->screen[0].fg == COLOR_PAL(1), "colour survives byte-at-a-time");
    CHECK(vt->screen[20 + 2].ch == 'x', "CUP split across reads");
    vt_free(vt);
}

static void test_resize(void)
{
    VTerm *vt = vt_new(5, 10);
    FEED(vt, "1\r\n2\r\n3\r\n4\r\n5");
    vt_resize(vt, 3, 10);
    ROW_IS(vt, 0, "3");
    ROW_IS(vt, 2, "5");
    CHECK(vt->cy == 2, "cursor row kept: %d", vt->cy);
    CHECK(vt->sb_count == 2, "shrink pushed to history: %d", vt->sb_count);

    vt_resize(vt, 6, 4);
    ROW_IS(vt, 0, "3");
    CHECK(vt->rows == 6 && vt->cols == 4, "grown");
    FEED(vt, "\033[6;4Hz");
    CHECK(vt->screen[5 * 4 + 3].ch == 'z', "new area usable");
    vt_free(vt);
}

static void test_tabs(void)
{
    VTerm *vt = vt_new(2, 30);
    FEED(vt, "a\tb\tc");
    CHECK(vt->screen[8].ch == 'b' && vt->screen[16].ch == 'c', "default tab stops");
    vt_free(vt);
}

/* Random garbage and random resizes must never crash or corrupt memory.
 * Run under `make asan` for this to mean much. */
static void test_fuzz(void)
{
    static const char *pieces[] = {
        "\033[", "\033]", "\033P", "\033(0", "\033#8", "\033[?1049h", "\033[?1049l",
        "\033[r", "\033[5;2r", "\033[L", "\033[M", "\033[99@", "\033[99P", "\033[3J",
        "\xe6\x97\xa5", "\xf0\x9f\x98\x80", "\r\n", "\t", "\b", ";", "?", "38;2;",
        "999999", "m", "H", "J", "K", "b", "\033D", "\033M", "\0337", "\0338",
    };
    srand(1234);
    VTerm *vt = vt_new(24, 80);
    char buf[4096];
    for (int round = 0; round < 3000; round++) {
        size_t n = 0;
        while (n < sizeof buf - 32) {
            if (rand() % 3 == 0) {
                const char *p = pieces[rand() % (sizeof pieces / sizeof *pieces)];
                size_t l = strlen(p);
                memcpy(buf + n, p, l);
                n += l;
            } else {
                buf[n++] = (char)(rand() % 256);
            }
            if (rand() % 64 == 0)
                break;
        }
        vt_feed(vt, buf, n);
        vt->reply_len = 0;
        if (round % 97 == 0)
            vt_resize(vt, 1 + rand() % 60, 1 + rand() % 200);
        CHECK(vt->cx >= 0 && vt->cx < vt->cols && vt->cy >= 0 && vt->cy < vt->rows,
              "cursor in bounds after round %d: %d,%d in %dx%d", round, vt->cx, vt->cy,
              vt->cols, vt->rows);
        if (failures)
            break;
    }
    vt_free(vt);
}

static void test_mouse_modes(void)
{
    VTerm *vt = vt_new(4, 10);
    CHECK(vt->mouse_mode == 0 && !vt->mouse_sgr, "mouse off at start");
    FEED(vt, "\033[?1000h\033[?1002h\033[?1006h");
    CHECK(vt->mouse_mode == 1002 && vt->mouse_sgr, "mode %d sgr %d", vt->mouse_mode, vt->mouse_sgr);
    FEED(vt, "\033[?1000l");
    CHECK(vt->mouse_mode == 1002, "turning off another mode keeps 1002: %d", vt->mouse_mode);
    FEED(vt, "\033[?1002l\033[?1006l");
    CHECK(vt->mouse_mode == 0 && !vt->mouse_sgr, "all off");
    FEED(vt, "\033[?1003h\033c");
    CHECK(vt->mouse_mode == 0, "RIS clears mouse");
    vt_free(vt);
}

static void test_mouse_codec(void)
{
    MouseEvent ev;
    char out[32];
    size_t n;

    n = mouse_parse("\033[<64;12;5Mrest", 15, &ev);
    CHECK(n == 11 && ev.b == 64 && ev.x == 11 && ev.y == 4 && !ev.release,
          "sgr wheel: n=%zu b=%d x=%d y=%d", n, ev.b, ev.x, ev.y);
    n = mouse_parse("\033[<0;300;2m", 11, &ev);
    CHECK(n == 11 && ev.b == 0 && ev.x == 299 && ev.release, "sgr release, wide column");
    CHECK(mouse_parse("\033[<0;1", 6, &ev) == 0, "incomplete sgr");
    CHECK(mouse_parse("\033[A", 3, &ev) == 0, "arrow key is not a mouse report");
    n = mouse_parse("\033[M`!!", 6, &ev);
    CHECK(n == 6 && ev.b == 64 && ev.x == 0 && ev.y == 0, "x10 wheel");

    ev = (MouseEvent){ 65, 4, 2, 0 };
    n = mouse_encode(&ev, 1000, 1, out);
    CHECK(n && strncmp(out, "\033[<65;5;3M", n) == 0, "sgr encode: %.*s", (int)n, out);
    n = mouse_encode(&ev, 1000, 0, out);
    CHECK(n == 6 && out[3] == 65 + 32 && out[4] == 5 + 32 && out[5] == 3 + 32, "x10 encode");
    CHECK(mouse_encode(&ev, 0, 1, out) == 0, "no mode, no report");

    ev = (MouseEvent){ MOUSE_MOTION | 0, 1, 1, 0 };
    CHECK(mouse_encode(&ev, 1000, 1, out) == 0, "1000 drops drags");
    CHECK(mouse_encode(&ev, 1002, 1, out) > 0, "1002 keeps drags");
    ev.b = MOUSE_MOTION | 3;
    CHECK(mouse_encode(&ev, 1002, 1, out) == 0, "1002 drops bare motion");
    CHECK(mouse_encode(&ev, 1003, 1, out) > 0, "1003 keeps bare motion");

    ev = (MouseEvent){ 0, 1, 1, 1 };
    n = mouse_encode(&ev, 1000, 0, out);
    CHECK(n == 6 && out[3] == 3 + 32, "x10 release is button 3");
    CHECK(mouse_encode(&ev, 9, 0, out) == 0, "mode 9 has no releases");
    ev = (MouseEvent){ 0, 300, 1, 0 };
    CHECK(mouse_encode(&ev, 1000, 0, out) == 0, "x10 can't reach column 301");
}

int main(void)
{
    setlocale(LC_CTYPE, "");
    if (MB_CUR_MAX == 1)
        setlocale(LC_CTYPE, "C.UTF-8");

    test_print_and_wrap();
    test_cursor_and_erase();
    test_scrolling();
    test_sgr();
    test_alt_screen();
    test_replies();
    test_unicode();
    test_strings_ignored();
    test_split_sequences();
    test_resize();
    test_tabs();
    test_mouse_modes();
    test_mouse_codec();
    test_fuzz();

    printf("%d/%d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
