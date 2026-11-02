// Drawing with the psp's GPU (the GE): tile layers, sprites, rectangles - included by Drawing.cpp
//
// The CPU drawing pixel by pixel took ~11.5ms a frame for the tile layers and ~20ms for the sprites of a menu on the
// hardware. Here the GE draws them:
//  - the engine's frame (Engine.frameBuffer) lives in VRAM, where the GE draws and the CPU still draws whatever the GE
//    doesn't do here, in the engine's order;
//  - the GE's commands go into one batch (a display list) for as long as only the GE draws: the batch is closed and
//    the GE waited for (PSPGEFlush) when the CPU is about to draw into the frame (every other drawing function starts
//    with that, see PSP_SPROF) and at the end of the frame. Opening a batch writes the CPU's data cache back and
//    empties it, so each side sees what the other drew;
//  - the tileset is an 8-bit texture in VRAM (512x512: the 1024 16x16 tiles, 32 a row), kept in step with
//    tilesetGFXData (stage loads, Copy16x16Tile); sprite sheets are 8-bit textures read from RAM (graphicData), a
//    window of up to 512x512 starting at the sprite;
//  - each palette is a CLUT made from the engine's 16-bit palette (bit-replicated to 8888, so the GE gives back the
//    same 16-bit colour), entry 0 transparent (alpha test), like the CPU's "index 0 isn't drawn". A CLUT is copied
//    into the display list each time its palette has changed, so a palette changed during the frame can't alter
//    commands already queued;
//  - a tile layer is drawn in runs of lines that share the horizontal position, the palette and the tile row (the
//    parallax and the water ripples make one-line runs), one sprite per tile, flips by swapped texture coordinates.
//    Every line's position is worked out by the same code as DrawHLineScrollLayer, scroll side effects included;
//  - a sprite is split in bands of lines with the same palette (gfxLineBuffer), clipped exactly as the CPU clips it.
// GELayers=false in settings.ini ([Dev]) goes back to the CPU for everything (the frame in RAM; PPSSPP needs that).

#define PSP_VRAM_RT      (0x88000)  // the engine's frame: 448x240, after the two 512x272 display buffers
#define PSP_VRAM_TILESET (0xC0000)  // 512x512 8-bit
#define PSP_RT_STRIDE    (448)      // the GE draws into buffers a multiple of 64 pixels wide

bool pspGELayers = true; // (settings.ini [Dev] GELayers)

#define PSP_GE_LIST_BYTES  (512 * 1024)
#define PSP_GE_LIST_MARGIN (192 * 1024) // a new batch before an op that might not fit (a layer of one-line runs: ~150KB)
static unsigned int __attribute__((aligned(16))) pspGEList[PSP_GE_LIST_BYTES / 4];

bool pspGEOpen = false;
// (counters for the [sprof] line: sprites the GE drew, those it left to the CPU and why, batches closed)
int pspGEStatSprites, pspGEStatTooBig, pspGEStatUnaligned, pspGEStatFlushes;
enum PSPGEModes { PSPGE_NONE, PSPGE_TEXTURED, PSPGE_FILL };
static int pspGEMode           = PSPGE_NONE;
static const void *pspGETexture = NULL; // the texture the GE has now (this batch)
static int pspGEClutLoaded      = -1;   // the palette loaded as the CLUT now (this batch)
static uint *pspGEClut[PALETTE_COUNT];  // each palette's CLUT in this batch (NULL: none yet)
// the CLUTs' memory: a ring of slots, used from the start again by each batch (one draw needs at most one per palette)
#define PSP_GE_CLUT_RING (128)
static uint __attribute__((aligned(64))) pspGEClutRing[PSP_GE_CLUT_RING][PALETTE_COLOR_COUNT];
static int pspGEClutRingUsed = 0;
static ushort pspGEClutSource[PALETTE_COUNT][PALETTE_COLOR_COUNT]; // the palette that CLUT was made from

struct PSPTileVertex {
    unsigned short u, v;
    short x, y, z;
};

// Sprites with the same texture, CLUT and mode go out as one draw: their vertices pile up in a block of the list and
// the draw is sent when anything changes (PSPGEFlushSprites, called first by everything that changes the GE's state)
#define PSP_GE_SPRITE_BATCH (128)
static PSPTileVertex *pspGESpriteVerts = NULL;
static int pspGESpriteCount            = 0;

