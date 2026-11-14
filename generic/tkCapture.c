/*
 * tkCapture.c --
 *
 *	Generic part of the tkcapture extension: the ::tkcapture::_capture
 *	command, which captures a rectangle of a Tk window into a photo
 *	image, and the package initialization. The public commands are
 *	defined in library/tkcapture.tcl on top of this primitive.
 *
 * Copyright © 2026 Bandoti Ltd.
 *
 * See the file "license.terms" for information on usage and redistribution of
 * this file, and for a DISCLAIMER OF ALL WARRANTIES.
 */

#include "tkCapture.h"
#include <limits.h>
#include <string.h>

#ifndef PACKAGE_VERSION
#   define PACKAGE_VERSION "1.0"
#endif

/*
 * Build configuration, reported by ::tkcapture::pkgconfig.
 */

static const Tcl_Config configuration[] = {
#ifdef HAVE_XCOMPOSITE
    {"xcomposite", "1"},	/* X11 reads through the Composite extension. */
#else
    {"xcomposite", "0"},
#endif
    {NULL, NULL}
};

/*
 * Forward declarations for functions defined in this file.
 */

static Tcl_ObjCmdProc2	CaptureObjCmd;
static void		DictPut(Tcl_Obj *dictObj, const char *key,
			    Tcl_Obj *valueObj);
static Tcl_ObjCmdProc2	ExposeObjCmd;
static int		GetRegion(Tcl_Interp *interp, Tcl_Obj *regionObj,
			    int *coords);
static Tk_Window	GetViewableToplevel(Tk_Window tkwin);
static int		GetVisibleArea(Tk_Window top, int *areaPtr);
static void		GetWindowOffset(Tk_Window tkwin, Tk_Window top,
			    int *xPtr, int *yPtr);
static int		Init(Tcl_Interp *interp);
static int		IsEmpty(const TkCaptureBuffer *bufPtr);
static int		ReplacePhoto(Tcl_Interp *interp,
			    Tk_PhotoHandle photo,
			    const TkCaptureBuffer *bufPtr);

/*
 *----------------------------------------------------------------------
 *
 * TkCaptureAllocBuffer --
 *
 *	Allocates the pixel storage of a capture buffer for an image of
 *	width x height pixels, sets every pixel to transparent black and
 *	marks the buffer as complete.
 *
 * Results:
 *	TCL_OK, or TCL_ERROR with a message in interp if the memory could
 *	not be allocated.
 *
 * Side effects:
 *	Memory is allocated; release it with TkCaptureFreeBuffer.
 *
 *----------------------------------------------------------------------
 */

int
TkCaptureAllocBuffer(
    Tcl_Interp *interp,		/* For error reporting. */
    TkCaptureBuffer *bufPtr,	/* Buffer to initialize. */
    int width, int height,	/* Size in image pixels, both positive. */
    double scale)		/* Image pixels per Tk pixel. */
{
    size_t size;

    bufPtr->pixels = NULL;
    if (width <= 0 || height <= 0 || width > INT_MAX / 4
	    || (size_t) height > ((size_t) -1) / (4 * (size_t) width)) {
	goto fail;
    }
    size = (size_t) width * 4 * (size_t) height;
    bufPtr->pixels = (unsigned char *) Tcl_AttemptAlloc(size);
    if (bufPtr->pixels == NULL) {
	goto fail;
    }
    memset(bufPtr->pixels, 0, size);
    bufPtr->width = width;
    bufPtr->height = height;
    bufPtr->pitch = 4 * width;
    bufPtr->scale = scale;
    bufPtr->complete = 1;
    return TCL_OK;

  fail:
    Tcl_SetObjResult(interp, Tcl_ObjPrintf(
	    "not enough memory for a %dx%d capture", width, height));
    Tcl_SetErrorCode(interp, "TK", "MALLOC", (char *)NULL);
    return TCL_ERROR;
}

