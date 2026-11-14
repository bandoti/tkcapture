# tkcapture.tcl --
#
#	Script part of the tkcapture package: capture of Tk windows,
#	widgets and regions into photo images. The pixels are read by
#	::tkcapture::_capture, which is implemented in C; this file is sourced
#	by the package index after the shared library has been loaded.
#
#	The package can be used in a safe interpreter with Safe Tk. There
#	it lacks [save], because a safe interpreter may not write files.
#
# Copyright © 2026 Bandoti Ltd.
#
# See the file "license.terms" for information on usage and redistribution of
# this file, and for a DISCLAIMER OF ALL WARRANTIES.

namespace eval ::tkcapture {
    namespace export capture png layout locate

    variable version
    if {![info exists version]
	    || [namespace which -command _capture] eq ""} {
	return -code error "the tkcapture shared library is not loaded"
    }

    # Options of [capture] and their defaults.
    variable captureOptions {
	-photo {} -region {} -scale logical -update 1 -menubar 0
	-nocomplain 0
    }
}

# ::tkcapture::ParseOptions --
#
#	Parses a list of option-value pairs.
#
# Arguments:
#	defaults	Dictionary of the valid options and their defaults.
#	arguments	Option-value pairs. Options can be abbreviated.
#
# Results:
#	The dictionary 'defaults' with the given values filled in.

proc ::tkcapture::ParseOptions {defaults arguments} {
    if {[llength $arguments] % 2} {
	return -code error -level 2 -errorcode {TK VALUE_MISSING} \
	    "value for \"[lindex $arguments end]\" missing"
    }
    foreach {option value} $arguments {
	try {
	    set option [::tcl::prefix match -message option \
		    [dict keys $defaults] $option]
	} on error {message options} {
	    return -code error -level 2 \
		-errorcode [dict get $options -errorcode] $message
	}
	dict set defaults $option $value
    }
    return $defaults
}

# ::tkcapture::CheckWindow --
#
#	Raises the standard Tk error if a window does not exist.
#
# Arguments:
#	window		Path name to check.
#
# Results:
#	None.

proc ::tkcapture::CheckWindow {window} {
    if {![winfo exists $window]} {
	return -code error -level 2 \
	    -errorcode [list TK LOOKUP WINDOW $window] \
	    "bad window path name \"$window\""
    }
    return
}

# ::tkcapture::capture --
#
#	Captures what the toplevel of a window displays in a rectangle of
#	that window and stores it in a photo image.
#
# Arguments:
#	window		Window that defines the coordinate system and the
#			default rectangle.
#	args		Options:
#	  -photo name	  Existing photo image to overwrite. By default a
#			  new image is created, which the caller owns.
#	  -region list	  Rectangle {x1 y1 x2 y2} in window's coordinates,
#			  lower right corner exclusive. It may extend
#			  beyond the window and is clipped to the toplevel.
#			  Defaults to the whole window.
#	  -scale mode	  logical (one image pixel per Tk pixel, default)
#			  or device (one per backing-store pixel).
#	  -update bool	  Run [update idletasks] first, and redraw what
#			  the capture made the system ask for (default
#			  true).
#	  -menubar bool	  Include the menubar of a toplevel.
#	  -nocomplain bool  Return a fully transparent image instead of
#			  raising an error when no pixel could be read.
#
# Results:
#	The name of the photo image. Its -metadata receives the keys
#	"capture" (window, toplevel, x, y, rootx, rooty, width, height,
#	scale, complete, windowingsystem, time) and "DPI".

proc ::tkcapture::capture {window args} {
    variable captureOptions

    CheckWindow $window
    set opts [ParseOptions $captureOptions $args]
    try {
	set scale [::tcl::prefix match -message scale {logical device} \
		[dict get $opts -scale]]
    } on error {message options} {
	return -code error -errorcode [dict get $options -errorcode] $message
    }
    foreach option {-update -menubar -nocomplain} {
	if {![string is boolean -strict [dict get $opts $option]]} {
	    return -code error -errorcode {TCL VALUE NUMBER} \
		"expected boolean value but got \"[dict get $opts $option]\""
	}
    }
    set update [dict get $opts -update]

    if {$update} {
	update idletasks
	# An idle callback may have destroyed the window.
	CheckWindow $window
    }

    # An iconified toplevel has no surface to read on some platforms and
    # is unmapped on the others; report it the same way everywhere. A
    # safe interpreter has no [wm]; its toplevel is embedded and is not
    # iconified by itself.
    set top [winfo toplevel $window]
    if {![catch {wm state $top} state] && $state eq "iconic"} {
	return -code error -errorcode {TK CAPTURE ICONIFIED} \
	    "window \"$top\" is iconified"
    }

    set photo [dict get $opts -photo]
    set created [expr {$photo eq ""}]
    if {$created} {
	set photo [image create photo]
    }
    set captureCmd [list _capture $window $photo [dict get $opts -region] \
	    $scale [dict get $opts -menubar] [dict get $opts -nocomplain]]
    try {
	set info [{*}$captureCmd]
	# Reading the pixels can make the system ask for parts that were
	# never drawn (on Windows, those that were off screen when the
	# window was shown). They are drawn and read again.
	for {set i 0} {$i < 3 && $update && [_expose $window]} {incr i} {
	    update idletasks
	    if {![winfo exists $window]} {
		throw [list TK LOOKUP WINDOW $window] \
		    "bad window path name \"$window\""
	    }
	    set info [{*}$captureCmd]
	}
	set capture [dict merge \
		[dict create window $window toplevel $top] $info \
		[dict create windowingsystem [tk windowingsystem] \
		    time [clock milliseconds]]]
	set dpi [expr {72.0 * [tk scaling -displayof $window]
		* [dict get $info scale]}]
	set metadata [$photo cget -metadata]
	dict set metadata capture $capture
	dict set metadata DPI $dpi
	$photo configure -metadata $metadata
    } on error {message options} {
	if {$created} {
	    image delete $photo
	}
	return -code error -errorcode [dict get $options -errorcode] $message
    }
    return $photo
}

