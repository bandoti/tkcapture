/*
 * tkCaptureWin.c --
 *
 *	Windows backend of the tkcapture extension: asks the desktop
 *	window manager for the rendered contents of the top-level window
 *	that holds a Tk toplevel.
 *
 * Copyright © 2026 Bandoti Ltd.
 *
 * See the file "license.terms" for information on usage and redistribution of
 * this file, and for a DISCLAIMER OF ALL WARRANTIES.
 */

#include "tkCapture.h"
#include <windows.h>
#include <dwmapi.h>
#include <tkPlatDecls.h>	/* Needs HWND from windows.h. */

#ifndef PW_RENDERFULLCONTENT
#   define PW_RENDERFULLCONTENT 0x00000002
#endif
#ifndef CAPTUREBLT
#   define CAPTUREBLT 0x40000000
#endif

/*
 * Paint requests made while PrintWindow renders a toplevel, recorded by
 * RecordPaint, one list per thread. Tk answers each with a queued Expose
 * event, which TkCapturePlatformExpose delivers without waiting for the
 * event loop.
 */

typedef struct PaintRecord {
    HWND hwnd;			/* Window asked to paint. */
    RECT rect;			/* Bounds of the area to paint. */
} PaintRecord;

typedef struct ThreadSpecificData {
    PaintRecord *records;	/* Tcl_Alloc'ed array. */
    int count, size;		/* Used and allocated elements. */
} ThreadSpecificData;

static Tcl_ThreadDataKey dataKey;

/*
 * Forward declarations for functions defined in this file.
 */

static int		CanRenderFullContent(void);
static void		MaskHidden(HWND client, HWND frame, int x, int y,
			    TkCaptureBuffer *bufPtr);
static LRESULT CALLBACK	RecordPaint(int code, WPARAM wParam,
			    LPARAM lParam);
static BOOL		RenderFrame(HWND frame, HDC memDC, int width,
			    int height);
static int		Win32Failed(Tcl_Interp *interp, const char *what);

/*
 *----------------------------------------------------------------------
 *
 * CanRenderFullContent --
 *
 *	Tells whether PrintWindow supports PW_RENDERFULLCONTENT, that is,
 *	whether the system is Windows 8.1 or later.
 *
 * Results:
 *	1 if the flag is supported, 0 otherwise.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static int
CanRenderFullContent(void)
{
    static int supported = -1;

    if (supported < 0) {
	/*
	 * RtlGetVersion reports the real version. GetVersionEx and
	 * VerifyVersionInfo report Windows 8 on later systems for
	 * executables whose manifest does not declare them compatible.
	 */

	typedef LONG (WINAPI RtlGetVersionProc)(OSVERSIONINFOW *);
	HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
	RtlGetVersionProc *rtlGetVersion = (ntdll == NULL) ? NULL
		: (RtlGetVersionProc *)(void *)
		GetProcAddress(ntdll, "RtlGetVersion");
	OSVERSIONINFOW info;

	ZeroMemory(&info, sizeof(info));
	info.dwOSVersionInfoSize = sizeof(info);
	if (rtlGetVersion != NULL && rtlGetVersion(&info) == 0) {
	    supported = (info.dwMajorVersion > 6
		    || (info.dwMajorVersion == 6 && info.dwMinorVersion >= 3));
	} else {
	    supported = 1;
	}
    }
    return supported;
}

/*
 *----------------------------------------------------------------------
 *
 * Win32Failed --
 *
 *	Reports the failure of a Win32 call as a capture error with the
 *	error code {TK CAPTURE FAILED}.
 *
 * Results:
 *	Always TCL_ERROR.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static int
Win32Failed(
    Tcl_Interp *interp,		/* Receives the error. */
    const char *what)		/* The operation that failed. */
{
    Tcl_SetObjResult(interp, Tcl_ObjPrintf(
	    "can't capture the window: %s failed (error %lu)", what,
	    (unsigned long) GetLastError()));
    Tcl_SetErrorCode(interp, "TK", "CAPTURE", "FAILED", (char *)NULL);
    return TCL_ERROR;
}