/*
 *----------------------------------------------------------------------
 *
 * TkCaptureFreeBuffer --
 *
 *	Releases the pixel storage of a capture buffer.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Memory is freed.
 *
 *----------------------------------------------------------------------
 */

void
TkCaptureFreeBuffer(
    TkCaptureBuffer *bufPtr)	/* Buffer to release. */
{
    if (bufPtr->pixels != NULL) {
	Tcl_Free(bufPtr->pixels);
	bufPtr->pixels = NULL;
    }
}

/*
 *----------------------------------------------------------------------
 *
 * TkCaptureClearRect --
 *
 *	Makes the pixels of a capture buffer transparent that lie inside
 *	the rectangle from (x1, y1) to (x2, y2), lower right corner
 *	exclusive, given in buffer coordinates.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The buffer is marked incomplete if any of its pixels is cleared.
 *
 *----------------------------------------------------------------------
 */

void
TkCaptureClearRect(
    TkCaptureBuffer *bufPtr,	/* Buffer to modify. */
    int x1, int y1,		/* Upper left corner. */
    int x2, int y2)		/* Lower right corner, exclusive. */
{
    int y;

    x1 = MAX(x1, 0);
    y1 = MAX(y1, 0);
    x2 = MIN(x2, bufPtr->width);
    y2 = MIN(y2, bufPtr->height);
    if (x2 <= x1 || y2 <= y1) {
	return;
    }
    for (y = y1; y < y2; y++) {
	memset(bufPtr->pixels + (size_t) y * bufPtr->pitch + (size_t) x1 * 4,
		0, (size_t) (x2 - x1) * 4);
    }
    bufPtr->complete = 0;
}

/*
 *----------------------------------------------------------------------
 *
 * TkCaptureForceFallback --
 *
 *	Tells whether the test suite asks the platform code to skip its
 *	primary way of reading pixels, by setting the variable
 *	::tkcapture::forceFallback to true.
 *
 * Results:
 *	1 if the fallback is to be used, 0 otherwise.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

int
TkCaptureForceFallback(
    Tcl_Interp *interp)		/* Interpreter of the capture. */
{
    Tcl_Obj *forceObj = Tcl_GetVar2Ex(interp, "::tkcapture::forceFallback",
	    NULL, TCL_GLOBAL_ONLY);
    int force = 0;

    return forceObj != NULL
	    && Tcl_GetBooleanFromObj(NULL, forceObj, &force) == TCL_OK
	    && force;
}

/*
 *----------------------------------------------------------------------
 *
 * TkCaptureFailed --
 *
 *	Reports a capture error: leaves message in interp and sets the
 *	error code to {TK CAPTURE detail}.
 *
 * Results:
 *	Always TCL_ERROR.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

int
TkCaptureFailed(
    Tcl_Interp *interp,		/* Receives the error. */
    const char *detail,		/* Third element of the error code. */
    const char *message)	/* Human-readable message. */
{
    Tcl_SetObjResult(interp, Tcl_NewStringObj(message, TCL_INDEX_NONE));
    Tcl_SetErrorCode(interp, "TK", "CAPTURE", detail, (char *)NULL);
    return TCL_ERROR;
}

/*
 *----------------------------------------------------------------------
 *
 * GetRegion --
 *
 *	Parses a region, a list of four integers {x1 y1 x2 y2}.
 *
 * Results:
 *	TCL_OK with the integers in coords[0..3], or TCL_ERROR with a
 *	message and the error code {TK CAPTURE REGION} in interp.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static int
GetRegion(
    Tcl_Interp *interp,		/* For error reporting. */
    Tcl_Obj *regionObj,		/* The region to parse. */
    int *coords)		/* Receives x1, y1, x2, y2. */
{
    Tcl_Size objc, i;
    Tcl_Obj **objv;

    if (Tcl_ListObjGetElements(NULL, regionObj, &objc, &objv) != TCL_OK
	    || objc != 4) {
	goto bad;
    }
    for (i = 0; i < 4; i++) {
	if (Tcl_GetIntFromObj(NULL, objv[i], &coords[i]) != TCL_OK) {
	    goto bad;
	}
    }
    return TCL_OK;

  bad:
    Tcl_SetObjResult(interp, Tcl_ObjPrintf(
	    "bad region \"%s\": must be a list of four integers x1 y1 x2 y2",
	    Tcl_GetString(regionObj)));
    Tcl_SetErrorCode(interp, "TK", "CAPTURE", "REGION", (char *)NULL);
    return TCL_ERROR;
}