# ::tkcapture::save --
#
#	Captures a window and writes the image to a file. Not available
#	in a safe interpreter.
#
# Arguments:
#	window		Window to capture.
#	fileName	File to write.
#	args		Options of [capture], and:
#	  -format fmt	  Image file format. By default it is derived from
#			  the extension of fileName; png if there is none.
#
# Results:
#	fileName.

if {![interp issafe]} {
namespace eval ::tkcapture {
    namespace export save
}

proc ::tkcapture::save {window fileName args} {
    variable captureOptions

    CheckWindow $window
    set opts [ParseOptions [dict replace $captureOptions -format {}] $args]
    set format [dict get $opts -format]
    if {$format eq ""} {
	set format [string tolower [string range \
		[file extension $fileName] 1 end]]
	switch -- $format {
	    ""  {set format png}
	    jpg {set format jpeg}
	    tif {set format tiff}
	}
    }
    set captureArgs [dict remove $opts -format]

    try {
	set photo [capture $window {*}$captureArgs]
    } on error {message options} {
	return -code error -errorcode [dict get $options -errorcode] $message
    }
    try {
	$photo write $fileName -format $format
    } finally {
	if {[dict get $opts -photo] eq ""} {
	    image delete $photo
	}
    }
    return $fileName
}
}

# ::tkcapture::png --
#
#	Captures a window and returns the image as PNG data.
#
# Arguments:
#	window		Window to capture.
#	args		Options of [capture].
#
# Results:
#	The bytes of a PNG file.

proc ::tkcapture::png {window args} {
    variable captureOptions

    CheckWindow $window
    set opts [ParseOptions $captureOptions $args]
    try {
	set photo [capture $window {*}$opts]
    } on error {message options} {
	return -code error -errorcode [dict get $options -errorcode] $message
    }
    try {
	return [$photo data -format png]
    } finally {
	if {[dict get $opts -photo] eq ""} {
	    image delete $photo
	}
    }
}

# ::tkcapture::layout --
#
#	Describes the geometry of a window and of its viewable descendants
#	in the same toplevel.
#
# Arguments:
#	window		Root of the window tree to describe.
#
# Results:
#	A dictionary that maps each window path to a dictionary with the
#	keys class, x, y, width and height. Coordinates are relative to
#	'window', so they are pixel positions in a logical-scale capture
#	of it. Parents come before their children and siblings are in
#	stacking order, lowest first: where rectangles overlap, the later
#	entry is displayed on top.

proc ::tkcapture::layout {window} {
    CheckWindow $window
    set top [winfo toplevel $window]
    set originX [winfo rootx $window]
    set originY [winfo rooty $window]
    set result [dict create]
    set pending [list $window]
    while {[llength $pending]} {
	set pending [lassign $pending w]
	if {![winfo viewable $w]
		|| ($w ne $window && [winfo toplevel $w] ne $top)} {
	    continue
	}
	dict set result $w [dict create \
		class [winfo class $w] \
		x [expr {[winfo rootx $w] - $originX}] \
		y [expr {[winfo rooty $w] - $originY}] \
		width [winfo width $w] \
		height [winfo height $w]]
	# Depth first: the children go to the front of the queue.
	set pending [list {*}[winfo children $w] {*}$pending]
    }
    return $result
}

# ::tkcapture::locate --
#
#	Maps a pixel of a captured image back to the widget shown there,
#	using the image's capture metadata and the current geometry.
#
# Arguments:
#	image		Photo image filled by [capture].
#	x, y		Pixel position in the image.
#
# Results:
#	A dictionary with the keys window (the topmost window of the
#	captured toplevel that contains the point) and x, y (the point in
#	that window's coordinates), or an empty dictionary if the point is
#	outside the toplevel.

proc ::tkcapture::locate {image x y} {
    if {[catch {dict get [$image cget -metadata] capture} capture]
	    || [catch {dict get $capture window} window]} {
	return -code error -errorcode {TK CAPTURE NO_METADATA} \
	    "image \"$image\" has no capture metadata"
    }
    if {![string is integer -strict $x] || ![string is integer -strict $y]} {
	return -code error -errorcode {TCL VALUE NUMBER} \
	    "expected integer pixel coordinates but got \"$x $y\""
    }
    CheckWindow $window
    set scale [dict get $capture scale]
    set top [winfo toplevel $window]

    # The point in the coordinates of the toplevel.
    set px [expr {[dict get $capture x] + int(floor($x / $scale))
	    + [winfo rootx $window] - [winfo rootx $top]}]
    set py [expr {[dict get $capture y] + int(floor($y / $scale))
	    + [winfo rooty $window] - [winfo rooty $top]}]

    set result [dict create]
    dict for {w geometry} [layout $top] {
	set wx [expr {$px - [dict get $geometry x]}]
	set wy [expr {$py - [dict get $geometry y]}]
	if {$wx >= 0 && $wx < [dict get $geometry width]
		&& $wy >= 0 && $wy < [dict get $geometry height]} {
	    set result [dict create window $w x $wx y $wy]
	}
    }
    return $result
}

package provide tkcapture $::tkcapture::version
