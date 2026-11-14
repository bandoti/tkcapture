# all.tcl --
#
#	Runs every *.test file in this directory.  Invoked by "make test",
#	which passes -load with a script that makes the freshly built
#	package available.
#
# Copyright © 2026 Bandoti Ltd.
#
# See the file "license.terms" for information on usage and redistribution of
# this file, and for a DISCLAIMER OF ALL WARRANTIES.

package require tcltest 2.5
namespace import ::tcltest::*

configure -testdir [file dirname [file normalize [info script]]] \
    -singleproc 1 {*}$argv

set failed [runAllTests]
exit $failed
