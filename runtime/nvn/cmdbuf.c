// NVN command buffers: commands are recorded into the guest-provided command memory as CmdRec records, so the
// game's own memory management and out-of-memory callbacks work as designed. A command handle is the guest address of
// a list's first record. Port of LAS Recomp's runtime/nvn/cmdbuf.c: records hold the call's arguments as the adapter
// (nvn.h) hands them over, so the executor reads them the same way on 32- and 64-bit games.
#include <stdlib.h>
#include <string.h>

#include "nvn.h"

#define JUMP_RESERVE (sizeof(CmdRec) + 8)

static NvnCmdBuf *cb_of(uint64_t guest) { return HOST(NvnCmdBuf, guest); }

NVN(nvnCommandBufferInitialize) {
    NvnCmdBuf *cb = calloc(1, sizeof *cb);
    cb->guest = A(0);
    SET_HOST(A(0), cb);
    RET(1);
}
NVN(nvnCommandBufferFinalize) { free(cb_of(A(0))); SET_HOST(A(0), 0); }
NVN(nvnCommandBufferSetDebugLabel) { (void)c; NVN_NOOP(); }

static CmdRec *raw_rec(uint64_t at, uint32_t op, uint32_t size) {
    CmdRec *r = (CmdRec *)G2H((uint32_t)at);
    memset(r, 0, sizeof *r);
    r->op = op;
    r->size = size;
    return r;
}

NVN(nvnCommandBufferAddCommandMemory) {  // (cmd, pool, ptrdiff_t offset, size_t size)
    NvnCmdBuf *cb = cb_of(A(0));
    uint64_t mem = HOST(NvnPool, A(1))->storage + A(2), size = A(3);
    if (cb->recording && cb->mem) {  // chain the list being recorded into the new chunk
        CmdRec *j = raw_rec(cb->mem + cb->used, CMD_JUMP, sizeof(CmdRec));
        j->x[0] = mem;
    }
    cb->mem = mem;
    cb->size = size;
    cb->used = 0;
}
NVN(nvnCommandBufferAddControlMemory) { (void)c; NVN_NOOP(); }
NVN(nvnCommandBufferGetCommandMemorySize) { RET(cb_of(A(0))->size); }
NVN(nvnCommandBufferGetCommandMemoryUsed) { RET(cb_of(A(0))->used); }
NVN(nvnCommandBufferGetCommandMemoryFree) {
    NvnCmdBuf *cb = cb_of(A(0));
    RET(cb->size > cb->used + JUMP_RESERVE ? cb->size - cb->used - JUMP_RESERVE : 0);
}
NVN(nvnCommandBufferGetControlMemorySize) { RET(0x10000); }
NVN(nvnCommandBufferGetControlMemoryUsed) { RET(0); }
NVN(nvnCommandBufferGetControlMemoryFree) { RET(0x10000); }
NVN(nvnCommandBufferSetMemoryCallback) { cb_of(A(0))->callback = A(1); }
NVN(nvnCommandBufferGetMemoryCallback) { RET(cb_of(A(0))->callback); }
NVN(nvnCommandBufferSetMemoryCallbackData) { cb_of(A(0))->callback_data = A(1); }
NVN(nvnCommandBufferGetMemoryCallbackData) { RET(cb_of(A(0))->callback_data); }
NVN(nvnCommandBufferSetCommandMemoryCallbackEnabled) { (void)c; NVN_NOOP(); }

// Reserve a record. May call back into the guest for more memory, so callers snapshot their arguments first.
static CmdRec *rec(Cpu *c, NvnCmdBuf *cb, uint32_t op, uint32_t extra) {
    uint32_t size = (uint32_t)((sizeof(CmdRec) + extra + 7) & ~7u);
    for (int tries = 0; !cb->mem || cb->used + size + JUMP_RESERVE > cb->size; tries++) {
        if (!cb->callback || tries == 2) FATAL("nvn: out of command memory (need %u)\n", size);
        uint32_t saved[16];
        memcpy(saved, c->r, sizeof saved);
        // callback(NVNcommandBuffer *, NVNcommandBufferMemoryEvent, size_t minSize, void *data)
        c->r[0] = (uint32_t)cb->guest;
        c->r[1] = 0;  // NVN_COMMAND_BUFFER_MEMORY_EVENT_OUT_OF_COMMAND_MEMORY
        c->r[2] = size + (uint32_t)JUMP_RESERVE;
        c->r[3] = (uint32_t)cb->callback_data;
        guest_call(c, (uint32_t)cb->callback);
        memcpy(c->r, saved, sizeof saved);
    }
    CmdRec *r = raw_rec(cb->mem + cb->used, op, size);
    cb->used += size;
    return r;
}

