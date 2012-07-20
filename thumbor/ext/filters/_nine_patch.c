#include "filter.h"

typedef struct
{
    int width;
    int height;
    int stride;
    int alpha_idx;
    unsigned char* buffer;
} bitmap;

int get_subpixel(bitmap *b, int x, int y, int s)
{
    int pixel = (y * b->width + x) * b->stride;
    return b->buffer[pixel + s];
}

void set_subpixel(bitmap *b, int x, int y, int s, int value)
{
    int pixel = (y * b->width + x) * b->stride;
    b->buffer[pixel + s] = value;
}

// Returns nonzero if the pixel at x,y is opaque black. That color indicates stretchiness!
int is_stretchy(bitmap *b, int x, int y)
{
    int s;
    for (s = 0; s < b->stride; s++) {
        int expected = (s == b->alpha_idx) ? 255 : 0;
        if (get_subpixel(b, x, y, s) != expected) {
            return 0;
        }
    }
    return 1;
}

// Returns the number of stretchy columns in 'b'.
int compute_stretchy_width(bitmap *b)
{
    int result = 0;
    int x;
    for (x = 1; x < b->width - 1; x++) {
        if (is_stretchy(b, x, 0) != 0) {
            result++;
        }
    }
    return result;
}

// Returns the number of stretchy rows in 'b'.
int compute_stretchy_height(bitmap *b)
{
    int result = 0;
    int y;
    for (y = 1; y < b->height - 1; y++) {
        if (is_stretchy(b, 0, y) != 0) {
            result++;
        }
    }
    return result;
}

// Returns the first pixel in the next row. That is, the next row with different stretchiness.
int next_row(bitmap *b, int y)
{
    int stretchy = is_stretchy(b, 0, y) != 0;
    int n;
    for (n = y + 1; n < b->height - 1; n++) {
        int n_stretchy = is_stretchy(b, 0, n) != 0;
        if (stretchy != n_stretchy) {
            break;
        }
    }
    return n;
}

// Returns the first pixel in the next column. That is, the next column with different stretchiness.
int next_column(bitmap *b, int x)
{
    int stretchy = is_stretchy(b, x, 0) != 0;
    int n;
    for (n = x + 1; n < b->width - 1; n++) {
        int n_stretchy = is_stretchy(b, n, 0) != 0;
        if (stretchy != n_stretchy) {
            break;
        }
    }
    return n;
}

// Draws a region of source into a region of target, stretching as necessary.
void paste_rectangle(bitmap *source, int sx, int sy, int sw, int sh,
        bitmap *target, int tx, int ty, int tw, int th)
{
    // TODO: currently this just copies pixels without stretching. Implement stretching!
    // TODO: don't go out of bounds on either rectangle (security!)
    int y;
    int x;
    int s; // subpixel; either r, g, b, or a
    for (y = 0; y < sh; y++) {
        for (x = 0; x < sw; x++) {
            int source_alpha = 255 - get_subpixel(source, sx + x, sy + y, source->alpha_idx);
            int target_alpha = 255 - get_subpixel(target, tx + x, ty + y, target->alpha_idx);
            for (s = 0; s < source->stride; s++) {
                if (s == source->alpha_idx) {
                    continue;
                }
                double pixel = ALPHA_COMPOSITE_COLOR_CHANNEL(
                        get_subpixel(source, sx + x, sy + y, s), source_alpha,
                        get_subpixel(target, tx + x, ty + y, s), target_alpha);
                set_subpixel(target, tx + x, ty + y, s, ADJUST_COLOR_DOUBLE(pixel));
            }
        }
    }
}

static PyObject*
_nine_patch_apply(PyObject *self, PyObject *args)
{
    PyObject *image_mode = NULL;
    PyObject *target_buffer = NULL;
    PyObject *target_w = NULL;
    PyObject *target_h = NULL;
    PyObject *nine_patch_buffer = NULL;
    PyObject *nine_patch_w = NULL;
    PyObject *nine_patch_h = NULL;

    if (!PyArg_UnpackTuple(args, "apply", 7, 7, &image_mode,
            &target_buffer, &target_w, &target_h,
            &nine_patch_buffer, &nine_patch_w, &nine_patch_h)) {
        return NULL;
    }

    char *image_mode_str = PyString_AsString(image_mode);

    bitmap target;
    target.buffer = (unsigned char *) PyString_AsString(target_buffer);
    target.width = (int) PyInt_AsLong(target_w);
    target.height = (int) PyInt_AsLong(target_h);
    target.stride = bytes_per_pixel(image_mode_str); // typically 4 for 'RGBA'
    target.alpha_idx = rgb_order(image_mode_str, 'A');

    bitmap nine_patch;
    nine_patch.buffer = (unsigned char *) PyString_AsString(nine_patch_buffer);
    nine_patch.width = (int) PyInt_AsLong(nine_patch_w);
    nine_patch.height = (int) PyInt_AsLong(nine_patch_h);
    nine_patch.stride = bytes_per_pixel(image_mode_str); // typically 4 for 'RGBA'
    nine_patch.alpha_idx = rgb_order(image_mode_str, 'A');

    // The number of stretchy pixels in the source.
    int source_stretchy_width = compute_stretchy_width(&nine_patch);
    int source_stretchy_height = compute_stretchy_height(&nine_patch);

    // The number of fixed pixels in the source and target.
    int fixed_width = nine_patch.width - 2 - source_stretchy_width;
    int fixed_height = nine_patch.height - 2 - source_stretchy_height;

    // The number of target pixels to be shared by all stretchy regions.
    int target_stretchy_width = target.width - fixed_width;
    int target_stretchy_height = target.height - fixed_height;

    /*
     * Cut the image into rows and columns, each axis alternating between
     * stretchy and non-stretchy. This walks through the rows from top to
     * bottom. Within each row it walks through the columns left to right.
     * It computes the size & location of each cell in both the nine patch and
     * the target image. Then it pastes one cell over the other, stretching the
     * source image as necessary.
     */
    int source_y = 1;
    int target_y = 0;
    while (source_y < nine_patch.height - 1) {
        int row_stretchy = is_stretchy(&nine_patch, 0, source_y);
        int source_height = next_row(&nine_patch, source_y) - source_y;
        int target_height = (row_stretchy != 0)
                ? (int) ((double) source_height / source_stretchy_height * target_stretchy_height + 0.5)
                : source_height;

        int source_x = 1;
        int target_x = 0;
        while (source_x < nine_patch.width - 1) {
            int col_stretchy = is_stretchy(&nine_patch, source_x, 0);
            int source_width = next_column(&nine_patch, source_x) - source_x;
            int target_width = (col_stretchy != 0)
                    ? (int) ((double) source_width / source_stretchy_width * target_stretchy_width + 0.5)
                    : source_width;
            paste_rectangle(&nine_patch, source_x, source_y, source_width, source_height,
                    &target, target_x, target_y, target_width, target_height);
            target_x += target_width;
            source_x += source_width;
        }

        target_y += target_height;
        source_y += source_height;
    }

    Py_INCREF(target_buffer);
    return target_buffer;
}

FILTER_MODULE(_nine_patch,
    "apply(image_mode, target_buffer, target_w, target_h, nine_patch_buffer, nine_patch_w, nine_patch_h) -> string\n"
    "Applies a nine patch..."
)
