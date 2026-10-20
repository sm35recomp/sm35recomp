// The overlay menu's font file (runtime/ui/system_font.mm): a real font file on this system, found the way the iOS
// app finds it too (CoreText on Apple; iOS has no /System/Library/Fonts/*.ttc paths, which left the menu in ImGui's
// built-in 13 px font). `make test-ui`.
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "ui/system_font.h"

int main(void) {
    char path[1024];
    int face = -1;
    bool ok = ui_system_font(path, sizeof path, &face);
    struct stat st;
    bool exists = ok && stat(path, &st) == 0 && st.st_size > 10000;
    const char *ext = ok ? strrchr(path, '.') : NULL;
    bool font_file = ext && (!strcasecmp(ext, ".ttc") || !strcasecmp(ext, ".ttf") || !strcasecmp(ext, ".otf"));
    bool good = ok && exists && font_file && face >= 0;
    printf("%s test_system_font (%s, face %d)\n", good ? "PASS" : "FAIL", ok ? path : "none", face);
    int bad = !good;
#ifdef __APPLE__
    // the iOS route: what CoreText reports (the Mac answers from its known paths above, so ask it directly too)
    face = -1;
    ok = ui_system_font_coretext(path, sizeof path, &face);
    good = ok && stat(path, &st) == 0 && st.st_size > 10000 && face >= 0;
    printf("%s test_system_font_coretext (%s, face %d)\n", good ? "PASS" : "FAIL", ok ? path : "none", face);
    bad += !good;
#endif
    return bad ? 1 : 0;
}