NVN(nvnCommandBufferBeginRecording) {
    NvnCmdBuf *cb = cb_of(A(0));
    cb->recording = true;
    rec(c, cb, CMD_BARRIER, 0);  // makes sure memory exists; the list starts here
    cb->start = cb->mem + cb->used - sizeof(CmdRec);
}
NVN(nvnCommandBufferEndRecording) {
    NvnCmdBuf *cb = cb_of(A(0));
    rec(c, cb, CMD_END, 0);
    cb->recording = false;
    RET64(cb->start);
}
NVN(nvnCommandBufferIsRecording) { RET(cb_of(A(0))->recording); }

// ---- generic recorders
static CmdRec *record(Cpu *c, uint32_t op, uint32_t extra, NvnArgs *s) {
    CmdRec *r = rec(c, cb_of(s->x[0]), op, extra);
    memcpy(r->x, s->x + 1, sizeof r->x);
    memcpy(r->f, s->f, sizeof r->f);
    return r;
}
// record with a copy of `len` bytes from the guest pointer in argument `arg`
static CmdRec *record_copy(Cpu *c, NvnArgs *s, uint32_t op, int arg, uint32_t len) {
    NvnArgs keep = *s;
    CmdRec *r = record(c, op, len, &keep);
    if (keep.x[arg] && len) memcpy(r->extra, G2H((uint32_t)keep.x[arg]), len);
    return r;
}
#define PLAIN(name, op) NVN(name) { record(c, op, 0, na_); }
#define PLAINS(name, sig, op) NVNS(name, sig) { record(c, op, 0, na_); }

// ---- state binding
NVN(nvnCommandBufferBindProgram) {
    CmdRec *r = record(c, CMD_BIND_PROGRAM, 0, na_);
    r->x[0] = (uint64_t)(uintptr_t)HOST(NvnProgram, na_->x[1]);  // host program
}
NVN(nvnCommandBufferBindBlendState) { record_copy(c, na_, CMD_BIND_BLEND, 1, 8); }
NVN(nvnCommandBufferBindChannelMaskState) { record_copy(c, na_, CMD_BIND_CHANNEL_MASK, 1, 4); }
NVN(nvnCommandBufferBindColorState) { record_copy(c, na_, CMD_BIND_COLOR, 1, 4); }
NVN(nvnCommandBufferBindDepthStencilState) { record_copy(c, na_, CMD_BIND_DEPTH_STENCIL, 1, 8); }
NVN(nvnCommandBufferBindMultisampleState) { record_copy(c, na_, CMD_BIND_MULTISAMPLE, 1, 0x18); }
NVN(nvnCommandBufferBindPolygonState) { record_copy(c, na_, CMD_BIND_POLYGON, 1, 4); }
NVN(nvnCommandBufferBindVertexAttribState) { record_copy(c, na_, CMD_BIND_VERTEX_ATTRIB, 2, (uint32_t)A(1) * 4); }
NVN(nvnCommandBufferBindVertexStreamState) { record_copy(c, na_, CMD_BIND_VERTEX_STREAM, 2, (uint32_t)A(1) * 8); }