// rotated sprites: two triangles each, float coordinates (batched the same way)
struct PSPRotVertex {
    float u, v;
    float x, y, z;
};
#define PSP_GE_ROT_BATCH (64)
static PSPRotVertex *pspGERotVerts = NULL;
static int pspGERotCount           = 0;

struct PSPFillVertex {
    uint color;
    short x, y, z;
};

// flat faces (Draw3DScene's FadedFace): coloured triangles, batched the same way
#define PSP_GE_FACE_VERTS (128 * 12)
static PSPFillVertex *pspGEFaceVerts = NULL;
static int pspGEFaceVertCount        = 0;

static void PSPGEFlushSprites()
{
    if (pspGESpriteCount)
        sceGuDrawArray(GU_SPRITES, GU_TEXTURE_16BIT | GU_VERTEX_16BIT | GU_TRANSFORM_2D, pspGESpriteCount * 2, 0, pspGESpriteVerts);
    pspGESpriteVerts = NULL;
    pspGESpriteCount = 0;
    if (pspGERotCount)
        sceGuDrawArray(GU_TRIANGLES, GU_TEXTURE_32BITF | GU_VERTEX_32BITF | GU_TRANSFORM_2D, pspGERotCount * 6, 0, pspGERotVerts);
    pspGERotVerts = NULL;
    pspGERotCount = 0;
    if (pspGEFaceVertCount)
        sceGuDrawArray(GU_TRIANGLES, GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, pspGEFaceVertCount, 0, pspGEFaceVerts);
    pspGEFaceVerts     = NULL;
    pspGEFaceVertCount = 0;
}

static inline PSPRotVertex *PSPGEAddRotSprite()
{
    if (pspGERotCount == PSP_GE_ROT_BATCH || pspGESpriteCount || pspGEFaceVertCount) // (one kind waiting at a time keeps the order)
        PSPGEFlushSprites();
    if (!pspGERotVerts)
        pspGERotVerts = (PSPRotVertex *)sceGuGetMemory(PSP_GE_ROT_BATCH * 6 * sizeof(PSPRotVertex));
    return &pspGERotVerts[6 * pspGERotCount++];
}

static inline PSPTileVertex *PSPGEAddSprite()
{
    if (pspGESpriteCount == PSP_GE_SPRITE_BATCH || pspGERotCount || pspGEFaceVertCount)
        PSPGEFlushSprites();
    if (!pspGESpriteVerts)
        pspGESpriteVerts = (PSPTileVertex *)sceGuGetMemory(PSP_GE_SPRITE_BATCH * 2 * sizeof(PSPTileVertex));
    return &pspGESpriteVerts[2 * pspGESpriteCount++];
}

static inline PSPFillVertex *PSPGEAddFaceVerts(int count)
{
    if (pspGEFaceVertCount + count > PSP_GE_FACE_VERTS || pspGESpriteCount || pspGERotCount)
        PSPGEFlushSprites();
    if (!pspGEFaceVerts)
        pspGEFaceVerts = (PSPFillVertex *)sceGuGetMemory(PSP_GE_FACE_VERTS * sizeof(PSPFillVertex));
    PSPFillVertex *v = &pspGEFaceVerts[pspGEFaceVertCount];
    pspGEFaceVertCount += count;
    return v;
}

static inline byte *PSPVram(uint offset) { return (byte *)sceGeEdramGetAddr() + offset; }

// the engine's frame in VRAM (cached address: the CPU draws through its data cache)
static ushort *PSPLayerFrameBuffer() { return (ushort *)PSPVram(PSP_VRAM_RT); }

// a 16-bit colour (red in the low bits: see RGB888_TO_RGB565) as the GE's 8888, bit-replicated: it comes back the same
static inline uint PSPColor8888(uint c, uint alpha)
{
    uint r5 = c & 0x1F, g6 = (c >> 5) & 0x3F, b5 = (c >> 11) & 0x1F;
    uint r8 = (r5 << 3) | (r5 >> 2), g8 = (g6 << 2) | (g6 >> 4), b8 = (b5 << 3) | (b5 >> 2);
    return (alpha << 24) | (b8 << 16) | (g8 << 8) | r8;
}