/*
 *----------------------------------------------------------------------
 *
 * RecordPaint --
 *
 *	WH_CALLWNDPROC hook, installed while PrintWindow runs: records
 *	each WM_PAINT sent to a Tk window, with the area to paint, before
 *	the window procedure validates it.
 *
 * Results:
 *	The result of the next hook.
 *
 * Side effects:
 *	The thread's list of records may grow.
 *
 *----------------------------------------------------------------------
 */

static LRESULT CALLBACK
RecordPaint(
    int code,			/* HC_ACTION: process the message. */
    WPARAM wParam,		/* Whether the current thread sent it. */
    LPARAM lParam)		/* The CWPSTRUCT of the message. */
{
    const CWPSTRUCT *msgPtr = (const CWPSTRUCT *) lParam;

    if (code == HC_ACTION && msgPtr->message == WM_PAINT
	    && Tk_HWNDToWindow(msgPtr->hwnd) != NULL) {
	ThreadSpecificData *tsdPtr = (ThreadSpecificData *)
		Tcl_GetThreadData(&dataKey, sizeof(ThreadSpecificData));
	RECT rect;

	if (GetUpdateRect(msgPtr->hwnd, &rect, FALSE)) {
	    PaintRecord *recPtr;

	    if (tsdPtr->count == tsdPtr->size) {
		tsdPtr->size = (tsdPtr->size == 0) ? 8 : 2 * tsdPtr->size;
		tsdPtr->records = (PaintRecord *) Tcl_Realloc(
			tsdPtr->records, tsdPtr->size * sizeof(PaintRecord));
	    }
	    recPtr = &tsdPtr->records[tsdPtr->count++];
	    recPtr->hwnd = msgPtr->hwnd;
	    recPtr->rect = rect;
	}
    }
    return CallNextHookEx(NULL, code, wParam, lParam);
}

/*
 *----------------------------------------------------------------------
 *
 * TkCapturePlatformExpose --
 *
 *	See tkCapture.h. PrintWindow asks the windows of a toplevel to
 *	paint what they have not painted since they were shown, such as
 *	parts that were off screen. Tk answers by queueing Expose events,
 *	which only the event loop would deliver; they are delivered here
 *	instead, so that nothing else in the event queue is processed. The
 *	queued events arrive later and cause redundant redraws only.
 *
 *----------------------------------------------------------------------
 */

int
TkCapturePlatformExpose(
    Tk_Window top)		/* A mapped toplevel. */
{
    ThreadSpecificData *tsdPtr = (ThreadSpecificData *)
	    Tcl_GetThreadData(&dataKey, sizeof(ThreadSpecificData));
    PaintRecord *records = tsdPtr->records;
    int i, count = 0, recordCount = tsdPtr->count;
    (void) top;

    /*
     * The list is detached first: Expose bindings may capture again.
     */

    tsdPtr->records = NULL;
    tsdPtr->count = tsdPtr->size = 0;
    for (i = 0; i < recordCount; i++) {
	const PaintRecord *recPtr = &records[i];
	Tk_Window tkwin;
	XEvent event;

	/*
	 * An Expose binding may have destroyed windows of the list.
	 */

	if (!IsWindow(recPtr->hwnd)) {
	    continue;
	}
	tkwin = Tk_HWNDToWindow(recPtr->hwnd);
	if (tkwin == NULL || Tk_WindowId(tkwin) == None) {
	    continue;
	}
	memset(&event, 0, sizeof(event));
	event.xexpose.type = Expose;
	event.xexpose.serial = LastKnownRequestProcessed(Tk_Display(tkwin));
	event.xexpose.display = Tk_Display(tkwin);
	event.xexpose.window = Tk_WindowId(tkwin);
	event.xexpose.x = recPtr->rect.left;
	event.xexpose.y = recPtr->rect.top;
	event.xexpose.width = recPtr->rect.right - recPtr->rect.left;
	event.xexpose.height = recPtr->rect.bottom - recPtr->rect.top;
	Tk_HandleEvent(&event);
	count++;
    }
    if (records != NULL) {
	Tcl_Free(records);
    }
    return count;
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
    HWND client = Tk_GetHWND(Tk_WindowId(top));
    HWND frame = GetAncestor(client, GA_ROOT);
    MENUBARINFO info;
    POINT origin = {0, 0};

    /*
     * The menubar is a native menu in the non-client area of the frame
     * window that wraps the toplevel. An embedded toplevel has none.
     */

    if (frame == NULL || GetAncestor(client, GA_PARENT) != frame
	    || GetMenu(frame) == NULL) {
	return 0;
    }
    ZeroMemory(&info, sizeof(info));
    info.cbSize = sizeof(info);
    if (!GetMenuBarInfo(frame, OBJID_MENU, 0, &info)
	    || !ClientToScreen(client, &origin)
	    || origin.y <= info.rcBar.top) {
	return 0;
    }
    return (int) (origin.y - info.rcBar.top);
}

