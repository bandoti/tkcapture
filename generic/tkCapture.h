/*
 * tkCapture.h --
 *
 *	Internal declarations shared by the generic and the per-platform
 *	parts of the tkcapture extension.
 *
 * Copyright © 2026 Bandoti Ltd.
 *
 * See the file "license.terms" for information on usage and redistribution of
 * this file, and for a DISCLAIMER OF ALL WARRANTIES.
 */

#ifndef _TKCAPTURE_H
#define _TKCAPTURE_H

#include <tcl.h>
#include <tk.h>

#ifndef MAX
#   define MAX(a,b) (((a) > (b)) ? (a) : (b))
#endif
#ifndef MIN
#   define MIN(a,b) (((a) < (b)) ? (a) : (b))
#endif

/*
 * A captured rectangle of pixels.
 */

typedef struct TkCaptureBuffer {
    unsigned char *pixels;	/* RGBA bytes, 8 bits per channel, not
				 * premultiplied. Allocated by
				 * TkCaptureAllocBuffer. */
    int width, height;		/* Size in image pixels. */
    int pitch;			/* Bytes per row. */
    double scale;		/* Image pixels per Tk pixel. */
    int complete;		/* 0 if some pixels could not be read and
				 * are transparent, 1 otherwise. */
} TkCaptureBuffer;

/*
 * Flag bits for the platform hooks.
 */

#define TKCAPTURE_DEVICE_SCALE	1	/* One image pixel per backing-store
					 * pixel instead of one per Tk
					 * pixel. */
#define TKCAPTURE_MENUBAR	2	/* The capturable area includes the
					 * toplevel's menubar. */

/*
 * Platform hooks, one implementation per windowing system.
 *
 * TkCapturePlatformMenubarHeight returns the height in Tk pixels of the
 * menubar shown directly above the client area of the toplevel 'top', or
 * 0 if there is none that can be captured.
 *
 * TkCapturePlatformGrab captures the rectangle (x, y, width, height),
 * given in Tk pixels relative to the client origin of the mapped
 * toplevel 'top'. The rectangle is not empty and lies inside the client
 * area, extended upwards by the menubar height when TKCAPTURE_MENUBAR is
 * set. On success the function fills *bufPtr through
 * TkCaptureAllocBuffer and returns TCL_OK: pixels that could not be read
 * have alpha 0 and clear bufPtr->complete, all others are opaque. On
 * failure it returns TCL_ERROR with a message and an error code in
 * interp, and *bufPtr holds no allocated pixels.
 *
 * TkCapturePlatformExpose delivers the Expose events that captures in
 * the current thread have caused and the event loop has not delivered
 * yet: rendering a toplevel can make the windowing system ask its
 * windows to repaint. 'top' is the mapped toplevel about to be captured
 * again. It returns the number of events delivered.
 */

MODULE_SCOPE int	TkCapturePlatformMenubarHeight(Tk_Window top);
MODULE_SCOPE int	TkCapturePlatformExpose(Tk_Window top);
MODULE_SCOPE int	TkCapturePlatformGrab(Tcl_Interp *interp,
			    Tk_Window top, int x, int y, int width,
			    int height, int flags, TkCaptureBuffer *bufPtr);

/*
 * Helpers for the platform code (tkCapture.c).
 */

MODULE_SCOPE int	TkCaptureAllocBuffer(Tcl_Interp *interp,
			    TkCaptureBuffer *bufPtr, int width, int height,
			    double scale);
MODULE_SCOPE void	TkCaptureFreeBuffer(TkCaptureBuffer *bufPtr);
MODULE_SCOPE void	TkCaptureClearRect(TkCaptureBuffer *bufPtr, int x1,
			    int y1, int x2, int y2);
MODULE_SCOPE int	TkCaptureFailed(Tcl_Interp *interp,
			    const char *detail, const char *message);
MODULE_SCOPE int	TkCaptureForceFallback(Tcl_Interp *interp);

/*
 * XImage conversion (tkCaptureConvert.c).
 */

MODULE_SCOPE int	TkCaptureXImageToRGBA(Tcl_Interp *interp,
			    Display *display, XImage *imagePtr,
			    Visual *visual, Colormap colormap,
			    unsigned char *dst, int pitch);

#endif /* _TKCAPTURE_H */

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * End:
 */