// tiles [first, first + count) of tilesetGFXData into the VRAM texture (uncached writes: nothing to flush)
void PSPUploadTiles(int first, int count)
{
    if (!pspGELayers)
        return;
    PSPGEFlush(); // (the GE may still be drawing with the old ones)
    byte *tex = (byte *)((uintptr_t)PSPVram(PSP_VRAM_TILESET) | 0x40000000);
    for (int t = first; t < first + count && t < TILE_COUNT; ++t) {
        byte *dst       = &tex[((t >> 5) * TILE_SIZE) * 512 + (t & 31) * TILE_SIZE];
        const byte *src = &tilesetGFXData[t * TILE_DATASIZE];
        for (int r = 0; r < TILE_SIZE; ++r) memcpy(&dst[r * 512], &src[r * TILE_SIZE], TILE_SIZE);
    }
}

// closes the batch and waits for the GE: the frame is the CPU's again
bool pspBlitPending = false; // (FlipScreenPSP: the GE may still be putting the last frame on screen from the frame buffer)
void PSPGEFlush()
{
    if (!pspGEOpen) {
        if (pspBlitPending) { // (the last list started is that one: wait for it)
            sceGuSync(0, 0);
            pspBlitPending = false;
        }
        return;
    }
    pspBlitPending = false; // (this list comes after it: waiting for this one waits for both)
    ++pspGEStatFlushes;
    PSPGEFlushSprites();
    sceGuFinish();
    int result = sceGuSync(0, 0);
    pspGEOpen  = false;
    static int lastResult = 0;
    if (result != lastResult) { // (diagnostics: the GE refusing a list shows here)
        PSP_BOOTLOG("GE: sync returned %08X (list used %d bytes)", result, sceGuCheckList());
        lastResult = result;
    }
}

// the GE draws into the engine's frame from here on (a new batch, unless one is open with room in it)
static void PSPGEBegin()
{
    if (pspGEOpen
        && (sceGuCheckList() > PSP_GE_LIST_BYTES - PSP_GE_LIST_MARGIN || pspGEClutRingUsed > PSP_GE_CLUT_RING - PALETTE_COUNT))
        PSPGEFlush();
    if (pspGEOpen)
        return;
    sceKernelDcacheWritebackInvalidateAll(); // the CPU's drawing reaches VRAM, and it'll read back what the GE draws
    sceGuStart(GU_DIRECT, pspGEList);
    sceGuDrawBufferList(GU_PSM_5650, (void *)PSP_VRAM_RT, PSP_RT_STRIDE);
    sceGuScissor(0, 0, SCREEN_XSIZE, SCREEN_YSIZE);
    sceGuDisable(GU_DITHER);
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDisable(GU_BLEND);
    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
    sceGuTexFilter(GU_NEAREST, GU_NEAREST);
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
    sceGuClutMode(GU_PSM_8888, 0, 0xFF, 0);
    sceGuAlphaFunc(GU_GREATER, 0, 0xFF);
    pspGEOpen       = true;
    pspGEMode       = PSPGE_NONE;
    pspGETexture    = NULL;
    pspGEClutLoaded   = -1;
    pspGEClutRingUsed = 0;
    memset(pspGEClut, 0, sizeof(pspGEClut));
}

static void PSPGEModeTextured()
{
    if (pspGEMode == PSPGE_TEXTURED)
        return;
    PSPGEFlushSprites();
    sceGuEnable(GU_TEXTURE_2D);
    sceGuEnable(GU_ALPHA_TEST);
    sceGuDisable(GU_BLEND);
    pspGEMode = PSPGE_TEXTURED;
}

static int pspGETextureW, pspGETextureH, pspGETextureBW;
static void PSPGETexture(const void *data, int width, int height, int bufferWidth)
{
    if (data == pspGETexture && width == pspGETextureW && height == pspGETextureH && bufferWidth == pspGETextureBW)
        return;
    PSPGEFlushSprites();
    sceGuTexMode(GU_PSM_T8, 0, 0, 0);
    sceGuTexImage(0, width, height, bufferWidth, data);
    sceGuTexFlush();
    pspGETexture   = data;
    pspGETextureW  = width;
    pspGETextureH  = height;
    pspGETextureBW = bufferWidth;
}

