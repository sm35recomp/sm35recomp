// frame_diff <a.png> <b.png>: "<pixels differing> <pixels off by more than 24> <total>" (any channel), exit 0;
// exit 2 when an image can't be read or the sizes differ. (for A/B runs of two builds: frames saved by LARECOMP_SCREENSHOT_EVERY.)
#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>
#include <stdio.h>
#include <stdlib.h>

static uint8_t *load(const char *path, size_t *w, size_t *h) {
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, (CFIndex)strlen(path), false);
    CGImageSourceRef src = CGImageSourceCreateWithURL(url, NULL);
    CFRelease(url);
    if (!src) return NULL;
    CGImageRef img = CGImageSourceCreateImageAtIndex(src, 0, NULL);
    CFRelease(src);
    if (!img) return NULL;
    *w = CGImageGetWidth(img), *h = CGImageGetHeight(img);
    uint8_t *px = calloc(*w * *h, 4);
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef ctx = CGBitmapContextCreate(px, *w, *h, 8, *w * 4, cs, (CGBitmapInfo)kCGImageAlphaPremultipliedLast);
    CGContextDrawImage(ctx, CGRectMake(0, 0, *w, *h), img);
    CGContextRelease(ctx);
    CGColorSpaceRelease(cs);
    CGImageRelease(img);
    return px;
}

int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "usage: frame_diff a.png b.png\n"); return 2; }
    size_t aw, ah, bw, bh;
    uint8_t *a = load(argv[1], &aw, &ah), *b = load(argv[2], &bw, &bh);
    if (!a || !b || aw != bw || ah != bh) { printf("unreadable or different sizes\n"); return 2; }
    size_t any = 0, big = 0, n = aw * ah;
    for (size_t i = 0; i < n; i++) {
        int m = 0;
        for (int c = 0; c < 3; c++) { int d = abs((int)a[i * 4 + c] - (int)b[i * 4 + c]); if (d > m) m = d; }
        any += m > 0;
        big += m > 24;
    }
    printf("%zu %zu %zu\n", any, big, n);
    return 0;
}