/*
 *----------------------------------------------------------------------
 *
 * GetViewableToplevel --
 *
 *	Finds the toplevel of a window, provided that the window and all
 *	its ancestors up to the toplevel are mapped.
 *
 * Results:
 *	The toplevel, or NULL if the window is not viewable.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static Tk_Window
GetViewableToplevel(
    Tk_Window tkwin)		/* Window to start from. */
{
    Tk_Window w;

    for (w = tkwin; ; w = Tk_Parent(w)) {
	if (!Tk_IsMapped(w) || Tk_WindowId(w) == None) {
	    return NULL;
	}
	if (Tk_IsTopLevel(w)) {
	    return w;
	}
    }
}

/*
 *----------------------------------------------------------------------
 *
 * GetWindowOffset --
 *
 *	Computes the position of a window relative to its toplevel.
 *	Summing the positions along the parent chain is correct for every
 *	way a window can be managed (packed, placed, canvas window item,
 *	pane).
 *
 * Results:
 *	The offset is stored in *xPtr and *yPtr.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static void
GetWindowOffset(
    Tk_Window tkwin,		/* A window. */
    Tk_Window top,		/* Its toplevel. */
    int *xPtr, int *yPtr)	/* Receive the offset. */
{
    Tk_Window w;

    *xPtr = *yPtr = 0;
    for (w = tkwin; w != top; w = Tk_Parent(w)) {
	*xPtr += Tk_X(w) + Tk_Changes(w)->border_width;
	*yPtr += Tk_Y(w) + Tk_Changes(w)->border_width;
    }
}

/*
 *----------------------------------------------------------------------
 *
 * GetVisibleArea --
 *
 *	Computes the part of a toplevel's client area that can be
 *	displayed. For an ordinary toplevel that is the whole client
 *	area. A toplevel that is embedded in a container window of this
 *	process shows only what lies inside the container and inside
 *	every window around the container, up to and across further
 *	levels of embedding.
 *
 * Results:
 *	1 if the area is known; it is stored in areaPtr[0..3] as x1, y1,
 *	x2, y2 in the coordinates of top, and is empty if x2 <= x1 or
 *	y2 <= y1. 0 if top is embedded in a window of another
 *	application, whose geometry is not known; areaPtr then holds the
 *	whole client area. -1 if a container, or a window around one, is
 *	not mapped, so that top is not displayed at all.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static int
GetVisibleArea(
    Tk_Window top,		/* A toplevel. */
    int *areaPtr)		/* Receives x1, y1, x2, y2. */
{
    Tk_Window current = top, w;
    int originX = 0, originY = 0;
				/* Origin of the window being examined, in
				 * the coordinates of top. */

    areaPtr[0] = 0;
    areaPtr[1] = 0;
    areaPtr[2] = Tk_Width(top);
    areaPtr[3] = Tk_Height(top);

    while (Tk_IsEmbedded(current)) {
	Tk_Window container = Tk_GetOtherWindow(current);

	if (container == NULL) {
	    return 0;
	}

	/*
	 * The embedded toplevel lies at the origin of its container.
	 * Clip to the container and then to each of its ancestors, whose
	 * origins are found by stepping back along the parent chain.
	 */

	for (w = container; ; w = Tk_Parent(w)) {
	    if (!Tk_IsMapped(w)) {
		return -1;
	    }
	    areaPtr[0] = MAX(areaPtr[0], originX);
	    areaPtr[1] = MAX(areaPtr[1], originY);
	    areaPtr[2] = MIN(areaPtr[2], originX + Tk_Width(w));
	    areaPtr[3] = MIN(areaPtr[3], originY + Tk_Height(w));
	    if (Tk_IsTopLevel(w)) {
		break;
	    }
	    originX -= Tk_X(w) + Tk_Changes(w)->border_width;
	    originY -= Tk_Y(w) + Tk_Changes(w)->border_width;
	}
	current = w;
    }
    return 1;
}

