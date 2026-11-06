/*
 * display.h - the 3DS side of video: citro3d setup and frames.
 *
 * Kept apart from the GX code so that libctru's and the GameCube SDK's
 * names never meet in one file.
 */
#ifndef PLATFORM_VIDEO_DISPLAY_H
#define PLATFORM_VIDEO_DISPLAY_H

void display_init(void);
int display_frame_open(void);
void display_begin_frame(int clear, unsigned char r, unsigned char g, unsigned char b, unsigned char a,
                         unsigned long z);
void display_end_frame(void);
/* Waits for the top screen's vblank; returns 0 when the app should quit. */
int display_wait_vblank(void);
unsigned long display_frame_count(void);

#endif /* PLATFORM_VIDEO_DISPLAY_H */
