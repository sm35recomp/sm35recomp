// The overlay menu's text font: a system font file (runtime/ui/system_font.mm).
#pragma once
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif
// The path of a system font file for ImGui, and the face to use in it (a .ttc holds several). false: none found.
bool ui_system_font(char *path, size_t n, int *face);
#ifdef __APPLE__
bool ui_system_font_coretext(char *path, size_t n, int *face);  // (the Apple lookup alone: iOS, and its test on the Mac)
#endif
#ifdef __cplusplus
}
#endif
