/*
 * tkCaptureX11.c --
 *
 *	X11 backend of the tkcapture extension: reads the contents of a
 *	toplevel window back from the X server.
 *
 * Copyright © 2026 Bandoti Ltd.
 *
 * See the file "license.terms" for information on usage and redistribution of
 * this file, and for a DISCLAIMER OF ALL WARRANTIES.
 */

#include "tkCapture.h"
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#ifdef HAVE_XCOMPOSITE
#   include <X11/extensions/Xcomposite.h>
#endif
#include <stdio.h>

/*
 * Forward declarations for functions defined in this file.
 */

static int		ClipToAncestors(Display *display, Window source,
			    Window last, int *x1Ptr, int *y1Ptr, int *x2Ptr,
			    int *y2Ptr);
static Tk_ErrorProc	ErrorProc;
#ifdef HAVE_XCOMPOSITE
static Pixmap		GetFramePixmap(Display *display, Window source,
			    Window *framePtr, XWindowAttributes *attrsPtr,
			    int *xPtr, int *yPtr);
#endif
static Window		GetWrapper(Tk_Window top, int *menuHeightPtr);
static int		HasCompositor(Display *display, Screen *screen);
static void		MaskHidden(Display *display, Window source,
			    int composited, int x, int y,
			    TkCaptureBuffer *bufPtr);

/*
 *----------------------------------------------------------------------
 *
 * ErrorProc --
 *
 *	X error handler that records that an error happened.
 *
 * Results:
 *	Always 0: the error is handled.
 *
 * Side effects:
 *	Sets the int that clientData points to.
 *
 *----------------------------------------------------------------------
 */

static int
ErrorProc(
    void *clientData,		/* Points to the int error flag. */
    XErrorEvent *errEventPtr)	/* Not used. */
{
    (void) errEventPtr;
    *(int *) clientData = 1;
    return 0;
}

/*
 *----------------------------------------------------------------------
 *
 * GetWrapper --
 *
 *	Finds the wrapper window of a toplevel that shows a menubar. Tk
 *	puts every toplevel into a wrapper window of its own; a menubar
 *	is a second child of the wrapper, placed above the toplevel.
 *
 * Results:
 *	The wrapper window, with the height of the menubar stored in
 *	*menuHeightPtr; None if the toplevel has no menubar or no wrapper
 *	(an embedded toplevel).
 *
 * Side effects:
 *	Round trips to the X server.
 *
 *----------------------------------------------------------------------
 */

static Window
GetWrapper(
    Tk_Window top,		/* A mapped toplevel. */
    int *menuHeightPtr)		/* Receives the menubar height. */
{
    Display *display = Tk_Display(top);
    Window root, parent = None, child, *children = NULL;
    unsigned int numChildren, width, height, borderWidth, depth;
    int x, y, offsetX, offsetY, failed = 0;
    Tk_ErrorHandler handler;

    /*
     * The public API does not expose the wrapper, so it is recognized
     * by its geometry: the X parent of the toplevel is the wrapper if
     * the toplevel fills it except for a strip at the top.
     */

    handler = Tk_CreateErrorHandler(display, -1, -1, -1, ErrorProc, &failed);
    if (!XQueryTree(display, Tk_WindowId(top), &root, &parent, &children,
	    &numChildren)) {
	failed = 1;
    }
    if (children != NULL) {
	XFree(children);
    }
    if (!failed && parent != None && parent != root) {
	if (!XTranslateCoordinates(display, Tk_WindowId(top), parent, 0, 0,
		&offsetX, &offsetY, &child)
		|| !XGetGeometry(display, parent, &root, &x, &y, &width,
			&height, &borderWidth, &depth)) {
	    failed = 1;
	}
    } else {
	failed = 1;
    }
    XSync(display, False);
    Tk_DeleteErrorHandler(handler);

    if (failed || offsetX != 0 || offsetY <= 0
	    || (int) width != Tk_Width(top)
	    || (int) height != Tk_Height(top) + offsetY) {
	return None;
    }
    *menuHeightPtr = offsetY;
    return parent;
}

/*
 *----------------------------------------------------------------------
 *
 * ClipToAncestors --
 *
 *	Clips a rectangle, given in the coordinates of a window, to the
 *	bounds of every ancestor of the window up to and including 'last',
 *	or the root window if 'last' is None: to the part that the window
 *	can display inside 'last'.
 *
 * Results:
 *	1 with the clipped rectangle in *x1Ptr .. *y2Ptr (empty if
 *	*x2Ptr <= *x1Ptr or *y2Ptr <= *y1Ptr), 0 if the X server could
 *	not be asked.
 *
 * Side effects:
 *	Round trips to the X server.
 *
 *----------------------------------------------------------------------
 */

