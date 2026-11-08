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
  - Each GX vertex is decoded once and stored once; quads, strips and fans
    become 16-bit indices into the frame's vertex buffer.
  - Transform and lighting, two ways:
      - the transform shader (platform/host/tnl.v.pica), for every draw it
        can express: the CPU only decodes vertices (GpuTnlVertex: model
        position with its matrix index, normal, colour, and per texture
        unit its coordinate source), and the GX matrices, lights, channel
        registers and texture coordinate rows go into a uniform block
        (GpuTnlUniforms, filled by tnl_uniforms() in gx_draw.c);
      - the CPU path, for the rest (more than three lights, colour and
        alpha channels with different lights, per-vertex texture
        matrices, SRTG texture coordinates): emit() in gx_draw.c
        transforms and lights on the CPU, and vshader.v.pica only rotates
        the result into the framebuffer.
    The test script's "N tnl 0" forces the CPU path, "N tnl 1" allows the
    shader again, to compare the two.
  - The 640x480 frame is mapped 4:3 at half size into the middle of the
    400x240 top screen. Geometry outside it is not cut off, so wide scenes
    show more to the sides.
  - Textures are decoded from every GX format (I4, I8, IA4, IA8, RGB565,
    RGB5A3, RGBA8, CMPR, and C4/C8/C14X2 through their TLUT), padded to a
    power of two, into the smallest PICA format that holds them exactly:
    LA4 (I4, IA4), LA8 (I8, IA8, IA8 palettes), RGB565 (RGB565, RGB565
    palettes, opaque CMPR), RGBA5551 (CMPR with transparency), RGBA8 (the
    rest). Once per image address; textures the game writes again (the
    font's glyph cache) are decoded again, as DCFlushRange marks the
    cached textures in the flushed range. When texture memory runs out,
    the textures not used for two frames are freed.
  - TEV stages are compiled into PICA combiners. Constant inputs are
    folded on the CPU, so a GX stage that needs two constants becomes two
    PICA stages with one each (at most six PICA stages in all).
  - Alpha compare, blending, depth test and write masks are translated
    directly.
  - With clipping off (GX_CLIP_DISABLE) depth is kept just inside the
    range: 2D overlays sit exactly at the far end, which the PICA would
    clip.

The host side (platform/host/gpu.c) binds one of the two programs per
draw (switching uploads all of the new program's uniforms, as they share
the PICA's registers), uploads the transform shader's changed uniforms,
and applies the draw state with citro3d.

Speed. Draws with the same state (and, on the shader path, the same
uniforms) that follow each other are sent as one; the host only sets
state that changed and flushes the vertex buffers once per frame; the
translated draw state is reused until a GX setter changes it. On the
shader path the matrix part of the uniform block is rebuilt only when the
GX matrices changed. platform/src/os/prof.c logs every 120 frames where
the frame's time goes: vertex decoding and transform (unlit, lit),
texture decoding, GPU submission, the retrace wait, interrupt handlers
and where the main thread blocks.

Before the shader, the CPU path took 28-31 ms a frame on the New 3DS's
title screen (two spot lights on nearly every vertex). In Azahar the
shader path halves the CPU's vertex work there (17 to 9 ms); Azahar's
software vertex shader is slow, so the field runs faster in Azahar on the
CPU path. The console's numbers with the shader are still to be measured
(docs/HARDWARE.md).

Not done yet: lines and points, EFB copies to textures (GXCopyTex),
indirect texturing, swap tables, fog, TEV colour registers written by one
stage and read by a later one, culling, COLOR1/ALPHA1 lighting. Outskirt
Stand's ground draws black on both paths.


Testing without a screen
------------------------

`run_azahar.py --script "650 press START 8;1000 shot"` writes a script
for the port (platform/src/os/script.c): buttons held at given frames,
screenshots the port saves itself from its render target (as
build/shots/colosseum-shot-NNNNN.png), and "tnl 0/1". Azahar runs
minimised and does not take the desktop's focus.