// (cmd, first, count, const NVNbufferRange *) and the single-binding forms. NVNbufferRange = {u64 address, u64 size}.
NVN(nvnCommandBufferBindVertexBuffers) { record_copy(c, na_, CMD_BIND_VERTEX_BUFFERS, 3, (uint32_t)A(2) * 16); }
NVNS(nvnCommandBufferBindVertexBuffer, "iili") {  // (cmd, index, address, size)
    NvnArgs s = *na_;
    CmdRec *r = record(c, CMD_BIND_VERTEX_BUFFERS, 16, &s);
    r->x[0] = s.x[1]; r->x[1] = 1;
    memcpy(r->extra, &s.x[2], 16);
}
NVN(nvnCommandBufferBindUniformBuffers) { record_copy(c, na_, CMD_BIND_UNIFORM_BUFFERS, 4, (uint32_t)A(3) * 16); }
NVNS(nvnCommandBufferBindUniformBuffer, "iiili") {  // (cmd, stage, index, address, size)
    NvnArgs s = *na_;
    CmdRec *r = record(c, CMD_BIND_UNIFORM_BUFFERS, 16, &s);
    r->x[0] = s.x[1]; r->x[1] = s.x[2]; r->x[2] = 1;
    memcpy(r->extra, &s.x[3], 16);
}
NVN(nvnCommandBufferBindStorageBuffers) { record_copy(c, na_, CMD_BIND_STORAGE_BUFFERS, 4, (uint32_t)A(3) * 16); }
NVNS(nvnCommandBufferBindStorageBuffer, "iiili") {
    NvnArgs s = *na_;
    CmdRec *r = record(c, CMD_BIND_STORAGE_BUFFERS, 16, &s);
    r->x[0] = s.x[1]; r->x[1] = s.x[2]; r->x[2] = 1;
    memcpy(r->extra, &s.x[3], 16);
}
// (cmd, stage, first, count, const handle *): 64-bit handles
NVN(nvnCommandBufferBindTextures) { record_copy(c, na_, CMD_BIND_TEXTURES, 4, (uint32_t)A(3) * 8); }
static void bind_one(Cpu *c, NvnArgs *na_, uint32_t op) {
    NvnArgs s = *na_;
    CmdRec *r = record(c, op, 8, &s);
    r->x[0] = s.x[1]; r->x[1] = s.x[2]; r->x[2] = 1;
    memcpy(r->extra, &s.x[3], 8);
}
NVNS(nvnCommandBufferBindTexture, "iiil") { bind_one(c, na_, CMD_BIND_TEXTURES); }
NVN(nvnCommandBufferBindImages) { record_copy(c, na_, CMD_BIND_IMAGES, 4, (uint32_t)A(3) * 8); }
NVNS(nvnCommandBufferBindImage, "iiil") { bind_one(c, na_, CMD_BIND_IMAGES); }
NVN(nvnCommandBufferBindSeparateTextures) { record_copy(c, na_, CMD_BIND_TEXTURES, 4, (uint32_t)A(3) * 8); }
NVNS(nvnCommandBufferBindSeparateTexture, "iiil") { bind_one(c, na_, CMD_BIND_TEXTURES); }
NVN(nvnCommandBufferBindSeparateSamplers) { (void)c; NVN_NOOP(); }
NVNS(nvnCommandBufferBindSeparateSampler, "iiil") { (void)c; (void)na_; NVN_NOOP(); }
NVN(nvnCommandBufferBindTransformFeedbackBuffers) { (void)c; NVN_NOOP(); }
NVNS(nvnCommandBufferBindTransformFeedbackBuffer, "iili") { (void)c; (void)na_; NVN_NOOP(); }
NVN(nvnCommandBufferBindCoverageModulationTable) { (void)c; NVN_NOOP(); }

NVN(nvnCommandBufferSetTexturePool) {
    CmdRec *r = record(c, CMD_SET_TEXTURE_POOL, 0, na_);
    r->x[0] = (uint64_t)(uintptr_t)HOST(NvnDescPool, na_->x[1]);
}
NVN(nvnCommandBufferSetSamplerPool) {
    CmdRec *r = record(c, CMD_SET_SAMPLER_POOL, 0, na_);
    r->x[0] = (uint64_t)(uintptr_t)HOST(NvnDescPool, na_->x[1]);
}

// SetRenderTargets(cmd, n, const NVNtexture *const *colors, const NVNtextureView *const *views, depth, depthView):
// the arrays hold 32-bit guest pointers
NVN(nvnCommandBufferSetRenderTargets) {
    NvnArgs s = *na_;
    int n = (int)s.x[1];
    if (n > 8) n = 8;
    RtRec rt = {0};
    uint32_t colors = (uint32_t)s.x[2], views = (uint32_t)s.x[3];
    for (int i = 0; i < n; i++) {
        uint32_t ct = colors ? LD32(colors + 4u * (uint32_t)i) : 0, cv = views ? LD32(views + 4u * (uint32_t)i) : 0;
        rt.tex[i] = ct ? (uint64_t)(uintptr_t)HOST(NvnTexture, ct) : 0;
        if (cv) { rt.has_view[i] = 1; memcpy(&rt.views[i], G2H(cv), sizeof(NvnTextureView)); }
    }
    rt.depth = s.x[4] ? (uint64_t)(uintptr_t)HOST(NvnTexture, s.x[4]) : 0;
    if (s.x[5]) { rt.has_dview = 1; memcpy(&rt.dview, G2H((uint32_t)s.x[5]), sizeof(NvnTextureView)); }
    CmdRec *r = record(c, CMD_SET_RENDER_TARGETS, sizeof rt, &s);
    r->x[0] = (uint64_t)n;
    memcpy(r->extra, &rt, sizeof rt);
}

