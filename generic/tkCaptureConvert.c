/*
 * tkCaptureConvert.c --
 *
 *	Conversion of an XImage in ZPixmap format into RGBA bytes, for
 *	visuals of any class and depth.
 *
 * Copyright © 2026 Bandoti Ltd.
 *
 * See the file "license.terms" for information on usage and redistribution of
 * this file, and for a DISCLAIMER OF ALL WARRANTIES.
 */

#include "tkCapture.h"
#include <X11/Xutil.h>
#include <string.h>

/*
 * Position and width of one color channel inside a pixel value.
 */

typedef struct Channel {
    int shift;			/* Bits below the channel. */
    int bits;			/* Width of the channel; 0 if absent. */
    unsigned long max;		/* Largest channel value, (1 << bits) - 1. */
} Channel;

/*
 * Largest colormap that is read as a whole for colormapped visuals.
 */

#define MAX_COLORMAP_SIZE 65536

/*
 * Forward declarations for functions defined in this file.
 */

static void		DecomposeMask(unsigned long mask, Channel *chanPtr);
static inline unsigned char Chan8(unsigned long pixel,
			    const Channel *chanPtr);
static void		ConvertTrueColor(XImage *imagePtr, Visual *visual,
			    unsigned char *dst, int pitch);
static int		ConvertMapped(Tcl_Interp *interp, Display *display,
			    XImage *imagePtr, Visual *visual,
			    Colormap colormap, unsigned char *dst,
			    int pitch);

/*
 *----------------------------------------------------------------------
 *
 * DecomposeMask --
 *
 *	Finds the position and the width of the contiguous run of bits
 *	set in a channel mask.
 *
 * Results:
 *	*chanPtr is filled in. A zero mask yields a channel of 0 bits.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static void
DecomposeMask(
    unsigned long mask,		/* Channel mask of a visual. */
    Channel *chanPtr)		/* Receives shift, width and maximum. */
{
    chanPtr->shift = 0;
    chanPtr->bits = 0;
    chanPtr->max = 0;
    if (mask == 0) {
	return;
    }
    while (!(mask & 1)) {
	mask >>= 1;
	chanPtr->shift++;
    }
    while (mask & 1) {
	mask >>= 1;
	chanPtr->bits++;
    }
    chanPtr->max = (chanPtr->bits >= (int) (8 * sizeof(unsigned long)))
	    ? ~0UL : (1UL << chanPtr->bits) - 1;
}

/*
 *----------------------------------------------------------------------
 *
 * Chan8 --
 *
 *	Extracts one channel from a pixel value and scales it to the
 *	range 0..255, so that the largest channel value maps to 255
 *	whatever the width of the channel.
 *
 * Results:
 *	The 8-bit channel value; 0 for an absent channel.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static inline unsigned char
Chan8(
    unsigned long pixel,	/* Pixel value. */
    const Channel *chanPtr)	/* Channel to extract. */
{
    unsigned long value;

    if (chanPtr->bits == 0) {
	return 0;
    }
    value = (pixel >> chanPtr->shift) & chanPtr->max;
    if (chanPtr->bits == 8) {
	return (unsigned char) value;
    }
    if (chanPtr->bits > 8) {
	return (unsigned char) (value >> (chanPtr->bits - 8));
    }
    return (unsigned char) ((value * 255 + chanPtr->max / 2) / chanPtr->max);
}

/*
 *----------------------------------------------------------------------
 *
 * ConvertTrueColor --
 *
 *	Converts an image of a TrueColor visual.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Writes imagePtr->width x imagePtr->height RGBA pixels to dst.
 *
 *----------------------------------------------------------------------
 */

static void
ConvertTrueColor(
    XImage *imagePtr,		/* Source image. */
    Visual *visual,		/* Visual the image was read from. */
    unsigned char *dst,		/* First destination pixel. */
    int pitch)			/* Bytes per destination row. */
{
    Channel red, green, blue;
    int x, y, byteChannels;

    DecomposeMask(visual->red_mask, &red);
    DecomposeMask(visual->green_mask, &green);
    DecomposeMask(visual->blue_mask, &blue);
    byteChannels = (red.bits == 8 && green.bits == 8 && blue.bits == 8
	    && !(red.shift & 7) && !(green.shift & 7) && !(blue.shift & 7));

    if (imagePtr->bits_per_pixel == 32 && byteChannels) {
	/*
	 * Common case: every channel is one byte of a 4-byte pixel. Pick
	 * the bytes directly; their position in memory follows from the
	 * byte order of the image.
	 */

	int rOff = red.shift / 8, gOff = green.shift / 8;
	int bOff = blue.shift / 8;

	if (imagePtr->byte_order == MSBFirst) {
	    rOff = 3 - rOff;
	    gOff = 3 - gOff;
	    bOff = 3 - bOff;
	}
	for (y = 0; y < imagePtr->height; y++) {
	    const unsigned char *src = (const unsigned char *)
		    imagePtr->data + (size_t) y * imagePtr->bytes_per_line
		    + (size_t) imagePtr->xoffset * 4;
	    unsigned char *out = dst + (size_t) y * pitch;

	    for (x = 0; x < imagePtr->width; x++, src += 4, out += 4) {
		out[0] = src[rOff];
		out[1] = src[gOff];
		out[2] = src[bOff];
		out[3] = 255;
	    }
	}
	return;
    }

    for (y = 0; y < imagePtr->height; y++) {
	unsigned char *out = dst + (size_t) y * pitch;

	for (x = 0; x < imagePtr->width; x++, out += 4) {
	    unsigned long pixel = XGetPixel(imagePtr, x, y);

	    out[0] = Chan8(pixel, &red);
	    out[1] = Chan8(pixel, &green);
	    out[2] = Chan8(pixel, &blue);
	    out[3] = 255;
	}
    }
}