/*
 *----------------------------------------------------------------------
 *
 * IsEmpty --
 *
 *	Tells whether a capture buffer holds no pixel that could be read.
 *
 * Results:
 *	1 if every pixel is transparent, 0 otherwise.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static int
IsEmpty(
    const TkCaptureBuffer *bufPtr)
				/* Buffer to examine. */
{
    int x, y;

    if (bufPtr->complete) {
	return 0;
    }
    for (y = 0; y < bufPtr->height; y++) {
	const unsigned char *alphaPtr = bufPtr->pixels
		+ (size_t) y * bufPtr->pitch + 3;

	for (x = 0; x < bufPtr->width; x++, alphaPtr += 4) {
	    if (*alphaPtr != 0) {
		return 0;
	    }
	}
    }
    return 1;
}

/*
 *----------------------------------------------------------------------
 *
 * ReplacePhoto --
 *
 *	Replaces the contents of a photo image with the captured pixels.
 *	Afterwards the photo has exactly the size of the capture and no
 *	user-declared size.
 *
 * Results:
 *	A standard Tcl result.
 *
 * Side effects:
 *	The photo image changes; its instances are redisplayed.
 *
 *----------------------------------------------------------------------
 */

static int
ReplacePhoto(
    Tcl_Interp *interp,		/* For error reporting. */
    Tk_PhotoHandle photo,	/* Image to overwrite. */
    const TkCaptureBuffer *bufPtr)
				/* Pixels to store. */
{
    Tk_PhotoImageBlock block;

    block.pixelPtr = bufPtr->pixels;
    block.width = bufPtr->width;
    block.height = bufPtr->height;
    block.pitch = bufPtr->pitch;
    block.pixelSize = 4;
    block.offset[0] = 0;
    block.offset[1] = 1;
    block.offset[2] = 2;
    block.offset[3] = 3;

    /*
     * Tk_PhotoSetSize with an explicit size shrinks a larger photo but
     * leaves that size as the user-declared one, which would freeze the
     * photo's -width and -height. The second call, with zeros, keeps the
     * actual size and drops the declaration.
     */

    Tk_PhotoBlank(photo);
    if (Tk_PhotoSetSize(interp, photo, block.width, block.height) != TCL_OK
	    || Tk_PhotoSetSize(interp, photo, 0, 0) != TCL_OK) {
	return TCL_ERROR;
    }
    return Tk_PhotoPutBlock(interp, photo, &block, 0, 0, block.width,
	    block.height, TK_PHOTO_COMPOSITE_SET);
}

/*
 *----------------------------------------------------------------------
 *
 * DictPut --
 *
 *	Stores a value under a string key in an unshared dictionary.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The dictionary is modified.
 *
 *----------------------------------------------------------------------
 */

static void
DictPut(
    Tcl_Obj *dictObj,		/* Unshared dictionary. */
    const char *key,		/* Key to set. */
    Tcl_Obj *valueObj)		/* Value to store. */
{
    Tcl_DictObjPut(NULL, dictObj, Tcl_NewStringObj(key, TCL_INDEX_NONE),
	    valueObj);
}

/*
 *----------------------------------------------------------------------
 *
 * ExposeObjCmd --
 *
 *	Implements ::tkcapture::_expose:
 *
 *	    _expose window
 *
 *	Delivers the Expose events that capturing has caused, so that the
 *	areas the windowing system asked for are redrawn by the next idle
 *	callbacks, without running the event loop. window is the window
 *	about to be captured again.
 *
 * Results:
 *	A standard Tcl result. On success the interpreter result is the
 *	number of Expose events delivered; 0 if the window is not
 *	viewable.
 *
 * Side effects:
 *	Expose bindings may run.
 *
 *----------------------------------------------------------------------
 */

