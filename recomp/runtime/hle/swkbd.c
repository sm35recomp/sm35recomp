// Software keyboard (naming Link on a new file, ...). On the Switch nn::swkbd::ShowKeyboard launches the keyboard
// library applet; our am service has no applets, so the game crashed. ShowKeyboard is replaced here: the overlay
// (runtime/ui/menu.mm menu_keyboard_run) asks for the text - typed directly with a keyboard, an on-screen keyboard
// with a controller - while this (game) thread keeps re-presenting the last frame.
#include <stdio.h>
#include <string.h>

#include "../hle.h"

int menu_keyboard_run(const char *initial, int max_len, char *out, int cap);  // runtime/ui/menu.mm

// nn::swkbd::ShowKeyboard(nn::swkbd::String *out, const nn::swkbd::ShowKeyboardArg &arg) -> nn::Result
// String = {void *ptr; size_t bufSize}
// ShowKeyboardArg (probed 2026-10-08, naming Link): +0x3ac text max length, +0x3b0 min length, +0x3c0 / +0x3c4
// initial text offset / length (UTF-16) in the work buffer at +0x4d0. SMM2 reads the answer as UTF-16 (LE, NUL-
// terminated; Link's Awakening read UTF-8): typed UTF-8 went in as CJK junk in Save As course names.
HLE_FUNC("_ZN2nn5swkbd12ShowKeyboardEPNS0_6StringERKNS0_15ShowKeyboardArgE", swkbd_show) {
    uint64_t out = c->x[0], arg = c->x[1];
    uint64_t ptr = LD64(out), size = LD64(out + 8);
    int max_len = (int)*(const int32_t *)(uintptr_t)(arg + 0x3ac);
    uint32_t off = *(const uint32_t *)(uintptr_t)(arg + 0x3c0), n = *(const uint32_t *)(uintptr_t)(arg + 0x3c4);
    uint64_t wb = LD64(arg + 0x4d0);
    char initial[192] = "";
    size_t il = 0;
    for (uint32_t i = 0; wb && i < n && il + 4 < sizeof initial; i++) {  // UTF-16 -> UTF-8
        uint32_t ch = *(const uint16_t *)(uintptr_t)(wb + off + 2 * i);
        if (ch >= 0xd800 && ch < 0xdc00 && i + 1 < n) {
            uint32_t lo = *(const uint16_t *)(uintptr_t)(wb + off + 2 * (i + 1));
            if (lo >= 0xdc00 && lo < 0xe000) { ch = 0x10000 + ((ch - 0xd800) << 10) + (lo - 0xdc00); i++; }
        }
        if (ch < 0x80) initial[il++] = (char)ch;
        else if (ch < 0x800) { initial[il++] = (char)(0xc0 | ch >> 6); initial[il++] = (char)(0x80 | (ch & 0x3f)); }
        else if (ch < 0x10000) {
            initial[il++] = (char)(0xe0 | ch >> 12); initial[il++] = (char)(0x80 | (ch >> 6 & 0x3f));
            initial[il++] = (char)(0x80 | (ch & 0x3f));
        } else {
            initial[il++] = (char)(0xf0 | ch >> 18); initial[il++] = (char)(0x80 | (ch >> 12 & 0x3f));
            initial[il++] = (char)(0x80 | (ch >> 6 & 0x3f)); initial[il++] = (char)(0x80 | (ch & 0x3f));
        }
    }
    initial[il] = 0;
    char text[192];
    int ok = menu_keyboard_run(initial, max_len, text, sizeof text);
    if (!ok) snprintf(text, sizeof text, "%s", initial);  // cancelled: keep what was there
    LOG("[swkbd] name entry (max %d, initial \"%s\"): %s \"%s\"\n", max_len, initial, ok ? "entered" : "cancelled", text);
    if (ptr && size >= 2) {  // UTF-8 -> UTF-16 LE, NUL-terminated
        memset((void *)(uintptr_t)ptr, 0, size);
        uint16_t *o = (uint16_t *)(uintptr_t)ptr;
        size_t cap = size / 2 - 1, k = 0;
        const unsigned char *t = (const unsigned char *)text;
        while (*t && k < cap) {
            uint32_t ch = *t++;
            int more = ch >= 0xf0 ? 3 : ch >= 0xe0 ? 2 : ch >= 0xc0 ? 1 : 0;
            if (more) ch &= 0x3f >> more;
            for (; more && (*t & 0xc0) == 0x80; more--) ch = ch << 6 | (*t++ & 0x3f);
            if (ch >= 0x10000) {
                if (k + 1 >= cap) break;
                ch -= 0x10000;
                o[k++] = (uint16_t)(0xd800 | ch >> 10);
                o[k++] = (uint16_t)(0xdc00 | (ch & 0x3ff));
            } else o[k++] = (uint16_t)ch;
        }
    }
    c->x[0] = 0;
}
