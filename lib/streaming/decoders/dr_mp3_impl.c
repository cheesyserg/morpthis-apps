// The one translation unit that compiles dr_mp3. The app feeds it from the
// network through a read callback, so the stdio helpers are not needed and are
// left out of the image.

#define DR_MP3_IMPLEMENTATION
#define DR_MP3_NO_STDIO
#include "dr_mp3.h"