// viewports / scissors / depth ranges
NVN(nvnCommandBufferSetViewports) { record_copy(c, na_, CMD_SET_VIEWPORTS, 3, (uint32_t)A(2) * 16); }
NVN(nvnCommandBufferSetViewport) {
    NvnArgs s = *na_;
    CmdRec *r = record(c, CMD_SET_VIEWPORTS, 16, &s);
    float v[4] = {(float)(int32_t)s.x[1], (float)(int32_t)s.x[2], (float)(int32_t)s.x[3], (float)(int32_t)s.x[4]};
    r->x[0] = 0; r->x[1] = 1;
    memcpy(r->extra, v, 16);
}
NVN(nvnCommandBufferSetScissors) { record_copy(c, na_, CMD_SET_SCISSORS, 3, (uint32_t)A(2) * 16); }
NVN(nvnCommandBufferSetScissor) {
    NvnArgs s = *na_;
    CmdRec *r = record(c, CMD_SET_SCISSORS, 16, &s);
    int32_t v[4] = {(int32_t)s.x[1], (int32_t)s.x[2], (int32_t)s.x[3], (int32_t)s.x[4]};
    r->x[0] = 0; r->x[1] = 1;
    memcpy(r->extra, v, 16);
}
NVN(nvnCommandBufferSetDepthRanges) { record_copy(c, na_, CMD_SET_DEPTH_RANGES, 3, (uint32_t)A(2) * 8); }
NVNS(nvnCommandBufferSetDepthRange, "iff") {
    NvnArgs s = *na_;
    CmdRec *r = record(c, CMD_SET_DEPTH_RANGES, 8, &s);
    r->x[0] = 0; r->x[1] = 1;
    memcpy(r->extra, s.f, 8);
}
NVN(nvnCommandBufferSetBlendColor) { record_copy(c, na_, CMD_SET_BLEND_COLOR, 1, 16); }
NVN(nvnCommandBufferSetStencilRef) { CmdRec *r = record(c, CMD_SET_STENCIL, 0, na_); r->x[6] = 0; }
NVN(nvnCommandBufferSetStencilMask) { CmdRec *r = record(c, CMD_SET_STENCIL, 0, na_); r->x[6] = 1; }
NVN(nvnCommandBufferSetStencilValueMask) { CmdRec *r = record(c, CMD_SET_STENCIL, 0, na_); r->x[6] = 2; }
PLAIN(nvnCommandBufferSetPrimitiveRestart, CMD_SET_PRIMITIVE_RESTART)
PLAINS(nvnCommandBufferSetAlphaRef, "if", CMD_SET_ALPHA_REF)
PLAINS(nvnCommandBufferSetDepthBounds, "iiff", CMD_SET_DEPTH_BOUNDS)
PLAINS(nvnCommandBufferSetPolygonOffsetClamp, "ifff", CMD_SET_POLYGON_OFFSET)
PLAINS(nvnCommandBufferSetLineWidth, "if", CMD_SET_LINE_WIDTH)
PLAINS(nvnCommandBufferSetPointSize, "if", CMD_SET_POINT_SIZE)
PLAIN(nvnCommandBufferSetSampleMask, CMD_SET_SAMPLE_MASK)
PLAIN(nvnCommandBufferSetRenderEnable, CMD_SET_RENDER_ENABLE)
NVNS(nvnCommandBufferSetRenderEnableConditional, "iil") { (void)c; (void)na_; NVN_NOOP(); }
PLAIN(nvnCommandBufferSetDepthClamp, CMD_SET_DEPTH_CLAMP)
NVN(nvnCommandBufferSetRasterizerDiscard) { (void)c; NVN_NOOP(); }
NVN(nvnCommandBufferSetConservativeRasterEnable) { (void)c; NVN_NOOP(); }
NVNS(nvnCommandBufferSetConservativeRasterDilate, "if") { (void)c; (void)na_; NVN_NOOP(); }
NVN(nvnCommandBufferSetSubpixelPrecisionBias) { (void)c; NVN_NOOP(); }
NVN(nvnCommandBufferSetViewportSwizzles) { (void)c; NVN_NOOP(); }
NVN(nvnCommandBufferSetStencilCullCriteria) { (void)c; NVN_NOOP(); }
NVN(nvnCommandBufferSetColorReductionEnable) { (void)c; NVN_NOOP(); }
NVN(nvnCommandBufferSetColorReductionThresholds) { (void)c; NVN_NOOP(); }
NVN(nvnCommandBufferSetTiledCacheAction) { (void)c; NVN_NOOP(); }
NVN(nvnCommandBufferSetTiledCacheTileSize) { (void)c; NVN_NOOP(); }
NVN(nvnCommandBufferSetShaderScratchMemory) { (void)c; NVN_NOOP(); }
NVN(nvnCommandBufferSetPatchSize) { (void)c; NVN_NOOP(); }
NVN(nvnCommandBufferSetInnerTessellationLevels) { (void)c; NVN_NOOP(); }
NVN(nvnCommandBufferSetOuterTessellationLevels) { (void)c; NVN_NOOP(); }
NVN(nvnCommandBufferSetProgramSubroutines) { (void)c; NVN_NOOP(); }
NVN(nvnCommandBufferSaveZCullData) { (void)c; NVN_NOOP(); }
NVN(nvnCommandBufferRestoreZCullData) { (void)c; NVN_NOOP(); }

