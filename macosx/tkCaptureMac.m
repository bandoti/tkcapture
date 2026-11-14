/*
 * tkCaptureMac.m --
 *
 *	macOS backend of the tkcapture extension: copies the pixels from
 *	the bitmap that backs the content view of a Tk window.
 *
 * Copyright © 2026 Bandoti Ltd.
 *
 * See the file "license.terms" for information on usage and redistribution of
 * this file, and for a DISCLAIMER OF ALL WARRANTIES.
 */

#include "tkCapture.h"
#include <tkPlatDecls.h>
#include <X11/Xutil.h>
#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>
#import <objc/message.h>
#include <math.h>
#include <string.h>

/*
 * Forward declarations for functions defined in this file.
 */

static CGContextRef	GetBackingContext(Tcl_Interp *interp, NSView *view,
			    int *hasBackingPtr);
static void		GetViewOffset(Tk_Window top, NSWindow *window,
			    int *xPtr, int *yPtr);
static int		GrabFromBacking(Tcl_Interp *interp,
			    CGContextRef backing, CGFloat backingScale,
			    int x, int y, int width, int height, int flags,
			    TkCaptureBuffer *bufPtr);
static int		GrabWithXGetImage(Tcl_Interp *interp, Tk_Window top,
			    int x, int y, int width, int height,
			    TkCaptureBuffer *bufPtr);
static int		MaskShift(unsigned long mask);

/*
 *----------------------------------------------------------------------
 *
 * TkCapturePlatformExpose --
 *
 *	See tkCapture.h. Reading the pixels of a window here does not make
 *	the system ask for repaints.
 *
 *----------------------------------------------------------------------
 */

int
TkCapturePlatformExpose(
    Tk_Window top)		/* A mapped toplevel. */
{
    (void) top;
    return 0;
}

/*
 *----------------------------------------------------------------------
 *
 * TkCapturePlatformMenubarHeight --
 *
 *	See tkCapture.h. The menubar is the global one at the top of the
 *	screen, not a part of the window.
 *
 *----------------------------------------------------------------------
 */

int
TkCapturePlatformMenubarHeight(
    Tk_Window top)		/* A mapped toplevel. */
{
    (void) top;
    return 0;
}

/*
 *----------------------------------------------------------------------
 *
 * GetBackingContext --
 *
 *	Returns the bitmap context into which Tk draws everything that a
 *	content view displays.
 *
 * Results:
 *	The context, owned by the view, or NULL if it is not available.
 *	*hasBackingPtr is set to 1 if the view keeps its pixels in such a
 *	bitmap, even if it has none at the moment, and to 0 if the caller
 *	must use the fallback.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static CGContextRef
GetBackingContext(
    Tcl_Interp *interp,		/* For the variable that disables the
				 * lookup. */
    NSView *view,		/* Content view of a Tk window. */
    int *hasBackingPtr)		/* Receives whether Tk draws into a
				 * bitmap of the view. */
{
    SEL selector = NSSelectorFromString(@"tkLayerBitmapContext");

    /*
     * The test suite can make the view look as if it had no bitmap.
     */

    *hasBackingPtr = 0;
    if (TkCaptureForceFallback(interp)) {
	return NULL;
    }

    /*
     * The bitmap context is a property of Tk's private view class, so
     * it is looked up by name at run time. A Tk without the property
     * makes the caller use the slower public path.
     */

    if (view == nil || ![view respondsToSelector:selector]) {
	return NULL;
    }
    *hasBackingPtr = 1;
    return ((CGContextRef (*)(id, SEL)) objc_msgSend)(view, selector);
}

/*
 *----------------------------------------------------------------------
 *
 * GetViewOffset --
 *
 *	Computes the position of a toplevel's origin inside the content
 *	view of its NSWindow. It is (0, 0) unless the toplevel is embedded
 *	in another Tk window.
 *
 * Results:
 *	The offsets are stored in *xPtr and *yPtr.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static void
GetViewOffset(
    Tk_Window top,		/* A mapped toplevel. */
    NSWindow *window,		/* The NSWindow that displays it. */
    int *xPtr, int *yPtr)	/* Receive the offsets. */
{
    Tk_Window owner = Tk_MacOSXGetTkWindow(window);

    *xPtr = *yPtr = 0;
    if (owner != NULL && owner != top) {
	int topX, topY, ownerX, ownerY;

	Tk_GetRootCoords(top, &topX, &topY);
	Tk_GetRootCoords(owner, &ownerX, &ownerY);
	*xPtr = topX - ownerX;
	*yPtr = topY - ownerY;
    }
}

/*
 *----------------------------------------------------------------------
 *
 * GrabFromBacking --
 *
 *	Copies a rectangle, given in Tk pixels relative to the upper left
 *	corner of a content view, out of the view's backing bitmap,
 *	converting it to sRGB.
 *
 * Results:
 *	A standard Tcl result; on success *bufPtr holds the pixels.
 *
 * Side effects:
 *	Memory is allocated for the pixels.
 *
 *----------------------------------------------------------------------
 */

