/* AudioOutput.h - plays the game's mixed audio (rt_audio_read) through Core Audio. */
#pragma once
#include <stdbool.h>
bool rt_audio_output_start(void);
void rt_audio_output_stop(void);