static uint64_t copy_row_stride, copy_image_stride;
NVN(nvnCommandBufferSetCopyRowStride) {
    copy_row_stride = (uint32_t)A(1);
    CmdRec *r = record(c, CMD_SET_COPY_STRIDES, 0, na_);
    r->x[0] = copy_row_stride; r->x[1] = copy_image_stride;
}
NVN(nvnCommandBufferSetCopyImageStride) {
    copy_image_stride = (uint32_t)A(1);
    CmdRec *r = record(c, CMD_SET_COPY_STRIDES, 0, na_);
    r->x[0] = copy_row_stride; r->x[1] = copy_image_stride;
}
NVN(nvnCommandBufferGetCopyRowStride) { RET(copy_row_stride); }
NVN(nvnCommandBufferGetCopyImageStride) { RET(copy_image_stride); }

// ---- clears
NVN(nvnCommandBufferClearColor) { record_copy(c, na_, CMD_CLEAR_COLOR, 2, 16); }
NVN(nvnCommandBufferClearColori) { CmdRec *r = record_copy(c, na_, CMD_CLEAR_COLOR, 2, 16); r->x[5] = 1; }
NVN(nvnCommandBufferClearColorui) { CmdRec *r = record_copy(c, na_, CMD_CLEAR_COLOR, 2, 16); r->x[5] = 2; }
PLAINS(nvnCommandBufferClearDepthStencil, "ifiii", CMD_CLEAR_DEPTH_STENCIL)  // (cmd, depth, depthMask, stencil, mask)
static void clear_texture(Cpu *c, NvnArgs *na_, int kind) {  // (cmd, texture, view, region, color, mask)
    NvnArgs s = *na_;
    ClearTexRec ct = {0};
    ct.tex = (uint64_t)(uintptr_t)HOST(NvnTexture, s.x[1]);
    if (s.x[2]) { ct.has_view = 1; memcpy(&ct.view, G2H((uint32_t)s.x[2]), sizeof ct.view); }
    if (s.x[3]) memcpy(ct.region, G2H((uint32_t)s.x[3]), sizeof ct.region);
    if (s.x[4]) memcpy(ct.color, G2H((uint32_t)s.x[4]), 16);
    CmdRec *r = record(c, CMD_CLEAR_TEXTURE, sizeof ct, &s);
    r->x[6] = (uint64_t)kind;
    memcpy(r->extra, &ct, sizeof ct);
}
NVN(nvnCommandBufferClearTexture) { clear_texture(c, na_, 0); }
NVN(nvnCommandBufferClearTexturei) { clear_texture(c, na_, 1); }
NVN(nvnCommandBufferClearTextureui) { clear_texture(c, na_, 2); }
PLAINS(nvnCommandBufferClearBuffer, "ilii", CMD_CLEAR_BUFFER)  // (cmd, address, size, value)
// DiscardColor(cmd, index): treated as a clear to (0,0,0,0), as a discarded (compressed) target reads on the Switch
NVN(nvnCommandBufferDiscardColor) {
    NvnArgs s = *na_;
    CmdRec *r = record(c, CMD_CLEAR_COLOR, 16, &s);
    memset(r->extra, 0, 16);
    r->x[2] = 0xF;  // all channels
    r->x[5] = 0;    // float clear
}
NVN(nvnCommandBufferDiscardDepthStencil) { (void)c; NVN_NOOP(); }

