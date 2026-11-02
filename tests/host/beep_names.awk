# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
#
# beep.h's beeps as a table for tests/host/beepReel.c, which needs each one's name for its captions and WAV files and
# can't get it from C. For each BEEP_X in the first enum, but BEEP_NONE and BEEP_COUNT, one line:
#
#	{ "X", BEEP_X },
#
# beepReel checks the table against BEEP_COUNT when it starts, so a beep.h this misreads fails there.
#
#   awk -f beep_names.awk beep.h > beep_names.h

# comments out, so a beep named in one isn't taken for a beep
{
	sub(/\/\/.*/, "")
	gsub(/\/\*[^*]*\*\//, "")
}

# the enum's body: from the line with "enum" to the line with "}"
/enum/ {
	in_enum = 1
}

# every BEEP_ name on the line (there may be several)
in_enum {
	rest = $0
	while (match(rest, /BEEP_[A-Z0-9_]+/)) {
		name = substr(rest, RSTART, RLENGTH)
		rest = substr(rest, RSTART + RLENGTH)
		if (name != "BEEP_NONE" && name != "BEEP_COUNT")
			printf "\t{ \"%s\", %s },\n", substr(name, length("BEEP_") + 1), name
	}
}

in_enum && /}/ {
	in_enum = 0
}