// palette pal as the CLUT (a new copy in the list if it changed since the last one made in this batch)
static uint pspGEClutVersion[PALETTE_COUNT]; // the paletteVersion the CLUT was last checked against
static void PSPGEClut(int pal)
{
    // the palette is only compared again when some palette changed since (paletteVersion)
    bool changed = !pspGEClut[pal];
    if (!changed && pspGEClutVersion[pal] != paletteVersion) {
        changed                = memcmp(pspGEClutSource[pal], fullPalette[pal], sizeof(pspGEClutSource[pal])) != 0;
        pspGEClutVersion[pal] = paletteVersion;
    }
    if (!changed && pal == pspGEClutLoaded)
        return;
    PSPGEFlushSprites(); // (the sprites waiting use the CLUT loaded now)
    if (changed) {
        // a slot of the CLUT ring, written through the uncached address (nothing to flush): it stays as it is until
        // the batch is over (the ring starts again with each batch; PSPGEBegin closes a batch that has used most of it)
        uint *clut = pspGEClutRing[pspGEClutRingUsed++];
        const ushort *src = fullPalette[pal];
        for (int i = 0; i < PALETTE_COLOR_COUNT; ++i) clut[i] = PSPColor8888(src[i], i ? 0xFF : 0);
        sceKernelDcacheWritebackRange(clut, PALETTE_COLOR_COUNT * sizeof(uint)); // (the GE reads it from memory)
        memcpy(pspGEClutSource[pal], src, sizeof(pspGEClutSource[pal]));
        pspGEClut[pal]        = clut;
        pspGEClutVersion[pal] = paletteVersion;
    }
    sceGuClutLoad(PALETTE_COLOR_COUNT / 8, pspGEClut[pal]);
    pspGEClutLoaded = pal;
}

static inline int PSPLinePalette(int y) { return gfxLineBuffer[y] < PALETTE_COUNT ? gfxLineBuffer[y] : 0; }

static inline int PSPPow2(int n)
{
    int p = 1;
    while (p < n) p <<= 1;
    return p;
}

// ---------------------------------------------------------------------------------------------
// Sprites
// ---------------------------------------------------------------------------------------------
// A sprite already clipped (as the CPU clips it): drawn at (x, y) width x height, from the sheet's pixels
// [srcLeft, srcLeft + width) x [srcTop, srcTop + height), mirrored as asked. False: the GE can't (the caller draws it).
// The texture for the sheet's pixels [srcLeft, srcLeft + width) x [srcTop, srcTop + height), set on the GE (opening the
// batch and the textured mode as needed); u0/vBase: where those pixels start in it. False: the GE can't.
static bool PSPGESheetTexture(int sheetID, int srcLeft, int srcTop, int width, int height, int *u0Out, int *vBaseOut)
{
    GFXSurface *surface = &gfxSurface[sheetID];
    if ((surface->width & 15) || surface->width > 1024 || width > 512 || height > 512 || srcLeft < 0 || srcTop < 0) {
        ++pspGEStatTooBig;
        return false;
    }

    // The texture: the whole sheet when it fits the GE's 512x512, else a 512x512 window at a 256 pixel step (so the
    // sprites of a big sheet still share a few textures): the texture only changes when the sheet (or window) does
    int winX = 0, winY = 0;
    if (surface->width > 512) {
        winX = srcLeft & ~255;
        if (srcLeft + width > winX + 512)
            winX = srcLeft & ~15;
    }
    if (surface->height > 512) {
        winY = srcTop & ~255;
        if (srcTop + height > winY + 512)
            winY = srcTop;
    }
    int texW = surface->width - winX < 512 ? PSPPow2(surface->width - winX) : 512;
    int texH = surface->height - winY < 512 ? PSPPow2(surface->height - winY) : 512;
    const byte *base = &graphicData[surface->dataPosition + winY * surface->width + winX];
    if ((uintptr_t)base & 15) {
        ++pspGEStatUnaligned;
        return false;
    }
    int u0    = srcLeft - winX;
    int vBase = srcTop - winY;
    if (u0 + width > 512 || vBase + height > 512) { // (a sprite almost 512 wide, not 16-aligned)
        ++pspGEStatTooBig;
        return false;
    }
    ++pspGEStatSprites;

    PSPGEBegin();
    PSPGEModeTextured();
    PSPGETexture(base, texW, texH, surface->width);
    *u0Out    = u0;
    *vBaseOut = vBase;
    return true;
}

