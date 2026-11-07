Renderer
========

The game draws through the GX API. The platform keeps GX state in GameCube
terms (platform/src/gx/gx_state.c) and draws in platform/src/gx/gx_draw.c:

  - Vertex data comes from immediate mode (GXBegin and the GXPosition*
    family, which write to the FIFO buffer; a primitive is drawn when its
    last byte is written, since GXEnd is empty) or from display lists
    (GXCallDisplayList; the game's files, read as they are in big-endian
    mode). Direct and indexed attributes, all component types, matrix
    indices per vertex, and indexed matrix loads in display lists.
  - The transform runs on the CPU: position and normal matrices,
    projection, viewport, the two lighting channels (ambient, material,
    diffuse and spot/distance attenuation), and texture coordinate
    generation with texture matrices. Primitives become triangle lists.
  - The 640x480 frame is mapped 4:3 at half size into the middle of the
    400x240 top screen. Geometry outside it is not cut off, so wide scenes
    show more to the sides.
  - Textures are decoded from every GX format (I4, I8, IA4, IA8, RGB565,
    RGB5A3, RGBA8, CMPR, and C4/C8/C14X2 through their TLUT) into the
    PICA's tiled RGBA8, once per image address, padded to a power of two.
  - TEV stages are compiled into PICA combiners. Constant inputs are
    folded on the CPU, so a GX stage that needs two constants becomes two
    PICA stages with one each (at most six PICA stages in all).
  - Alpha compare, blending, depth test and write masks are translated
    directly.

  - With clipping off (GX_CLIP_DISABLE) depth is kept just inside the
    range: 2D overlays sit exactly at the far end, which the PICA would
    clip.
  - Textures written again by the game (the font's glyph cache) are
    decoded again: DCFlushRange marks cached textures in the flushed range.

The host side (platform/host/gpu.c, vshader.v.pica) only rotates the clip
coordinates into the top screen's portrait framebuffer, and applies the
draw state with citro3d.

Speed. Draws with the same state that follow each other are sent as one
(about 45 instead of 520 per frame on the title screen); the host only sets
state that changed and flushes the vertex buffer once per frame; the
translated draw state is reused until a GX setter changes it. The platform
is built with -O2; the game stays at -O1 (see compile_check.py). On the
title screen in Azahar that gives 15 frames a second, from 10: per frame
about 19 ms of vertex work and 30 ms of game code, and the rest waiting for
the next retrace. platform/src/os/prof.c logs that breakdown every 120
frames.

Not done yet: lines and points, EFB copies to textures (GXCopyTex),
indirect texturing, swap tables, fog, TEV colour registers written by one
stage and read by a later one, culling.


Testing without a screen
------------------------

`run_azahar.py --script "650 press START 8;1000 shot"` writes a script
for the port (platform/src/os/script.c): buttons held at given frames, and
screenshots the port saves itself from its render target, as
build/shots/colosseum-shot-NNNNN.png. Azahar runs minimised and does not
take the desktop's focus.