static int
ClipToAncestors(
    Display *display,		/* Display of the window. */
    Window source,		/* Window that the rectangle refers to. */
    Window last,		/* Outermost ancestor to clip to, or None. */
    int *x1Ptr, int *y1Ptr,	/* Upper left corner, updated. */
    int *x2Ptr, int *y2Ptr)	/* Lower right corner, exclusive, updated. */
{
    Window current = source, root, parent, *children;
    unsigned int numChildren;
    int originX = 0, originY = 0;
				/* Origin of the parent of current in the
				 * coordinates of source. */
    XWindowAttributes attrs;

    for (;;) {
	children = NULL;
	if (!XQueryTree(display, current, &root, &parent, &children,
		&numChildren)) {
	    return 0;
	}
	if (children != NULL) {
	    XFree(children);
	}
	if (parent == None || current == last) {
	    return 1;
	}
	if (!XGetWindowAttributes(display, current, &attrs)) {
	    return 0;
	}
	originX -= attrs.x + attrs.border_width;
	originY -= attrs.y + attrs.border_width;
	if (!XGetWindowAttributes(display, parent, &attrs)) {
	    return 0;
	}
	*x1Ptr = MAX(*x1Ptr, originX);
	*y1Ptr = MAX(*y1Ptr, originY);
	*x2Ptr = MIN(*x2Ptr, originX + attrs.width);
	*y2Ptr = MIN(*y2Ptr, originY + attrs.height);
	current = parent;
    }
}

#ifdef HAVE_XCOMPOSITE
/*
 *----------------------------------------------------------------------
 *
 * GetFramePixmap --
 *
 *	Finds the off-screen buffer in which a compositing manager keeps
 *	the contents of the child of the root window that holds a window.
 *	The buffer holds them wherever the frame is on the screen, and
 *	whatever covers it.
 *
 * Results:
 *	The buffer, to be released with XFreePixmap, or None if there is
 *	none. On success *framePtr is the child of the root and *attrsPtr
 *	holds its attributes, among them the visual and colormap of the
 *	buffer's pixels; *xPtr and *yPtr give the position of the window's
 *	origin in the buffer.
 *
 * Side effects:
 *	Round trips to the X server.
 *
 *----------------------------------------------------------------------
 */

static Pixmap
GetFramePixmap(
    Display *display,		/* Display of the window. */
    Window source,		/* The toplevel's window or its wrapper. */
    Window *framePtr,		/* Receives the child of the root. */
    XWindowAttributes *attrsPtr,
				/* Receives the attributes of the child. */
    int *xPtr, int *yPtr)	/* Receive the position of source. */
{
    Window frame = source, root, parent, child, *children;
    unsigned int numChildren;
    int eventBase, errorBase, major = 0, minor = 2, failed = 0;
    int x, y;			/* Position of source in the frame. */
    XWindowAttributes attrs;
    Pixmap pixmap = None;
    Tk_ErrorHandler handler;

    if (!XCompositeQueryExtension(display, &eventBase, &errorBase)
	    || !XCompositeQueryVersion(display, &major, &minor)
	    || (major == 0 && minor < 2)) {
	return None;
    }

    handler = Tk_CreateErrorHandler(display, -1, -1, -1, ErrorProc, &failed);
    for (;;) {
	children = NULL;
	if (!XQueryTree(display, frame, &root, &parent, &children,
		&numChildren)) {
	    failed = 1;
	    break;
	}
	if (children != NULL) {
	    XFree(children);
	}
	if (parent == root || parent == None) {
	    break;
	}
	frame = parent;
    }

    /*
     * The server refuses with BadMatch unless a compositing manager has
     * redirected the frame. The buffer includes the frame's border, and
     * its pixels are in the frame's visual, which the window manager may
     * have chosen differently from Tk's.
     */

    if (!failed && XGetWindowAttributes(display, frame, &attrs)
	    && XTranslateCoordinates(display, source, frame, 0, 0, &x, &y,
		    &child)) {
	pixmap = XCompositeNameWindowPixmap(display, frame);
    }
    XSync(display, False);
    Tk_DeleteErrorHandler(handler);
    if (failed || pixmap == None) {
	return None;
    }
    *framePtr = frame;
    *attrsPtr = attrs;
    *xPtr = x + attrs.border_width;
    *yPtr = y + attrs.border_width;
    return pixmap;
}
#endif /* HAVE_XCOMPOSITE */