// A sprite already clipped (as the CPU clips it): drawn at (x, y) width x height, from the sheet's pixels
// [srcLeft, srcLeft + width) x [srcTop, srcTop + height), mirrored as asked. False: the GE can't (the caller draws it).
static bool PSPDrawSpriteGE(int x, int y, int width, int height, int srcLeft, int srcTop, bool flipX, bool flipY, int sheetID)
{
    int u0, vBase;
    if (!PSPGESheetTexture(sheetID, srcLeft, srcTop, width, height, &u0, &vBase))
        return false;

    // one band per palette (the lines' palettes: usually one band)
    for (int band = 0; band < height;) {
        int pal = PSPLinePalette(y + band);
        int n   = 1;
        while (band + n < height && PSPLinePalette(y + band + n) == pal) ++n;
        PSPGEClut(pal);

        PSPTileVertex *v = PSPGEAddSprite();
        v[0].u = flipX ? u0 + width : u0;
        v[1].u = flipX ? u0 : u0 + width;
        v[0].v = vBase + (flipY ? height - band : band);
        v[1].v = vBase + (flipY ? height - band - n : band + n);
        v[0].x = x;
        v[0].y = y + band;
        v[1].x = x + width;
        v[1].y = y + band + n;
        v[0].z = v[1].z = 0;
        band += n;
    }
    return true;
}

