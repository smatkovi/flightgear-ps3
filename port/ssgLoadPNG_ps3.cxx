// PNG textures for PLIB (replaces PLIB's ssgLoadPNG.cxx, which needs glpng).
//
// Aircraft made for FlightGear 1.x mostly come with PNG textures. Uses libpng
// from the PS3 toolchain. Sizes that are not a power of two (later FlightGear
// versions allowed them) are scaled to one, PLIB's mipmapper needs it.

#include <stdio.h>
#include <string.h>
#include <png.h>
#include "ssgLocal.h"

static int pot(unsigned v)
{
    unsigned p = 1;
    while (p < v && p < 2048) p <<= 1;
    return (p > v && p > 1 && (p - v) > (v - p / 2)) ? p / 2 : p;     // nearest power of two
}

bool ssgLoadPNG ( const char *fname, ssgTextureInfo* info )
{
    FILE *fp = fopen(fname, "rb");
    png_structp png;
    png_infop pi;
    png_uint_32 w, h, y;
    int depth, type, comps;
    size_t rowbytes;
    GLubyte *image = NULL;
    png_bytep *rows = NULL;

    if (!fp) {
        ulSetError(UL_WARNING, "ssgLoadPNG: Failed to open '%s' for reading.", fname);
        return false;
    }
    png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    pi = png ? png_create_info_struct(png) : NULL;
    if (!png || !pi || setjmp(png_jmpbuf(png))) {
        ulSetError(UL_WARNING, "ssgLoadPNG: '%s' is not a readable PNG file.", fname);
        png_destroy_read_struct(&png, &pi, NULL);
        delete [] image;
        delete [] rows;
        fclose(fp);
        return false;
    }
    png_init_io(png, fp);
    png_read_info(png, pi);
    png_get_IHDR(png, pi, &w, &h, &depth, &type, NULL, NULL, NULL);
    if (type == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
    if (type == PNG_COLOR_TYPE_GRAY && depth < 8) png_set_expand_gray_1_2_4_to_8(png);
    if (png_get_valid(png, pi, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png);
    if (depth == 16) png_set_strip_16(png);
    png_read_update_info(png, pi);
    comps = png_get_channels(png, pi);
    rowbytes = png_get_rowbytes(png, pi);
    image = new GLubyte [ rowbytes * h ];
    rows = new png_bytep [ h ];
    for (y = 0; y < h; y++)
        rows[y] = image + (h - 1 - y) * rowbytes;       // OpenGL wants the bottom row first
    png_read_image(png, rows);
    png_read_end(png, NULL);
    png_destroy_read_struct(&png, &pi, NULL);
    delete [] rows;
    fclose(fp);

    int pw = pot(w), ph = pot(h);
    if (pw != (int)w || ph != (int)h) {                 // nearest-neighbour rescale
        GLubyte *s = new GLubyte [ pw * ph * comps ];
        for (int ty = 0; ty < ph; ty++)
            for (int tx = 0; tx < pw; tx++)
                memcpy(s + (ty * pw + tx) * comps,
                       image + (ty * h / ph) * rowbytes + (tx * w / pw) * comps, comps);
        delete [] image;
        image = s;
        w = pw;
        h = ph;
    }
    if (info != NULL) {
        info->width = w;
        info->height = h;
        info->depth = comps;
        info->alpha = (comps == 2 || comps == 4);
    }
    return ssgMakeMipMaps(image, w, h, comps);          // takes the image
}