// ---- draws
NVN(nvnCommandBufferDrawArrays) { CmdRec *r = record(c, CMD_DRAW_ARRAYS, 0, na_); r->x[3] = 0; r->x[4] = 1; }
NVN(nvnCommandBufferDrawArraysInstanced) { record(c, CMD_DRAW_ARRAYS, 0, na_); }  // (mode, first, count, baseInst, n)
NVNS(nvnCommandBufferDrawElements, "iiiil") {  // (cmd, mode, type, count, index buffer)
    CmdRec *r = record(c, CMD_DRAW_ELEMENTS, 0, na_);
    r->x[4] = 0; r->x[5] = 0; r->x[6] = 1;  // base vertex, base instance, instances
}
NVNS(nvnCommandBufferDrawElementsBaseVertex, "iiiili") {
    CmdRec *r = record(c, CMD_DRAW_ELEMENTS, 0, na_);
    r->x[5] = 0; r->x[6] = 1;
}
PLAINS(nvnCommandBufferDrawElementsInstanced, "iiiiliii", CMD_DRAW_ELEMENTS)
PLAINS(nvnCommandBufferDrawArraysIndirect, "iil", CMD_DRAW_ARRAYS_INDIRECT)
PLAINS(nvnCommandBufferDrawElementsIndirect, "iiill", CMD_DRAW_ELEMENTS_INDIRECT)
NVN(nvnCommandBufferMultiDrawArraysIndirectCount) { (void)c; NVN_NOOP(); }
NVN(nvnCommandBufferMultiDrawElementsIndirectCount) { (void)c; NVN_NOOP(); }
NVN(nvnCommandBufferDrawTransformFeedback) { (void)c; NVN_NOOP(); }
NVN(nvnCommandBufferDrawTexture) { (void)c; NVN_NOOP(); }
PLAIN(nvnCommandBufferDispatchCompute, CMD_DISPATCH)
PLAINS(nvnCommandBufferDispatchComputeIndirect, "il", CMD_DISPATCH_INDIRECT)
NVN(nvnCommandBufferBeginTransformFeedback) { (void)c; NVN_NOOP(); }
NVN(nvnCommandBufferEndTransformFeedback) { (void)c; NVN_NOOP(); }
NVN(nvnCommandBufferPauseTransformFeedback) { (void)c; NVN_NOOP(); }
NVN(nvnCommandBufferResumeTransformFeedback) { (void)c; NVN_NOOP(); }