/*
 *----------------------------------------------------------------------
 *
 * MaskHidden --
 *
 *	Makes those pixels of a capture transparent that a window stacked
 *	above the client window, or above one of its ancestors inside the
 *	frame window, covers. Such a window is part of the rendered frame,
 *	and may be a widget of an application that embeds the toplevel.
 *	Windows stacked above the frame are not: the rendering leaves them
 *	out.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	See TkCaptureClearRect.
 *
 *----------------------------------------------------------------------
 */

static void
MaskHidden(
    HWND client,		/* Window of the captured toplevel. */
    HWND frame,			/* Top-level window that was rendered. */
    int x, int y,		/* Screen position of the buffer's upper
				 * left pixel. */
    TkCaptureBuffer *bufPtr)	/* The pixels. */
{
    HWND current, sibling;
    RECT rect;

    for (current = client; current != NULL && current != frame;
	    current = GetAncestor(current, GA_PARENT)) {
	/*
	 * GW_HWNDPREV steps to the next window up in the stacking order.
	 */

	for (sibling = GetWindow(current, GW_HWNDPREV); sibling != NULL;
		sibling = GetWindow(sibling, GW_HWNDPREV)) {
	    if (IsWindowVisible(sibling) && GetWindowRect(sibling, &rect)) {
		TkCaptureClearRect(bufPtr, rect.left - x, rect.top - y,
			rect.right - x, rect.bottom - y);
	    }
	}
    }
}

/*
 *----------------------------------------------------------------------
 *
 * RenderFrame --
 *
 *	Renders a top-level window, decorations and menubar included, into
 *	the bitmap selected into a memory DC.
 *
 *	PW_RENDERFULLCONTENT makes the desktop window manager deliver the
 *	composed window: correct when the window is covered or partly off
 *	screen, and with the child windows that all Tk widgets are.
 *	Without the flag PrintWindow sends WM_PRINT, which Tk's windows do
 *	not answer, so on older systems and on failure the pixels are
 *	copied from the screen instead; they are right only where the
 *	window is not covered.
 *
 * Results:
 *	TRUE on success, FALSE with the cause in GetLastError otherwise.
 *
 * Side effects:
 *	Discards the paint requests recorded by an earlier capture and
 *	records those that this one causes (see RecordPaint).
 *
 *----------------------------------------------------------------------
 */

