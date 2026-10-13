#pragma once
// AKVR PANELMOUSE: the game's DirectInput mouse reads return nothing while `blocked()` says the VR panel is open.
void akvr_mouse_block_install(bool (*blocked)()); // once, after the game's window exists
unsigned akvr_mouse_block_reads();                // mouse reads / event batches blanked so far