// DrawSpriteRotated by the GE: the sprite's corners go where DrawSpriteRotated's own corner maths puts them (around
// the pivot, by the engine's 512-step angle and sine/cosine tables), and the GE fills the quad from the texture.
// The CPU draws it when it spans lines with different palettes (water). False: the caller draws it.
static bool PSPDrawSpriteRotatedGE(int direction, int XPos, int YPos, int pivotX, int pivotY, int sprX, int sprY, int width, int height,
                                   int rotation, int sheetID)
{
    if (width <= 0 || height <= 0)
        return true;
    int angle = rotation & 0x1FF;
    if (angle)
        angle = 0x200 - angle;
    float sine   = sin512LookupTable[angle] / 512.0f;
    float cosine = cos512LookupTable[angle] / 512.0f;

    // the corners, in the sprite's own space around the pivot: (lx, ly) -> screen (as the CPU code computes them)
    bool flipX  = direction == FLIP_X;
    float lx[2] = { flipX ? (float)pivotX : (float)-pivotX, flipX ? (float)(pivotX - width) : (float)(width - pivotX) };
    float ly[2] = { (float)-pivotY, (float)(height - pivotY) };
    float sx[4], sy[4];
    float top = 1e9f, bottom = -1e9f, left = 1e9f, right = -1e9f;
    for (int c = 0; c < 4; ++c) {
        float x = lx[c & 1], y = ly[c >> 1];
        sx[c]   = XPos + cosine * x + sine * y;
        sy[c]   = YPos + cosine * y - sine * x;
        top     = fminf(top, sy[c]);
        bottom  = fmaxf(bottom, sy[c]);
        left    = fminf(left, sx[c]);
        right   = fmaxf(right, sx[c]);
    }
    if (right <= 0 || left >= SCREEN_XSIZE || bottom <= 0 || top >= SCREEN_YSIZE)
        return true; // (nothing on screen)

    // one palette over the lines it covers
    int y0 = top < 0 ? 0 : (int)top, y1 = bottom > SCREEN_YSIZE - 1 ? SCREEN_YSIZE - 1 : (int)bottom;
    int pal = PSPLinePalette(y0);
    for (int y = y0 + 1; y <= y1; ++y)
        if (PSPLinePalette(y) != pal)
            return false;

    int u0, vBase;
    if (!PSPGESheetTexture(sheetID, sprX, sprY, width, height, &u0, &vBase))
        return false;
    PSPGEClut(pal);

    // corners 0..3 = (left, top) (right, top) (left, bottom) (right, bottom) of the texture: two triangles
    float u[2] = { (float)u0, (float)(u0 + width) };
    float v[2] = { (float)vBase, (float)(vBase + height) };
    static const int order[6] = { 0, 1, 2, 1, 3, 2 };
    PSPRotVertex *out = PSPGEAddRotSprite();
    for (int i = 0; i < 6; ++i) {
        int c    = order[i];
        out[i].u = u[c & 1];
        out[i].v = v[c >> 1];
        out[i].x = sx[c];
        out[i].y = sy[c];
        out[i].z = 0;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// Rectangles
// ---------------------------------------------------------------------------------------------
// A rectangle already clipped, in clr (16-bit), drawn solid (alpha 0xFF) or blended over the frame by alpha
static void PSPDrawRectangleGE(int x, int y, int width, int height, ushort clr, int alpha)
{
    PSPGEBegin();
    PSPGEFlushSprites();
    if (pspGEMode != PSPGE_FILL) {
        sceGuDisable(GU_TEXTURE_2D);
        sceGuDisable(GU_ALPHA_TEST);
        pspGEMode = PSPGE_FILL;
    }
    if (alpha >= 0xFF) {
        sceGuDisable(GU_BLEND);
    }
    else {
        sceGuEnable(GU_BLEND);
        sceGuBlendFunc(GU_ADD, GU_FIX, GU_FIX, alpha * 0x010101, (0xFF - alpha) * 0x010101);
    }
    PSPFillVertex *v = (PSPFillVertex *)sceGuGetMemory(2 * sizeof(PSPFillVertex));
    v[0].color = v[1].color = PSPColor8888(clr, 0xFF);
    v[0].x                  = x;
    v[0].y                  = y;
    v[1].x                  = x + width;
    v[1].y                  = y + height;
    v[0].z = v[1].z = 0;
    sceGuDrawArray(GU_SPRITES, GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 2, 0, v);
    if (alpha < 0xFF)
        sceGuDisable(GU_BLEND);
}

// ---------------------------------------------------------------------------------------------
// 3D faces (Draw3DScene: S2's special stage halfpipe is ~700 of them a frame)
// ---------------------------------------------------------------------------------------------
// The CPU fills a face as the outline of its 4 corners, whatever their order. For the GE: the two triangles that make
// it, by which pair of corners is the diagonal (the one whose segments cross). False: no such pair (a bent quad).
static inline long long PSPCross(const Vertex *o, const Vertex *a, const Vertex *b)
{
    return (long long)(a->x - o->x) * (b->y - o->y) - (long long)(a->y - o->y) * (b->x - o->x);
}
static inline bool PSPSegmentsCross(const Vertex *p1, const Vertex *p2, const Vertex *p3, const Vertex *p4)
{
    long long d1 = PSPCross(p3, p4, p1), d2 = PSPCross(p3, p4, p2);
    long long d3 = PSPCross(p1, p2, p3), d4 = PSPCross(p1, p2, p4);
    return ((d1 > 0 && d2 < 0) || (d1 < 0 && d2 > 0)) && ((d3 > 0 && d4 < 0) || (d3 < 0 && d4 > 0));
}
static bool PSPQuadTriangles(const Vertex *q, int order[6])
{
    static const int byDiagonal[3][6] = {
        { 0, 1, 2, 0, 2, 3 }, // diagonals 0-2 / 1-3: the outline is 0 1 2 3
        { 0, 1, 3, 0, 3, 2 }, // diagonals 0-3 / 1-2: the outline is 0 1 3 2
        { 0, 2, 1, 0, 1, 3 }, // diagonals 0-1 / 2-3: the outline is 0 2 1 3
    };
    int d = -1;
    if (PSPSegmentsCross(&q[0], &q[2], &q[1], &q[3]))
        d = 0;
    else if (PSPSegmentsCross(&q[0], &q[3], &q[1], &q[2]))
        d = 1;
    else if (PSPSegmentsCross(&q[0], &q[1], &q[2], &q[3]))
        d = 2;
    if (d < 0)
        return false;
    memcpy(order, byDiagonal[d], sizeof(byDiagonal[d]));
    return true;
}

// corners the GE can take (through mode: far off-screen ones are left to the CPU, which clips them itself)
static inline bool PSPQuadInRange(const Vertex *q)
{
    for (int c = 0; c < 4; ++c)
        if (q[c].x < -1024 || q[c].x > 1536 || q[c].y < -1024 || q[c].y > 1280)
            return false;
    return true;
}

// a face of one colour (16-bit, as the CPU would write it): false, the CPU draws it
static bool PSPDrawFaceGE(const Vertex *q, ushort color)
{
#ifdef PSP_NO_GE_FACES // (build switch: the CPU draws the faces, to compare)
    return false;
#endif
    if (!PSPQuadInRange(q))
        return false;
    int order[6];
    bool twoTriangles = PSPQuadTriangles(q, order);
    PSPGEBegin();
    if (pspGEMode != PSPGE_FILL) {
        PSPGEFlushSprites();
        sceGuDisable(GU_TEXTURE_2D);
        sceGuDisable(GU_ALPHA_TEST);
        sceGuDisable(GU_BLEND);
        pspGEMode = PSPGE_FILL;
    }
    uint c = PSPColor8888(color, 0xFF);
    if (twoTriangles) {
        PSPFillVertex *v = PSPGEAddFaceVerts(6);
        for (int i = 0; i < 6; ++i) {
            v[i].color = c;
            v[i].x     = q[order[i]].x;
            v[i].y     = q[order[i]].y;
            v[i].z     = 0;
        }
    }
    else {
        // a bent quad: all four triangles of its corners fill its outline (one colour: overlaps don't show)
        static const int all[12] = { 0, 1, 2, 0, 1, 3, 0, 2, 3, 1, 2, 3 };
        PSPFillVertex *v = PSPGEAddFaceVerts(12);
        for (int i = 0; i < 12; ++i) {
            v[i].color = c;
            v[i].x     = q[all[i]].x;
            v[i].y     = q[all[i]].y;
            v[i].z     = 0;
        }
    }
    return true;
}

// a face textured from a sprite sheet (its corners' u/v; index 0 see-through): false, the CPU draws it
static bool PSPDrawTexturedFaceGE(const Vertex *q, int sheetID, bool half = false)
{
#ifdef PSP_NO_GE_FACES
    return false;
#endif
    if (!PSPQuadInRange(q))
        return false;
    int order[6];
    if (!PSPQuadTriangles(q, order))
        return false;

    int top = q[0].y, bottom = q[0].y, minU = q[0].u, maxU = q[0].u, minV = q[0].v, maxV = q[0].v;
    for (int c = 1; c < 4; ++c) {
        top    = q[c].y < top ? q[c].y : top;
        bottom = q[c].y > bottom ? q[c].y : bottom;
        minU   = q[c].u < minU ? q[c].u : minU;
        maxU   = q[c].u > maxU ? q[c].u : maxU;
        minV   = q[c].v < minV ? q[c].v : minV;
        maxV   = q[c].v > maxV ? q[c].v : maxV;
    }
    // one palette over the lines it covers
    int y0 = top < 0 ? 0 : top, y1 = bottom > SCREEN_YSIZE - 1 ? SCREEN_YSIZE - 1 : bottom;
    int pal = PSPLinePalette(y0 < SCREEN_YSIZE ? y0 : SCREEN_YSIZE - 1);
    for (int y = y0 + 1; y <= y1; ++y)
        if (PSPLinePalette(y) != pal)
            return false;

    int u0, vBase;
    if (!PSPGESheetTexture(sheetID, minU, minV, maxU - minU + 1, maxV - minV + 1, &u0, &vBase))
        return false;
    PSPGEClut(pal);
    if (half) { // (DrawTexturedFaceBlended: half the face, half the frame)
        PSPGEFlushSprites();
        sceGuEnable(GU_BLEND);
        sceGuBlendFunc(GU_ADD, GU_FIX, GU_FIX, 0x808080, 0x808080);
    }
    PSPRotVertex *out = PSPGEAddRotSprite();
    for (int i = 0; i < 6; ++i) {
        const Vertex *c = &q[order[i]];
        out[i].u        = (float)(c->u - minU + u0);
        out[i].v        = (float)(c->v - minV + vBase);
        out[i].x        = (float)c->x;
        out[i].y        = (float)c->y;
        out[i].z        = 0;
    }
    if (half) {
        PSPGEFlushSprites();
        sceGuDisable(GU_BLEND);
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// Tile layers
// ---------------------------------------------------------------------------------------------
// One run: lines [y, y + lines) of the layer, all at the same horizontal position (chunkX) in the same tile row
static void PSPDrawLayerRun(TileLayer *layer, int layerwidth, int chunkX, int chunkY, int tileY, int tileY16, int y, int lines,
                            bool aboveMidPoint)
{
    int chunkXPos    = chunkX >> 7;
    int chunkTileX   = (chunkX & 0x7F) >> 4;
    int chunk        = (layer->tiles[chunkXPos + (chunkY << 8)] << 6) + chunkTileX + 8 * tileY;
    int x            = -(chunkX & 0xF);
    int maxSprites   = (SCREEN_XSIZE + 15) / TILE_SIZE + 1;
    PSPTileVertex *v = (PSPTileVertex *)sceGuGetMemory(maxSprites * 2 * sizeof(PSPTileVertex));
    int count        = 0;

    while (x < SCREEN_XSIZE) {
        if (tiles128x128.visualPlane[chunk] == (byte)aboveMidPoint) {
            int tile = tiles128x128.gfxDataPos[chunk] >> 8;
            int dir  = tiles128x128.direction[chunk];
            int tu   = (tile & 31) * TILE_SIZE;
            int tv   = (tile >> 5) * TILE_SIZE;
            int v0   = (dir & FLIP_Y) ? tv + TILE_SIZE - tileY16 : tv + tileY16;
            v[0].u   = (dir & FLIP_X) ? tu + TILE_SIZE : tu;
            v[0].v   = v0;
            v[0].x   = x;
            v[0].y   = y;
            v[0].z   = 0;
            v[1].u   = (dir & FLIP_X) ? tu : tu + TILE_SIZE;
            v[1].v   = (dir & FLIP_Y) ? v0 - lines : v0 + lines;
            v[1].x   = x + TILE_SIZE;
            v[1].y   = y + lines;
            v[1].z   = 0;
            v += 2;
            ++count;
        }
        x += TILE_SIZE;
        if (++chunkTileX < 8) {
            ++chunk;
        }
        else {
            if (++chunkXPos == layerwidth)
                chunkXPos = 0;
            chunkTileX = 0;
            chunk      = (layer->tiles[chunkXPos + (chunkY << 8)] << 6) + 8 * tileY;
        }
    }
    if (count)
        sceGuDrawArray(GU_SPRITES, GU_TEXTURE_16BIT | GU_VERTEX_16BIT | GU_TRANSFORM_2D, count * 2, 0, v - count * 2);
}

// The drawing part of DrawHLineScrollLayer, by the GE: the same walk over the lines, in runs
static void PSPDrawHLineScrollLayerGE(TileLayer *layer, int layerwidth, int layerheight, bool aboveMidPoint, byte *lineScroll,
                                      int *deformationData, int *deformationDataW, int tileYPos)
{
    PSPGEBegin();
    PSPGEFlushSprites(); // (the runs below are drawn on their own)
    PSPGEModeTextured();
    PSPGETexture(PSPVram(PSP_VRAM_TILESET), 512, 512, 512);

    byte *scrollIndex  = &lineScroll[tileYPos];
    int tileY16        = tileYPos & 0xF;
    int chunkY         = tileYPos >> 7;
    int tileY          = (tileYPos & 0x7F) >> 4;
    int fullLayerwidth = layerwidth << 7;

    int runY = 0, runLines = 0, runX = 0, runPal = -1, runChunkY = 0, runTileY = 0, runTileY16 = 0;

    int y                = 0;
    int drawableLines[2] = { waterDrawPos, SCREEN_YSIZE - waterDrawPos };
    for (int i = 0; i < 2; ++i) {
        while (drawableLines[i]--) {
            int pal    = PSPLinePalette(y);
            int chunkX = hParallax.linePos[*scrollIndex];
            if (i == 0) {
                if (hParallax.deform[*scrollIndex])
                    chunkX += *deformationData;
                ++deformationData;
            }
            else {
                if (hParallax.deform[*scrollIndex])
                    chunkX += *deformationDataW;
                ++deformationDataW;
            }
            ++scrollIndex;
            if (chunkX < 0)
                chunkX += fullLayerwidth;
            if (chunkX >= fullLayerwidth)
                chunkX -= fullLayerwidth;

            // a new run unless this line carries on the last one (same position, palette and tile row)
            if (!runLines || chunkX != runX || pal != runPal || tileY16 == 0) {
                if (runLines) {
                    PSPGEClut(runPal);
                    PSPDrawLayerRun(layer, layerwidth, runX, runChunkY, runTileY, runTileY16, runY, runLines, aboveMidPoint);
                }
                runY       = y;
                runLines   = 0;
                runX       = chunkX;
                runPal     = pal;
                runChunkY  = chunkY;
                runTileY   = tileY;
                runTileY16 = tileY16;
            }
            ++runLines;
            ++y;

            if (++tileY16 >= TILE_SIZE) {
                tileY16 = 0;
                ++tileY;
            }
            if (tileY >= 8) {
                if (++chunkY == layerheight) {
                    chunkY = 0;
                    scrollIndex -= 0x80 * layerheight;
                }
                tileY = 0;
            }
        }
    }
    if (runLines) {
        PSPGEClut(runPal);
        PSPDrawLayerRun(layer, layerwidth, runX, runChunkY, runTileY, runTileY16, runY, runLines, aboveMidPoint);
    }
}