static BOOL
RenderFrame(
    HWND frame,			/* Top-level window to render. */
    HDC memDC,			/* Receives the pixels. */
    int width, int height)	/* Size of the frame window. */
{
    BOOL ok = FALSE;

    if (CanRenderFullContent()) {
	ThreadSpecificData *tsdPtr = (ThreadSpecificData *)
		Tcl_GetThreadData(&dataKey, sizeof(ThreadSpecificData));
	HHOOK hook;

	/*
	 * Requests recorded by an earlier capture are dropped: the events
	 * that Tk queued for them will be delivered by the event loop.
	 *
	 * DwmFlush waits for the next composition pass: a window shown a
	 * moment ago may not have been composed yet, and is then rendered
	 * black.
	 */

	tsdPtr->count = 0;
	hook = SetWindowsHookExW(WH_CALLWNDPROC, RecordPaint, NULL,
		GetCurrentThreadId());
	DwmFlush();
	ok = PrintWindow(frame, memDC, PW_RENDERFULLCONTENT);
	if (hook != NULL) {
	    UnhookWindowsHookEx(hook);
	}
    }
    if (!ok) {
	HDC windowDC = GetWindowDC(frame);

	if (windowDC != NULL) {
	    ok = BitBlt(memDC, 0, 0, width, height, windowDC, 0, 0,
		    SRCCOPY | CAPTUREBLT);
	    ReleaseDC(frame, windowDC);
	}
    }
    GdiFlush();
    return ok;
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
    HWND client = Tk_GetHWND(Tk_WindowId(top));
    HWND frame = GetAncestor(client, GA_ROOT);
    RECT frameRect;
    POINT origin = {0, 0};	/* Client origin on the screen. */
    int frameWidth, frameHeight, row, col, code = TCL_ERROR;
    int dibX, dibY;		/* Position of the rectangle in the DIB. */
    HDC screenDC = NULL, memDC = NULL;
    HBITMAP dib = NULL;
    HGDIOBJ oldBitmap = NULL;
    BITMAPINFO info;
    void *bits = NULL;
    (void) flags;

    if (frame == NULL) {
	frame = client;
    }
    if (IsIconic(frame)) {
	return TkCaptureFailed(interp, "ICONIFIED",
		"the window is iconified");
    }
    if (!GetWindowRect(frame, &frameRect)
	    || !ClientToScreen(client, &origin)) {
	return Win32Failed(interp, "GetWindowRect");
    }
    frameWidth = frameRect.right - frameRect.left;
    frameHeight = frameRect.bottom - frameRect.top;
    if (frameWidth <= 0 || frameHeight <= 0) {
	return TkCaptureFailed(interp, "FAILED",
		"can't capture the window: it has no area");
    }

    /*
     * Render the frame window into a 32-bit top-down DIB and crop
     * afterwards: only top-level windows can be rendered, and the frame
     * is the smallest one that contains the toplevel.
     */

    screenDC = GetDC(NULL);
    memDC = (screenDC != NULL) ? CreateCompatibleDC(screenDC) : NULL;
    ZeroMemory(&info, sizeof(info));
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = frameWidth;
    info.bmiHeader.biHeight = -frameHeight;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    if (memDC != NULL) {
	dib = CreateDIBSection(screenDC, &info, DIB_RGB_COLORS, &bits, NULL,
		0);
    }
    if (dib == NULL || bits == NULL) {
	Win32Failed(interp, "CreateDIBSection");
	goto done;
    }
    oldBitmap = SelectObject(memDC, dib);
    if (!RenderFrame(frame, memDC, frameWidth, frameHeight)) {
	Win32Failed(interp, "PrintWindow");
	goto done;
    }

    if (TkCaptureAllocBuffer(interp, bufPtr, width, height, 1.0) != TCL_OK) {
	goto done;
    }

    /*
     * The rectangle starts above the client origin when it reaches into
     * the menubar.
     */

    dibX = origin.x - frameRect.left + x;
    dibY = origin.y - frameRect.top + y;
    for (row = 0; row < height; row++) {
	int srcY = dibY + row;
	const unsigned char *src;
	unsigned char *dst = bufPtr->pixels + (size_t) row * bufPtr->pitch;

	if (srcY < 0 || srcY >= frameHeight) {
	    bufPtr->complete = 0;
	    continue;
	}
	src = (const unsigned char *) bits + (size_t) srcY * frameWidth * 4;
	for (col = 0; col < width; col++, dst += 4) {
	    int srcX = dibX + col;

	    if (srcX < 0 || srcX >= frameWidth) {
		bufPtr->complete = 0;
		continue;
	    }

	    /*
	     * A DIB pixel is blue, green, red and an unused byte.
	     */

	    dst[0] = src[4 * srcX + 2];
	    dst[1] = src[4 * srcX + 1];
	    dst[2] = src[4 * srcX];
	    dst[3] = 255;
	}
    }
    MaskHidden(client, frame, frameRect.left + dibX, frameRect.top + dibY,
	    bufPtr);
    code = TCL_OK;

  done:
    if (oldBitmap != NULL) {
	SelectObject(memDC, oldBitmap);
    }
    if (dib != NULL) {
	DeleteObject(dib);
    }
    if (memDC != NULL) {
	DeleteDC(memDC);
    }
    if (screenDC != NULL) {
	ReleaseDC(NULL, screenDC);
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