/*
 *----------------------------------------------------------------------
 *
 * ConvertMapped --
 *
 *	Converts an image of a visual whose pixel values are indices into
 *	a colormap: PseudoColor, StaticColor, GrayScale and StaticGray
 *	(one index per pixel) and DirectColor (one index per channel).
 *
 * Results:
 *	TCL_OK, or TCL_ERROR with a message in interp if the colormap is
 *	too large to be read.
 *
 * Side effects:
 *	Queries the colormap from the X server. Writes imagePtr->width x
 *	imagePtr->height RGBA pixels to dst.
 *
 *----------------------------------------------------------------------
 */

static int
ConvertMapped(
    Tcl_Interp *interp,		/* For error reporting. */
    Display *display,		/* Display of the colormap. */
    XImage *imagePtr,		/* Source image. */
    Visual *visual,		/* Visual the image was read from. */
    Colormap colormap,		/* Colormap of the window. */
    unsigned char *dst,		/* First destination pixel. */
    int pitch)			/* Bytes per destination row. */
{
    Channel red, green, blue;
    int direct = (visual->class == DirectColor);
    int numColors = visual->map_entries;
    unsigned long mapSize;	/* numColors, for comparing with pixels. */
    XColor *colors;
    int i, x, y;

    if (numColors <= 0 || numColors > MAX_COLORMAP_SIZE) {
	return TkCaptureFailed(interp, "UNSUPPORTED",
		"can't capture a window with this kind of visual");
    }
    if (direct) {
	DecomposeMask(visual->red_mask, &red);
	DecomposeMask(visual->green_mask, &green);
	DecomposeMask(visual->blue_mask, &blue);
    }

    /*
     * Read the whole colormap with one request. For DirectColor, entry i
     * of each of the three channel maps is addressed by a pixel value
     * with i in all three channel fields.
     */

    colors = (XColor *) Tcl_Alloc(sizeof(XColor) * (size_t) numColors);
    for (i = 0; i < numColors; i++) {
	unsigned long index = (unsigned long) i;

	colors[i].pixel = direct ? (((index & red.max) << red.shift)
		| ((index & green.max) << green.shift)
		| ((index & blue.max) << blue.shift)) : index;
	colors[i].red = colors[i].green = colors[i].blue = 0;
	colors[i].flags = DoRed | DoGreen | DoBlue;
    }
    XQueryColors(display, colormap, colors, numColors);
    mapSize = (unsigned long) numColors;

    for (y = 0; y < imagePtr->height; y++) {
	unsigned char *out = dst + (size_t) y * pitch;

	for (x = 0; x < imagePtr->width; x++, out += 4) {
	    unsigned long pixel = XGetPixel(imagePtr, x, y);
	    unsigned long r, g, b;

	    if (direct) {
		r = (pixel >> red.shift) & red.max;
		g = (pixel >> green.shift) & green.max;
		b = (pixel >> blue.shift) & blue.max;
	    } else {
		r = g = b = pixel;
	    }
	    out[0] = (r < mapSize) ? colors[r].red >> 8 : 0;
	    out[1] = (g < mapSize) ? colors[g].green >> 8 : 0;
	    out[2] = (b < mapSize) ? colors[b].blue >> 8 : 0;
	    out[3] = 255;
	}
    }
    Tcl_Free(colors);
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * TkCaptureXImageToRGBA --
 *
 *	Converts an XImage in ZPixmap format, read from a drawable with
 *	the given visual and colormap, to RGBA bytes. Every converted
 *	pixel is opaque.
 *
 * Results:
 *	TCL_OK, or TCL_ERROR with a message and an error code in interp
 *	if the visual is not supported.
 *
 * Side effects:
 *	Writes imagePtr->width x imagePtr->height pixels starting at dst,
 *	with rows pitch bytes apart. May query the colormap from the X
 *	server.
 *
 *----------------------------------------------------------------------
 */

int
TkCaptureXImageToRGBA(
    Tcl_Interp *interp,		/* For error reporting. */
    Display *display,		/* Display the image was read from. */
    XImage *imagePtr,		/* Source image, ZPixmap format. */
    Visual *visual,		/* Visual of the source drawable. */
    Colormap colormap,		/* Colormap of the source drawable. */
    unsigned char *dst,		/* First destination pixel. */
    int pitch)			/* Bytes per destination row. */
{
    if (visual->class == TrueColor) {
	ConvertTrueColor(imagePtr, visual, dst, pitch);
	return TCL_OK;
    }
    return ConvertMapped(interp, display, imagePtr, visual, colormap, dst,
	    pitch);
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * End:
 */
