#ifndef AL_PHYSBOT_H
#define AL_PHYSBOT_H
#include "stage.h"
/* 1 if the exit can be reached with real inputs; states explored; the input path
 * (one digit per 4 frames, see physbot.c) */
int physbot_solve(const Stage *S, unsigned *states, char *path, int pathmax);
#endif
