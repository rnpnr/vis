#include "termkey/termkey.h"

#include "termkey/driver-csi.c"
#include "termkey/driver-ti.c"
#include "termkey/termkey.c"

/* wtf curses */
#undef color_names
#undef columns
#undef cursor_visible
#undef lines
#undef tab