/*
 *----------------------------------------------------------------------
 *
 * HasCompositor --
 *
 *	Tells whether a compositing manager runs on a screen. It then
 *	keeps the contents of every child of the root window in a buffer
 *	of its own, which other children of the root do not overwrite.
 *
 * Results:
 *	1 if there is a compositing manager, 0 otherwise.
 *
 * Side effects:
 *	A round trip to the X server.
 *
 *----------------------------------------------------------------------
 */

static int
HasCompositor(
    Display *display,		/* Display of the screen. */
    Screen *screen)		/* Screen to ask about. */
{
    char name[40];

    /*
     * By convention a compositing manager owns this selection.
     */

    snprintf(name, sizeof(name), "_NET_WM_CM_S%d",
	    XScreenNumberOfScreen(screen));
    return XGetSelectionOwner(display, XInternAtom(display, name, False))
	    != None;
}

/*
 *----------------------------------------------------------------------
 *
 * MaskHidden --
 *
 *	Makes those pixels of a capture transparent that the source
 *	window does not display itself because a window that is stacked
 *	above it, or above one of its ancestors, covers it. The X server
 *	leaves the contents of such areas undefined; in practice they
 *	hold the pixels of whatever is displayed there, which need not
 *	belong to this application. Children of the root do not cover
 *	each other if they were composited.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Round trips to the X server. See TkCaptureClearRect.
 *
 *----------------------------------------------------------------------
 */

static void
MaskHidden(
    Display *display,		/* Display of the source window. */
    Window source,		/* Window the pixels were read from. */
    int composited,		/* Whether a compositing manager keeps the
				 * children of the root apart. */
    int x, int y,		/* Position of the buffer's upper left
				 * pixel in the coordinates of source. */
    TkCaptureBuffer *bufPtr)	/* The pixels. */
{
    Window current = source, root, parent, dummy, *children;
    unsigned int numChildren, i;
    int originX = -x, originY = -y;
				/* Origin of current, later of its parent,
				 * in buffer coordinates. */
    XWindowAttributes attrs;

    for (;;) {
	children = NULL;
	if (!XQueryTree(display, current, &root, &parent, &children,
		&numChildren)) {
	    break;
	}
	if (children != NULL) {
	    XFree(children);
	}
	if (parent == None || !XGetWindowAttributes(display, current,
		&attrs)) {
	    break;
	}
	originX -= attrs.x + attrs.border_width;
	originY -= attrs.y + attrs.border_width;

	/*
	 * The children are listed from the bottom of the stacking order
	 * to the top: those after the current window can cover it.
	 */

	children = NULL;
	if (!XQueryTree(display, parent, &root, &dummy, &children,
		&numChildren)) {
	    break;
	}
	if (children != NULL) {
	    int siblingsCover = (parent != root || !composited);

	    /*
	     * Skip the siblings below current, and current itself.
	     */

	    for (i = 0; i < numChildren && children[i] != current; i++) {
	    }
	    for (i++; siblingsCover && i < numChildren; i++) {
		int coverX, coverY, coverWidth, coverHeight;

		if (!XGetWindowAttributes(display, children[i], &attrs)
			|| attrs.map_state != IsViewable
			|| attrs.class != InputOutput) {
		    continue;
		}
		coverX = originX + attrs.x;
		coverY = originY + attrs.y;
		coverWidth = attrs.width + 2 * attrs.border_width;
		coverHeight = attrs.height + 2 * attrs.border_width;
		TkCaptureClearRect(bufPtr, coverX, coverY, coverX + coverWidth,
			coverY + coverHeight);
	    }
	    XFree(children);
	}
	if (parent == root) {
	    break;
	}
	current = parent;
    }
}

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
 *	See tkCapture.h.
 *
 *----------------------------------------------------------------------
 */

int
TkCapturePlatformMenubarHeight(
    Tk_Window top)		/* A mapped toplevel. */
{
    int menuHeight = 0;

    return (GetWrapper(top, &menuHeight) != None) ? menuHeight : 0;
}

/*
 *----------------------------------------------------------------------
 *
 * TkCapturePlatformRead --
 *
 *	See tkCapture.h.
 *
 *----------------------------------------------------------------------
 */