// ---- copies
static void tex_ref(TexRef *t, uint64_t tex, uint64_t view, uint64_t region) {
    t->tex = tex ? (uint64_t)(uintptr_t)HOST(NvnTexture, tex) : 0;
    if (view) { t->has_view = 1; memcpy(&t->view, G2H((uint32_t)view), sizeof t->view); }
    if (region) memcpy(t->region, G2H((uint32_t)region), sizeof t->region);
}
NVNS(nvnCommandBufferCopyBufferToTexture, "iliiii") {  // (cmd, src, dst, dstView, dstRegion, flags)
    NvnArgs s = *na_;
    TexRef t = {0};
    tex_ref(&t, s.x[2], s.x[3], s.x[4]);
    CmdRec *r = record(c, CMD_COPY_BUFFER_TO_TEXTURE, sizeof t, &s);
    memcpy(r->extra, &t, sizeof t);
}
NVNS(nvnCommandBufferCopyTextureToBuffer, "iiiili") {  // (cmd, src, srcView, srcRegion, dst, flags)
    NvnArgs s = *na_;
    TexRef t = {0};
    tex_ref(&t, s.x[1], s.x[2], s.x[3]);
    CmdRec *r = record(c, CMD_COPY_TEXTURE_TO_BUFFER, sizeof t, &s);
    memcpy(r->extra, &t, sizeof t);
}
NVN(nvnCommandBufferCopyTextureToTexture) {  // (cmd, src, srcView, srcRegion, dst, dstView, dstRegion, flags)
    NvnArgs s = *na_;
    TexRef t[2] = {{0}};
    tex_ref(&t[0], s.x[1], s.x[2], s.x[3]);
    tex_ref(&t[1], s.x[4], s.x[5], s.x[6]);
    CmdRec *r = record(c, CMD_COPY_TEXTURE_TO_TEXTURE, sizeof t, &s);
    memcpy(r->extra, t, sizeof t);
}
PLAINS(nvnCommandBufferCopyBufferToBuffer, "illii", CMD_COPY_BUFFER_TO_BUFFER)  // (cmd, src, dst, size, flags)
NVN(nvnCommandBufferDownsample) {
    NvnArgs s = *na_;
    TexRef t[2] = {{0}};
    tex_ref(&t[0], s.x[1], 0, 0);
    tex_ref(&t[1], s.x[2], 0, 0);
    CmdRec *r = record(c, CMD_DOWNSAMPLE, sizeof t, &s);
    memcpy(r->extra, t, sizeof t);
}
NVN(nvnCommandBufferDownsampleTextureView) {
    NvnArgs s = *na_;
    TexRef t[2] = {{0}};
    tex_ref(&t[0], s.x[1], s.x[2], 0);
    tex_ref(&t[1], s.x[3], s.x[4], 0);
    CmdRec *r = record(c, CMD_DOWNSAMPLE, sizeof t, &s);
    memcpy(r->extra, t, sizeof t);
}
NVN(nvnCommandBufferTiledDownsample) { (void)c; NVN_NOOP(); }
NVN(nvnCommandBufferTiledDownsampleTextureView) { (void)c; NVN_NOOP(); }
NVN(nvnCommandBufferResolveDepthBuffer) { (void)c; NVN_NOOP(); }
// UpdateUniformBuffer(cmd, buffer address, buffer size, update offset, update size, data)
NVNS(nvnCommandBufferUpdateUniformBuffer, "iliiii") { record_copy(c, na_, CMD_UPDATE_UNIFORM_BUFFER, 5, (uint32_t)A(4)); }

// ---- synchronization and misc
NVN(nvnCommandBufferFenceSync) { record(c, CMD_FENCE_SYNC, 0, na_); }  // satisfied once this list has run
PLAIN(nvnCommandBufferWaitSync, CMD_WAIT_SYNC)
PLAIN(nvnCommandBufferBarrier, CMD_BARRIER)
PLAIN(nvnCommandBufferSignalEvent, CMD_SIGNAL_EVENT)
PLAIN(nvnCommandBufferWaitEvent, CMD_WAIT_EVENT)
PLAINS(nvnCommandBufferReportCounter, "iil", CMD_REPORT_COUNTER)  // (cmd, counter type, buffer address)
NVNS(nvnCommandBufferReportValue, "iil") { (void)c; (void)na_; NVN_NOOP(); }
NVN(nvnCommandBufferResetCounter) {
    static uint32_t seen;
    uint32_t ty = (uint32_t)A(1);
    if (ty < 32 && !(seen & (1u << ty))) { seen |= 1u << ty; LOG("[nvn] ResetCounter type %u\n", ty); }
}
NVN(nvnCommandBufferCallCommands) { record_copy(c, na_, CMD_CALL, 2, (uint32_t)A(1) * 8); }
NVN(nvnCommandBufferCopyCommands) { record_copy(c, na_, CMD_CALL, 2, (uint32_t)A(1) * 8); }
NVN(nvnCommandBufferInsertDebugMarker) { (void)c; NVN_NOOP(); }
NVN(nvnCommandBufferInsertDebugMarkerDynamic) { (void)c; NVN_NOOP(); }
NVN(nvnCommandBufferInsertDebugMarkerStatic) { (void)c; NVN_NOOP(); }
NVN(nvnCommandBufferPushDebugGroup) { (void)c; NVN_NOOP(); }
NVN(nvnCommandBufferPushDebugGroupDynamic) { (void)c; NVN_NOOP(); }
NVN(nvnCommandBufferPushDebugGroupStatic) { (void)c; NVN_NOOP(); }
NVN(nvnCommandBufferPopDebugGroup) { (void)c; NVN_NOOP(); }
NVN(nvnCommandBufferPopDebugGroupId) { (void)c; NVN_NOOP(); }
