# tkcapture

A Tk extension that captures any Tk window, widget or region of the
calling application into a photo image: for visual regression tests,
screenshots for documentation, and agents that need to see the
interface they build.

```tcl
package require tkcapture

tkcapture::save . main.png              ;# the whole window
tkcapture::save .toolbar toolbar.png    ;# one widget
set img [tkcapture::capture .f.ok]      ;# a photo image to work with
```

It runs on X11, Windows and macOS with Tcl and Tk 9.0 or later. It
only looks at the application's own windows: no desktop, no other
applications, no window-manager decorations, no extra permissions.

## What a capture contains

Every capture is a crop of what the **toplevel** shows. A capture of a
widget therefore contains the widget, its children and anything else of
the same toplevel that is drawn over it, and its pixels equal the
corresponding pixels of a capture of the whole window. With the default
`-scale logical`, one image pixel is one Tk pixel, so image coordinates
are window coordinates.

## Commands

| Command | Result |
|---|---|
| `tkcapture::capture window ?-photo img? ?-region {x1 y1 x2 y2}? ?-scale logical\|device? ?-update bool? ?-menubar bool? ?-nocomplain bool?` | Name of the photo image |
| `tkcapture::save window fileName ?-format fmt? ?capture options?` | `fileName`; the format follows the extension, PNG by default |
| `tkcapture::png window ?capture options?` | The bytes of a PNG file |
| `tkcapture::layout window` | A dictionary with one entry for `window` and one for each widget inside it, such as `.f.ok {class Button x 12 y 80 width 60 height 28}`; `x` and `y` are relative to `window` |
| `tkcapture::locate image x y` | The widget shown at pixel `x`,`y` of a captured image, such as `window .f.ok x 12 y 9`; in the result, `x` and `y` are that point in the widget's own coordinates |

## Using it from an agent or a test script

Wait until a new toplevel is displayed before capturing it:

```tcl
package require Tk
package require tkcapture

# ... build the user interface ...
update
if {![winfo viewable .]} {tkwait visibility .}
```

**The whole window**, to judge the overall layout:

```tcl
tkcapture::save . /tmp/main.png
```

**One widget**, to look at a part in isolation:

```tcl
tkcapture::save .toolbar /tmp/toolbar.png
```

**A widget with some context around it.** The region is given in the
widget's own coordinates and may reach beyond it:

```tcl
set w [winfo width .f.ok]
set h [winfo height .f.ok]
tkcapture::save .f.ok /tmp/ok.png \
    -region [list -8 -8 [expr {$w + 8}] [expr {$h + 8}]]
```

**PNG data without a file**, for example to hand it to a model as
base64:

```tcl
set base64 [binary encode base64 [tkcapture::png .]]
```

**Where the widgets are in the picture.** `layout` returns the rectangle
of every viewable widget in the pixel coordinates of a capture of the
same window:

```tcl
dict for {path g} [tkcapture::layout .] {
    dict with g {
        puts "$path ($class): $x,$y ${width}x$height"
    }
}
```

**From a pixel back to the widget.** Something looks wrong at pixel
(212, 40) of a screenshot; find the widget and click it:

```tcl
set img [tkcapture::capture .]
set hit [tkcapture::locate $img 212 40]
# -> window .f.ok x 12 y 9
event generate [dict get $hit window] <Button-1> \
    -x [dict get $hit x] -y [dict get $hit y]
```

Every capture records in the image's `-metadata` which window and
rectangle it shows; `locate` uses that. Pixels that could not be read
(for example off the screen) are transparent.

**Many captures without creating many images:**

```tcl
image create photo snap
foreach page [.nb tabs] {
    .nb select $page
    tkcapture::capture $page -photo snap
    snap write /tmp/[winfo name $page].png -format png
}
```

**Visual regression test:**

```tcl
set got  [tkcapture::capture .chart]
set want [image create photo -file golden/chart.png]
if {[$got data] ne [$want data]} { error "chart changed" }
```

