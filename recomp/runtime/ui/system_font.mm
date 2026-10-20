// The overlay menu's text font (system_font.h): a system font file ImGui can load. The known paths first (the Mac's,
// Windows', Linux's, Android's), then on Apple systems the file CoreText reports for Avenir Next or Helvetica: iOS keeps
// its fonts elsewhere, and without one the menu fell back to ImGui's built-in 13 px bitmap font (tiny on an iPad).
#include "system_font.h"

#include <stdio.h>
#include <sys/stat.h>

#ifdef __APPLE__
#import <CoreText/CoreText.h>
#endif

static bool exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0;
}

bool ui_system_font(char *path, size_t n, int *face) {
    static const char *const known[] = {"/System/Library/Fonts/Avenir Next.ttc", "/System/Library/Fonts/Helvetica.ttc",
                                        "C:/Windows/Fonts/segoeui.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                                        "/usr/share/fonts/TTF/DejaVuSans.ttf", "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf",
                                        "/system/fonts/Roboto-Regular.ttf"};
    for (const char *p : known)
        if (exists(p)) {
            *face = 0;
            return (size_t)snprintf(path, n, "%s", p) < n;
        }
#ifdef __APPLE__
    return ui_system_font_coretext(path, n, face);
#else
    return false;
#endif
}

#ifdef __APPLE__
bool ui_system_font_coretext(char *path, size_t n, int *face) {
    static const CFStringRef names[] = {CFSTR("AvenirNext-Regular"), CFSTR("Helvetica")};
    for (CFStringRef name : names) {
        CTFontDescriptorRef d = CTFontDescriptorCreateWithNameAndSize(name, 12);
        CFURLRef url = d ? (CFURLRef)CTFontDescriptorCopyAttribute(d, kCTFontURLAttribute) : NULL;
        bool ok = url && CFURLGetFileSystemRepresentation(url, true, (UInt8 *)path, (CFIndex)n) && exists(path);
        if (url) CFRelease(url);
        if (d) CFRelease(d);
        if (ok) {
            *face = 0;
            return true;
        }
    }
    return false;
}
#endif
