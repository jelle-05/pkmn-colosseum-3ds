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

The host side (platform/host/gpu.c, vshader.v.pica) only rotates the clip
coordinates into the top screen's portrait framebuffer, and applies the
draw state with citro3d.

Not done yet: lines and points, EFB copies to textures (GXCopyTex),
indirect texturing, swap tables, fog, TEV colour registers written by one
stage and read by a later one, culling, texture updates after the first
use, and speed (about 10 frames a second in Azahar on the title screen).

tools/azahar_shot.ps1 runs the build and saves screenshots of the Azahar
window, for checking the picture without watching.