static int
GrabFromBacking(
    Tcl_Interp *interp,		/* For error reporting. */
    CGContextRef backing,	/* Backing bitmap of the content view. */
    CGFloat backingScale,	/* Backing pixels per Tk pixel. */
    int x, int y,		/* Upper left corner of the rectangle in
				 * the view. */
    int width, int height,	/* Size of the rectangle. */
    int flags,			/* TKCAPTURE_* bits. */
    TkCaptureBuffer *bufPtr)	/* Receives the pixels. */
{
    CGFloat scale = (flags & TKCAPTURE_DEVICE_SCALE) ? backingScale : 1.0;
    int imageWidth = (int) lround(width * scale);
    int imageHeight = (int) lround(height * scale);
    CGImageRef full;
    CGColorSpaceRef colorSpace;
    CGContextRef target;
    CGRect rect;

    full = CGBitmapContextCreateImage(backing);
    if (full == NULL) {
	return TkCaptureFailed(interp, "FAILED",
		"can't capture the window: no image of its backing bitmap");
    }
    if (TkCaptureAllocBuffer(interp, bufPtr, imageWidth, imageHeight,
	    (double) scale) != TCL_OK) {
	CGImageRelease(full);
	return TCL_ERROR;
    }

    /*
     * Drawing into an sRGB context converts from the display's color
     * space, in which the backing bitmap is kept. With alpha last and
     * the default byte order, a pixel is stored as red, green, blue,
     * alpha. The backing bitmap is opaque, so premultiplication changes
     * nothing; what the image does not cover keeps alpha 0.
     */

    colorSpace = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    target = CGBitmapContextCreate(bufPtr->pixels, (size_t) imageWidth,
	    (size_t) imageHeight, 8, (size_t) bufPtr->pitch, colorSpace,
	    (CGBitmapInfo) kCGImageAlphaPremultipliedLast);
    CGColorSpaceRelease(colorSpace);
    if (target == NULL) {
	CGImageRelease(full);
	TkCaptureFreeBuffer(bufPtr);
	return TkCaptureFailed(interp, "FAILED",
		"can't capture the window: no bitmap context");
    }

    /*
     * Place the whole backing image so that the requested rectangle
     * fills the target. The image starts at the top of the view, but
     * the target's origin is its lower left corner, so the image's top
     * goes y Tk pixels above the target's. Interpolation is off so that
     * a Tk pixel keeps its exact color when a Retina bitmap is reduced
     * to logical size.
     */

    rect.size.width = CGImageGetWidth(full) * scale / backingScale;
    rect.size.height = CGImageGetHeight(full) * scale / backingScale;
    rect.origin.x = -x * scale;
    rect.origin.y = imageHeight + y * scale - rect.size.height;
    bufPtr->complete = (rect.origin.x <= 0.0 && rect.origin.y <= 0.0
	    && rect.origin.x + rect.size.width >= imageWidth
	    && rect.origin.y + rect.size.height >= imageHeight);
    CGContextSetInterpolationQuality(target, kCGInterpolationNone);
    CGContextSetBlendMode(target, kCGBlendModeCopy);
    CGContextDrawImage(target, rect, full);
    CGContextFlush(target);
    CGContextRelease(target);
    CGImageRelease(full);
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * MaskShift --
 *
 *	Finds the position of an 8-bit channel in a pixel word.
 *
 * Results:
 *	The number of bits below the mask, or -1 if the mask is not eight
 *	contiguous bits inside 32 bits.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static int
MaskShift(
    unsigned long mask)		/* Channel mask of an XImage. */
{
    int shift;

    for (shift = 0; shift <= 24; shift++) {
	if (mask == (0xFFUL << shift)) {
	    return shift;
	}
    }
    return -1;
}

/*
 *----------------------------------------------------------------------
 *
 * GrabWithXGetImage --
 *
 *	Reads a rectangle of a toplevel through Tk's emulation of
 *	XGetImage, which delivers one pixel per Tk pixel in the color
 *	space of the display.
 *
 * Results:
 *	A standard Tcl result; on success *bufPtr holds the pixels.
 *
 * Side effects:
 *	Memory is allocated for the pixels.
 *
 *----------------------------------------------------------------------
 */

static int
GrabWithXGetImage(
    Tcl_Interp *interp,		/* For error reporting. */
    Tk_Window top,		/* A mapped toplevel. */
    int x, int y,		/* Upper left corner of the rectangle,
				 * relative to the toplevel. */
    int width, int height,	/* Size of the rectangle. */
    TkCaptureBuffer *bufPtr)	/* Receives the pixels. */
{
    XImage *imagePtr;
    int row, col, redShift, greenShift, blueShift, lsbFirst;

    imagePtr = XGetImage(Tk_Display(top), Tk_WindowId(top), x, y,
	    (unsigned) width, (unsigned) height, AllPlanes, ZPixmap);
    if (imagePtr == NULL) {
	return TkCaptureFailed(interp, "FAILED",
		"can't capture the window: XGetImage failed");
    }
    redShift = MaskShift(imagePtr->red_mask);
    greenShift = MaskShift(imagePtr->green_mask);
    blueShift = MaskShift(imagePtr->blue_mask);
    if (imagePtr->bits_per_pixel != 32 || imagePtr->width < width
	    || imagePtr->height < height
	    || redShift < 0 || greenShift < 0 || blueShift < 0) {
	XDestroyImage(imagePtr);
	return TkCaptureFailed(interp, "UNSUPPORTED",
		"can't capture the window: unexpected image format");
    }
    if (TkCaptureAllocBuffer(interp, bufPtr, width, height, 1.0) != TCL_OK) {
	XDestroyImage(imagePtr);
	return TCL_ERROR;
    }

    /*
     * Tk's XGetImage delivers 32-bit words in the byte order and with the
     * 8-bit channel masks that the image declares: 0xAARRGGBB, least
     * significant byte first, so the bytes blue, green, red, alpha.
     */

    lsbFirst = (imagePtr->byte_order == LSBFirst);
    for (row = 0; row < height; row++) {
	const unsigned char *src = (const unsigned char *) imagePtr->data
		+ (size_t) row * imagePtr->bytes_per_line;
	unsigned char *dst = bufPtr->pixels + (size_t) row * bufPtr->pitch;

	for (col = 0; col < width; col++, src += 4, dst += 4) {
	    unsigned long pixel;

	    if (lsbFirst) {
		pixel = (unsigned long) src[0] | ((unsigned long) src[1] << 8)
			| ((unsigned long) src[2] << 16)
			| ((unsigned long) src[3] << 24);
	    } else {
		pixel = (unsigned long) src[3] | ((unsigned long) src[2] << 8)
			| ((unsigned long) src[1] << 16)
			| ((unsigned long) src[0] << 24);
	    }
	    dst[0] = (unsigned char) (pixel >> redShift);
	    dst[1] = (unsigned char) (pixel >> greenShift);
	    dst[2] = (unsigned char) (pixel >> blueShift);
	    dst[3] = 255;
	}
    }
    XDestroyImage(imagePtr);
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * TkCapturePlatformGrab --
 *
 *	See tkCapture.h.
 *
 *----------------------------------------------------------------------
 */

int
TkCapturePlatformGrab(
    Tcl_Interp *interp,		/* For error reporting. */
    Tk_Window top,		/* A mapped toplevel. */
    int x, int y,		/* Upper left corner of the rectangle,
				 * relative to the toplevel. */
    int width, int height,	/* Size of the rectangle. */
    int flags,			/* TKCAPTURE_* bits. */
    TkCaptureBuffer *bufPtr)	/* Receives the pixels. */
{
    NSWindow *window = (NSWindow *)
	    Tk_MacOSXGetNSWindowForDrawable(Tk_WindowId(top));
    NSView *view;
    CGContextRef backing;
    CGFloat backingScale;
    int offsetX, offsetY, hasBacking;

    if (window == nil) {
	return TkCaptureFailed(interp, "FAILED",
		"can't capture the window: it has no NSWindow");
    }
    if ([window isMiniaturized]) {
	return TkCaptureFailed(interp, "ICONIFIED",
		"the window is iconified");
    }

    view = [window contentView];
    backing = GetBackingContext(interp, view, &hasBacking);
    if (!hasBacking) {
	return GrabWithXGetImage(interp, top, x, y, width, height, bufPtr);
    }

    /*
     * The backing bitmap holds layer.contentsScale pixels per Tk pixel;
     * that is how Tk itself sizes it.
     */

    backingScale = (view.layer != nil) ? view.layer.contentsScale
	    : [window backingScaleFactor];
    if (backingScale <= 0.0) {
	backingScale = 1.0;
    }

    /*
     * Tk draws a window only into its backing bitmap, which it creates
     * for the window's screen. A window that had no screen then has no
     * bitmap and shows nothing (see
     * macos-window-without-backing-bitmap.md in the tk-bugs notes): the
     * image stays transparent, and the generic code reports it as off
     * screen.
     */

    if (backing == NULL) {
	CGFloat scale = (flags & TKCAPTURE_DEVICE_SCALE) ? backingScale : 1.0;

	if (TkCaptureAllocBuffer(interp, bufPtr, (int) lround(width * scale),
		(int) lround(height * scale), (double) scale) != TCL_OK) {
	    return TCL_ERROR;
	}
	bufPtr->complete = 0;
	return TCL_OK;
    }
    GetViewOffset(top, window, &offsetX, &offsetY);
    return GrabFromBacking(interp, backing, backingScale, x + offsetX,
	    y + offsetY, width, height, flags, bufPtr);
}

/*
 * Local Variables:
 * mode: objc
 * c-basic-offset: 4
 * fill-column: 78
 * End:
 */