int
TkCapturePlatformRead(
    Tcl_Interp *interp,		/* For error reporting. */
    Tk_Window top,		/* A mapped toplevel. */
    int x, int y,		/* Upper left corner of the rectangle,
				 * relative to the toplevel. */
    int width, int height,	/* Size of the rectangle. */
    int flags,			/* TKCAPTURE_* bits. */
    TkCaptureBuffer *bufPtr)	/* Receives the pixels. */
{
    Display *display = Tk_Display(top);
    Screen *screen = Tk_Screen(top);
    Window source = Tk_WindowId(top);
    int sourceY = y;		/* y in the coordinates of source. */
    int x1, y1, x2, y2;		/* Part of the rectangle that source can
				 * display, in its coordinates. */
    Drawable drawable;		/* What the pixels are read from. */
    int drawX = 0, drawY = 0;	/* Origin of source in drawable. */
    Pixmap pixmap = None;	/* The frame's off-screen buffer. */
    Visual *visual = Tk_Visual(top);
				/* Visual of the pixels in drawable. */
    Colormap colormap = Tk_Colormap(top);
				/* Colormap of the pixels in drawable. */
    Window last = None;		/* Outermost window to clip to; None for
				 * the root. */
    int composited, visible, failed = 0, code = TCL_OK;
    Tk_ErrorHandler handler;
    XImage *imagePtr = NULL;

    if (flags & TKCAPTURE_MENUBAR) {
	/*
	 * The menubar is not part of the toplevel's own X window: read
	 * from the wrapper, in which the toplevel lies below the menubar.
	 */

	int menuHeight = 0;
	Window wrapper = GetWrapper(top, &menuHeight);

	if (wrapper != None) {
	    source = wrapper;
	    sourceY = y + menuHeight;
	}
    }

    if (TkCaptureAllocBuffer(interp, bufPtr, width, height, 1.0) != TCL_OK) {
	return TCL_ERROR;
    }

    /*
     * Under a compositing manager the frame's off-screen buffer holds the
     * window even where it is off the screen or covered. Without one,
     * the pixels come from the window, which holds only what is on the
     * screen.
     */

#ifdef HAVE_XCOMPOSITE
    if (!TkCaptureForceFallback(interp)) {
	XWindowAttributes frameAttrs;

	pixmap = GetFramePixmap(display, source, &last, &frameAttrs, &drawX,
		&drawY);
	if (pixmap != None) {
	    visual = frameAttrs.visual;
	    colormap = frameAttrs.colormap;
	}
    }
#endif
    if (pixmap != None) {
	drawable = pixmap;
	composited = 1;
    } else {
	drawable = source;
	composited = HasCompositor(display, screen);
    }

    /*
     * GetImage fails with BadMatch unless the whole rectangle lies inside
     * the drawable: for a window, on the screen and, under a compositing
     * manager, inside its off-screen buffer. So only the part inside the
     * ancestors is read; the rest keeps alpha 0. The geometry comes from
     * the server because Tk's can lag behind the window manager.
     */

    handler = Tk_CreateErrorHandler(display, -1, -1, -1, ErrorProc, &failed);
    x1 = x;
    y1 = sourceY;
    x2 = x + width;
    y2 = sourceY + height;
    if (!ClipToAncestors(display, source, last, &x1, &y1, &x2, &y2)) {
	failed = 1;
    }
    visible = (x2 > x1 && y2 > y1);
    if (!failed && visible) {
	imagePtr = XGetImage(display, drawable, x1 + drawX, y1 + drawY,
		(unsigned) (x2 - x1), (unsigned) (y2 - y1), AllPlanes,
		ZPixmap);
    }
    if (pixmap != None) {
	XFreePixmap(display, pixmap);
    }
    if (failed || (visible && imagePtr == NULL)) {
	XSync(display, False);
	Tk_DeleteErrorHandler(handler);
	if (imagePtr != NULL) {
	    XDestroyImage(imagePtr);
	}
	TkCaptureFreeBuffer(bufPtr);
	return TkCaptureFailed(interp, "FAILED",
		"the X server could not provide the window contents");
    }
    bufPtr->complete = (x1 == x && y1 == sourceY && x2 == x + width
	    && y2 == sourceY + height);
    if (imagePtr != NULL) {
	unsigned char *dst = bufPtr->pixels
		+ (size_t) (y1 - sourceY) * bufPtr->pitch
		+ (size_t) (x1 - x) * 4;

	code = TkCaptureXImageToRGBA(interp, display, imagePtr, visual,
		colormap, dst, bufPtr->pitch);
	XDestroyImage(imagePtr);
	if (code == TCL_OK) {
	    /*
	     * Where other windows cover the source window, the image holds
	     * their pixels, possibly another application's: clear them.
	     */

	    MaskHidden(display, source, composited, x, sourceY, bufPtr);
	}
    }
    XSync(display, False);
    Tk_DeleteErrorHandler(handler);
    if (code != TCL_OK) {
	TkCaptureFreeBuffer(bufPtr);
    }
    return code;
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * End:
 */