## Sandboxed agents: safe interpreters

The package works in a safe interpreter with Safe Tk, so a sandboxed
script can look at the interface it builds:

```tcl
set sandbox [safe::interpCreate]
safe::loadTk $sandbox
$sandbox eval {
    package require tkcapture
    button .b -text Hello; pack .b; update
}
set png [$sandbox eval {tkcapture::png .}]     ;# fetched by the trusted side
```

In a safe interpreter `capture`, `png`, `layout` and `locate` exist;
`save` does not (no file access). A safe interpreter sees its own
windows only, and only what lies inside the container that its main
window is embedded in; pixels under anything stacked over it are
transparent on X11 and Windows. On macOS a host's widgets over the
container are captured.

## Building

The build uses TEA. Build out of tree against an installed Tcl and Tk:

```sh
mkdir -p build/9.0.4 && cd build/9.0.4
../../configure --with-tcl=/path/to/prefix/lib --with-tk=/path/to/prefix/lib
make
make test            # needs a display; headless: xvfb-run -a make test
make install
```

`make test-visuals` (X11 only) runs the tests under Xvfb once for each
kind of visual: 24-, 30-, 16- and 15-bit TrueColor, 8-bit PseudoColor,
DirectColor and StaticGray.

On X11, `configure` uses the X Composite extension if its development
files are installed (`libxcomposite-dev` on Debian and Ubuntu); under a
compositing window manager the capture then also reads parts of a
window that are off the screen. `--disable-xcomposite` turns this off.

`configure` is generated from `configure.ac` with `autoconf`.

### Windows

With Visual C++, from a developer command prompt:

```bat
cd win
nmake /f makefile.vc INSTALLDIR=C:\path\to\tcl
nmake /f makefile.vc INSTALLDIR=C:\path\to\tcl test
nmake /f makefile.vc INSTALLDIR=C:\path\to\tcl install
```

To keep builds against several Tcl versions apart, add
`OUT_DIR=…\build\<name> TMP_DIR=…\build\<name>\obj`.

With MSYS2/MinGW (UCRT64 shell), as on Unix:

```sh
mkdir -p build/win && cd build/win
../../configure --with-tcl=/path/to/prefix/lib --with-tk=/path/to/prefix/lib
make && make test
```

MSYS2 sets the default `--prefix` to its own tree (`/ucrt64`); pass
`--prefix` to install elsewhere. The headers always come from the Tcl
and Tk that `--with-tcl` and `--with-tk` name, even when MSYS2's own Tcl
package is installed.

### macOS

```sh
mkdir -p build/mac && cd build/mac
../../configure --with-tcl=/path/to/prefix/lib --with-tk=/path/to/prefix/lib
make && make test
```

For a framework build of Tcl and Tk, point `--with-tcl` and `--with-tk`
at the directories that hold `tclConfig.sh` and `tkConfig.sh`.

Tk on macOS sends standard output to `/dev/null` when standard input is
`/dev/null`, as under CI; `make test` feeds it an empty pipe so that the
results are printed. Do the same when running a script that uses Tk
from another program: `: | tclsh script.tcl`.

## Layout

```
generic/tkCapture.c          the _grab command: region arithmetic, photo output
generic/tkCapture.h          capture buffer and the platform hooks
generic/tkCaptureConvert.c   XImage to RGBA for every kind of X visual
unix/tkCaptureX11.c          X11 backend
win/tkCaptureWin.c           Windows backend
macosx/tkCaptureMac.m        macOS backend
library/tkcapture.tcl        capture, save, png, layout, locate
tests/                       tcltest suite (capture, helpers, safe, package)
doc/tkcapture.n              manual page
```

## See also

The manual page, `man -l doc/tkcapture.n`, for the details of each
command; its PLATFORM NOTES cover how each windowing system differs.

## License

Copyright © 2026 Bandoti Ltd. Distributed under the same terms as Tcl
and Tk; see [license.terms](license.terms).
