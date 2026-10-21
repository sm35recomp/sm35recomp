// Super Mario Bros. 35's controls: Mario moves by the d-pad only. The game reads the left stick (its input object
// gets the stick's position and direction bits) but does not walk with it in our setup, so the stick also presses the
// d-pad here (runtime/hle/input.c), and the touch stick and a controller's stick move Mario as the d-pad does.
#include <stdbool.h>

bool game_stick_as_dpad(void) { return true; }