static int
ExposeObjCmd(
    void *dummy,		/* Not used. */
    Tcl_Interp *interp,		/* Current interpreter. */
    Tcl_Size objc,		/* Number of arguments. */
    Tcl_Obj *const objv[])	/* Argument objects. */
{
    Tk_Window mainWin, tkwin, top;
    int count = 0;
    (void) dummy;

    if (objc != 2) {
	Tcl_WrongNumArgs(interp, 1, objv, "window");
	return TCL_ERROR;
    }
    mainWin = Tk_MainWindow(interp);
    if (mainWin == NULL) {
	return TCL_ERROR;
    }
    tkwin = Tk_NameToWindow(interp, Tcl_GetString(objv[1]), mainWin);
    if (tkwin == NULL) {
	return TCL_ERROR;
    }
    top = GetViewableToplevel(tkwin);
    if (top != NULL) {
	count = TkCapturePlatformExpose(top);
    }
    Tcl_SetObjResult(interp, Tcl_NewWideIntObj(count));
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * CaptureObjCmd --
 *
 *	Implements ::tkcapture::_capture:
 *
 *	    _capture window photo region scale menubar nocomplain
 *
 *	Stores in the photo image the pixels that the toplevel of window
 *	displays in region, a list {x1 y1 x2 y2} in window's coordinates
 *	with an exclusive lower right corner. An empty region stands for
 *	the whole window. The region is clipped to the visible part of
 *	the client area of the toplevel, which is extended upwards by the
 *	menubar if menubar is true (window must be a toplevel then). It
 *	is an error if no pixel at all could be read, unless nocomplain
 *	is true. The command neither creates images nor enters the event
 *	loop.
 *
 *	The command can be used in a safe interpreter: it reads pixels of
 *	the interpreter's own windows only, and of those only what lies
 *	inside the container in which the interpreter's toplevel is
 *	embedded.
 *
 * Results:
 *	A standard Tcl result. On success the interpreter result is a
 *	dictionary describing the image: x and y (window coordinates of
 *	the image's upper left pixel), rootx and rooty (the same point in
 *	screen coordinates), width and height (size of the captured
 *	rectangle in Tk pixels), scale (image pixels per Tk pixel) and
 *	complete (false if some pixels could not be read and are
 *	transparent).
 *
 * Side effects:
 *	The photo image is overwritten.
 *
 *----------------------------------------------------------------------
 */

static int
CaptureObjCmd(
    void *dummy,		/* Not used. */
    Tcl_Interp *interp,		/* Current interpreter. */
    Tcl_Size objc,		/* Number of arguments. */
    Tcl_Obj *const objv[])	/* Argument objects. */
{
    static const char *const scaleNames[] = {"logical", "device", NULL};
    enum scales { SCALE_LOGICAL, SCALE_DEVICE };
    Tk_Window mainWin, tkwin, top;
    Tk_PhotoHandle photo;
    int region[4], scaleIndex, menubar, noComplain, flags = 0;
    int winX, winY, topX, topY, menuHeight = 0, area[4], areaKnown;
    int isSafe = Tcl_IsSafe(interp), code;
    Tcl_WideInt x1, y1, x2, y2;	/* Rectangle to capture, relative to top;
				 * wide so that translation cannot
				 * overflow. */
    TkCaptureBuffer buf;
    Tcl_Obj *resultObj;
    (void) dummy;

    if (objc != 7) {
	Tcl_WrongNumArgs(interp, 1, objv,
		"window photo region scale menubar nocomplain");
	return TCL_ERROR;
    }
    mainWin = Tk_MainWindow(interp);
    if (mainWin == NULL) {
	return TCL_ERROR;
    }
    tkwin = Tk_NameToWindow(interp, Tcl_GetString(objv[1]), mainWin);
    if (tkwin == NULL) {
	return TCL_ERROR;
    }
    photo = Tk_FindPhoto(interp, Tcl_GetString(objv[2]));
    if (photo == NULL) {
	Tcl_SetObjResult(interp, Tcl_ObjPrintf(
		"image \"%s\" does not exist or is not a photo image",
		Tcl_GetString(objv[2])));
	Tcl_SetErrorCode(interp, "TK", "LOOKUP", "PHOTO",
		Tcl_GetString(objv[2]), (char *)NULL);
	return TCL_ERROR;
    }
    if (Tcl_GetIndexFromObj(interp, objv[4], scaleNames, "scale", 0,
	    &scaleIndex) != TCL_OK
	    || Tcl_GetBooleanFromObj(interp, objv[5], &menubar) != TCL_OK
	    || Tcl_GetBooleanFromObj(interp, objv[6], &noComplain)
		    != TCL_OK) {
	return TCL_ERROR;
    }
    if (scaleIndex == SCALE_DEVICE) {
	flags |= TKCAPTURE_DEVICE_SCALE;
    }

    /*
     * A toplevel embedded in a container that is not displayed is not
     * viewable either: the surface behind it shows something else.
     */

    top = GetViewableToplevel(tkwin);
    areaKnown = (top != NULL) ? GetVisibleArea(top, area) : -1;
    if (areaKnown < 0) {
	Tcl_SetObjResult(interp, Tcl_ObjPrintf(
		"window \"%s\" is not viewable", Tk_PathName(tkwin)));
	Tcl_SetErrorCode(interp, "TK", "CAPTURE", "UNMAPPED", (char *)NULL);
	return TCL_ERROR;
    }
    if (!areaKnown && isSafe) {
	/*
	 * What surrounds the toplevel is unknown, so there is no telling
	 * whether only its own pixels would be read.
	 */

	return TkCaptureFailed(interp, "UNSUPPORTED", "can't capture a"
		" window that is embedded in another application from a"
		" safe interpreter");
    }

    /*
     * A safe interpreter has no menus, and what lies above its toplevel
     * belongs to its container.
     */

    if (menubar && !isSafe) {
	if (tkwin != top) {
	    Tcl_SetObjResult(interp, Tcl_ObjPrintf(
		    "can't capture the menubar: \"%s\" is not a toplevel",
		    Tk_PathName(tkwin)));
	    Tcl_SetErrorCode(interp, "TK", "CAPTURE", "MENUBAR",
		    (char *)NULL);
	    return TCL_ERROR;
	}
	menuHeight = TkCapturePlatformMenubarHeight(top);
	if (menuHeight > 0) {
	    flags |= TKCAPTURE_MENUBAR;
	}
    }

    GetWindowOffset(tkwin, top, &winX, &winY);
    if (Tcl_GetCharLength(objv[3]) == 0) {
	region[0] = 0;
	region[1] = -menuHeight;
	region[2] = Tk_Width(tkwin);
	region[3] = Tk_Height(tkwin);
    } else if (GetRegion(interp, objv[3], region) != TCL_OK) {
	return TCL_ERROR;
    }

    /*
     * Translate the region to toplevel coordinates and clip it to the
     * visible area, which the menubar extends upwards.
     */

    x1 = MAX((Tcl_WideInt) region[0] + winX, area[0]);
    y1 = MAX((Tcl_WideInt) region[1] + winY, area[1] - menuHeight);
    x2 = MIN((Tcl_WideInt) region[2] + winX, area[2]);
    y2 = MIN((Tcl_WideInt) region[3] + winY, area[3]);
    if (x2 <= x1 || y2 <= y1) {
	return TkCaptureFailed(interp, "EMPTY_REGION",
		"capture region is empty");
    }

    if (TkCapturePlatformRead(interp, top, (int) x1, (int) y1,
	    (int) (x2 - x1), (int) (y2 - y1), flags, &buf) != TCL_OK) {
	return TCL_ERROR;
    }
    if (!noComplain && IsEmpty(&buf)) {
	TkCaptureFreeBuffer(&buf);
	Tcl_SetObjResult(interp, Tcl_ObjPrintf(
		"no pixels of window \"%s\" could be read:"
		" the region is off screen or covered", Tk_PathName(tkwin)));
	Tcl_SetErrorCode(interp, "TK", "CAPTURE", "OFFSCREEN", (char *)NULL);
	return TCL_ERROR;
    }
    code = ReplacePhoto(interp, photo, &buf);
    TkCaptureFreeBuffer(&buf);
    if (code != TCL_OK) {
	return TCL_ERROR;
    }

    Tk_GetRootCoords(top, &topX, &topY);
    resultObj = Tcl_NewDictObj();
    DictPut(resultObj, "x", Tcl_NewWideIntObj(x1 - winX));
    DictPut(resultObj, "y", Tcl_NewWideIntObj(y1 - winY));
    DictPut(resultObj, "rootx", Tcl_NewWideIntObj(topX + x1));
    DictPut(resultObj, "rooty", Tcl_NewWideIntObj(topY + y1));
    DictPut(resultObj, "width", Tcl_NewWideIntObj(x2 - x1));
    DictPut(resultObj, "height", Tcl_NewWideIntObj(y2 - y1));
    DictPut(resultObj, "scale", Tcl_NewDoubleObj(buf.scale));
    DictPut(resultObj, "complete", Tcl_NewBooleanObj(buf.complete));
    Tcl_SetObjResult(interp, resultObj);
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * Init --
 *
 *	Creates ::tkcapture::_capture, ::tkcapture::_expose,
 *	::tkcapture::pkgconfig and the variable ::tkcapture::version in an
 *	interpreter. The package itself is provided by
 *	library/tkcapture.tcl, which the package index sources after
 *	loading the library.
 *
 * Results:
 *	A standard Tcl result.
 *
 * Side effects:
 *	A namespace, three commands and a variable are created.
 *
 *----------------------------------------------------------------------
 */

static int
Init(
    Tcl_Interp *interp)		/* Interpreter to initialize. */
{
    if (Tcl_InitStubs(interp, "9.0", 0) == NULL
	    || Tk_InitStubs(interp, "9.0", 0) == NULL) {
	return TCL_ERROR;
    }
    if (Tcl_CreateNamespace(interp, "::tkcapture", NULL, NULL) == NULL
	    && Tcl_FindNamespace(interp, "::tkcapture", NULL, 0) == NULL) {
	return TCL_ERROR;
    }
    Tcl_ResetResult(interp);
    if (Tcl_SetVar2Ex(interp, "::tkcapture::version", NULL,
	    Tcl_NewStringObj(PACKAGE_VERSION, TCL_INDEX_NONE),
	    TCL_LEAVE_ERR_MSG) == NULL) {
	return TCL_ERROR;
    }
    Tcl_CreateObjCommand2(interp, "::tkcapture::_capture", CaptureObjCmd,
	    NULL, NULL);
    Tcl_CreateObjCommand2(interp, "::tkcapture::_expose", ExposeObjCmd,
	    NULL, NULL);
    Tcl_RegisterConfig(interp, "tkcapture", configuration, "utf-8");
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * Tkcapture_Init, Tkcapture_SafeInit --
 *
 *	Entry points of the shared library for trusted and for safe
 *	interpreters. Both provide the same command, which restricts
 *	itself in a safe interpreter (see CaptureObjCmd).
 *
 * Results:
 *	A standard Tcl result.
 *
 * Side effects:
 *	See Init.
 *
 *----------------------------------------------------------------------
 */

DLLEXPORT int
Tkcapture_Init(
    Tcl_Interp *interp)		/* Interpreter to initialize. */
{
    return Init(interp);
}

DLLEXPORT int
Tkcapture_SafeInit(
    Tcl_Interp *interp)		/* Safe interpreter to initialize. */
{
    return Init(interp);
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * End:
 */
