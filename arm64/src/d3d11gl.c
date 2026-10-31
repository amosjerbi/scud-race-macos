/* Direct3D 11 for the translated game, on OpenGL ES 3 (Linux, RG DS) or desktop OpenGL 4.1 (macOS).
 *
 * The game's D3D11 renderer (r3d_d3d11_*) is used as is: every COM object it gets back is a struct whose first field
 * points at a table of import tokens, so its vtable calls land in the functions below. Only the subset of the API the
 * game uses is implemented; any other method stops the game with its name.
 *
 * Shaders: the game compiles its HLSL at run time through D3DCompile (LoadLibraryA + GetProcAddress). Our D3DCompile
 * returns a blob naming the entry point, and CreateVertexShader / CreatePixelShader map that name to a hand-ported GLSL
 * version of the game's shader (see k_shaders below). Entries without a port fail to compile, and the game skips them.
 *
 * Conventions: D3D's clip space has y up and z in [0, 1], window row 0 at the top. Everything is drawn into textures
 * (the swap chain's back buffer too), with y negated in the vertex shaders, so texture row 0 is the D3D top row and
 * viewports, scissors, texel fetches and SV_Position need no conversion. Present blits the back buffer flipped. Depth
 * uses glClipControl(ZERO_TO_ONE) when the driver has it, otherwise the fragment shader writes D3D's z/w itself. */
#include "common.h"
#include <SDL.h>
#ifdef __APPLE__
#define GL_SILENCE_DEPRECATION
#include <OpenGL/gl3.h>
#define GLSL_HEAD "#version 410 core\n"
#else
#include <GLES3/gl3.h>
#define GLSL_HEAD "#version 300 es\nprecision highp float;\nprecision highp int;\nprecision highp usampler2D;\nprecision highp sampler2D;\n"
#endif
#include <stdarg.h>
#include <unistd.h>

uint64_t rt_import_token(const char *name, void *fn, const char *sig);

typedef uint64_t u64;
typedef uint32_t u32;
typedef int32_t HRESULT;
#define S_OK 0
#define S_FALSE 1
#define E_FAIL ((HRESULT)0x80004005)
#define E_NOINTERFACE ((HRESULT)0x80004002)
#define E_INVALIDARG ((HRESULT)0x80070057)

static int g_dbg = -1;
static void dlog(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); fprintf(stderr, "[d3dgl] "); vfprintf(stderr, fmt, ap); fputc('\n', stderr); va_end(ap);
}
#define DBG(...) do { if (g_dbg > 0) dlog(__VA_ARGS__); } while (0)

/* ------------------------------------------------------------------ objects */
enum kind { K_DEVICE = 1, K_CONTEXT, K_SWAP, K_BUF, K_TEX, K_SRV, K_RTV, K_DSV, K_VS, K_PS, K_LAYOUT, K_BLEND, K_DSS, K_RS, K_SAMP, K_QUERY, K_BLOB };

struct obj { const u64 *vt; int refs; int kind; };

struct buf {
    struct obj o; u32 size, usage, bind, cpu; GLuint id; GLenum target; uint8_t *shadow; u32 gen;   /* constant buffers: memory only */
};
struct tex {
    struct obj o; u32 w, h, fmt, usage, bind, cpu; GLuint id; GLenum ifmt, pfmt, ptype; int bpp, swap_rb, depth;
    uint8_t *shadow;                 /* staging textures live in memory only */
};
struct view { struct obj o; struct obj *res; };
struct shader { struct obj o; int entry; GLuint sh; };
struct elem { int slot, off, inst, n, integer; GLenum type; GLboolean norm; };
struct layout { struct obj o; int n; struct elem e[16]; };
struct blend { struct obj o; int en; GLenum src, dst, op, srca, dsta, opa; int mask; };
struct dss { struct obj o; int en, write; GLenum func; };
struct rs { struct obj o; int cull; int ccw; int scissor; int clip; };
struct samp { struct obj o; GLuint id; };
struct query { struct obj o; u32 type; u64 ts; };
struct blob { struct obj o; u32 size; uint8_t data[64]; };

static void *mk(size_t sz, int kind, const u64 *vt) { struct obj *o = calloc(1, sz); o->vt = vt; o->refs = 1; o->kind = kind; return o; }

/* ------------------------------------------------------------------ GL state the device owns */
static SDL_Window *g_win;
static SDL_GLContext g_gl;
static int g_clipz01;                  /* glClipControl(…, ZERO_TO_ONE) is on */
static int g_gles;
static int g_texq = 2;                 /* texture filtering in the 3D shader: 0 point, 1 bilinear, 2 trilinear (the game's) */
static float g_scale = 1.0f;           /* back buffer size / window size */
static void (*p_glClipControl)(GLenum, GLenum);
static pthread_t g_owner; static int g_owned;
static _Thread_local int t_current;

static struct obj g_device_o, g_ctx_o, g_swap_o;
static struct tex *g_backbuf;
static int g_swap_interval = -1;

/* the window is created before the device: misc.c hands it over */
void d3dgl_note_window(SDL_Window *w) { g_win = w; }

/* the GL context follows the thread that uses the device: the game sets it up on its own thread, then draws from its
 * render thread. CreateThread releases it (d3dgl_thread_handoff) and the next caller takes it. */
static void cur(void) {
    if (t_current) return;
    if (g_owned && !pthread_equal(g_owner, pthread_self())) {
        dlog("FATAL: the device is used from two threads at once; set M3_RENDER_THREAD=0"); fflush(stderr); _exit(71);
    }
    if (SDL_GL_MakeCurrent(g_win, g_gl)) dlog("SDL_GL_MakeCurrent: %s", SDL_GetError());
    g_owner = pthread_self(); g_owned = 1; t_current = 1;
}
void d3dgl_thread_handoff(void) {
    if (!t_current) return;
    glFlush(); SDL_GL_MakeCurrent(g_win, NULL); t_current = 0; g_owned = 0;
}

/* ------------------------------------------------------------------ formats */
static int fmt_info(u32 f, struct tex *t) {
    t->swap_rb = 0; t->depth = 0;
    switch (f) {
    case 0x57: case 0x5b: t->swap_rb = 1; /* fallthrough: BGRA is kept as RGBA, swapped on upload */
    case 0x1c: case 0x1d: t->ifmt = GL_RGBA8; t->pfmt = GL_RGBA; t->ptype = GL_UNSIGNED_BYTE; t->bpp = 4; return 1;
    case 0x39: t->ifmt = GL_R16UI; t->pfmt = GL_RED_INTEGER; t->ptype = GL_UNSIGNED_SHORT; t->bpp = 2; return 1;
    case 0x2a: t->ifmt = GL_R32UI; t->pfmt = GL_RED_INTEGER; t->ptype = GL_UNSIGNED_INT; t->bpp = 4; return 1;
    case 0x29: t->ifmt = GL_R32F; t->pfmt = GL_RED; t->ptype = GL_FLOAT; t->bpp = 4; return 1;
    case 0x3d: t->ifmt = GL_R8; t->pfmt = GL_RED; t->ptype = GL_UNSIGNED_BYTE; t->bpp = 1; return 1;
    case 0x02: t->ifmt = GL_RGBA32F; t->pfmt = GL_RGBA; t->ptype = GL_FLOAT; t->bpp = 16; return 1;
    case 0x0a: t->ifmt = GL_RGBA16F; t->pfmt = GL_RGBA; t->ptype = GL_HALF_FLOAT; t->bpp = 8; return 1;
    case 0x28: t->ifmt = GL_DEPTH_COMPONENT32F; t->pfmt = GL_DEPTH_COMPONENT; t->ptype = GL_FLOAT; t->bpp = 4; t->depth = 1; return 1;
    case 0x2d: t->ifmt = GL_DEPTH24_STENCIL8; t->pfmt = GL_DEPTH_STENCIL; t->ptype = GL_UNSIGNED_INT_24_8; t->bpp = 4; t->depth = 2; return 1;
    case 0x37: t->ifmt = GL_DEPTH_COMPONENT16; t->pfmt = GL_DEPTH_COMPONENT; t->ptype = GL_UNSIGNED_SHORT; t->bpp = 2; t->depth = 1; return 1;
    }
    return 0;
}

static GLuint g_unit_tex[16], g_unit_samp[16]; static int g_active_unit;
/* bind for uploads on a spare unit, so the draw-time binding cache stays right */
static void tex_bind_any(GLuint id) { if (g_active_unit != 15) { glActiveTexture(GL_TEXTURE0 + 15); g_active_unit = 15; } glBindTexture(GL_TEXTURE_2D, id); g_unit_tex[15] = id; }
static uint8_t *g_tmp; static size_t g_tmp_cap;
static uint8_t *tmpbuf(size_t n) { if (n > g_tmp_cap) { free(g_tmp); g_tmp = malloc(n); g_tmp_cap = n; } return g_tmp; }

/* upload rows (pitch bytes apart) into the texture; BGRA data is swapped into RGBA on the way */
static void tex_upload(struct tex *t, int x, int y, int w, int h, const uint8_t *src, u32 pitch) {
    if (t->shadow && !t->id) {                           /* staging */
        for (int r = 0; r < h; r++) memcpy(t->shadow + ((size_t)(y + r) * t->w + x) * t->bpp, src + (size_t)r * pitch, (size_t)w * t->bpp);
        return;
    }
    tex_bind_any(t->id);
    if (t->swap_rb) {
        uint8_t *d = tmpbuf((size_t)w * h * 4);
        for (int r = 0; r < h; r++) {
            const uint32_t *s = (const uint32_t *)(src + (size_t)r * pitch); uint32_t *o = (uint32_t *)(d + (size_t)r * w * 4);
            for (int i = 0; i < w; i++) { uint32_t v = s[i]; o[i] = (v & 0xff00ff00u) | ((v >> 16) & 0xff) | ((v & 0xff) << 16); }
        }
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, t->pfmt, t->ptype, d);
    } else {
        glPixelStorei(GL_UNPACK_ROW_LENGTH, (GLint)(pitch / t->bpp));
        glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, t->pfmt, t->ptype, src);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    }
}

/* ------------------------------------------------------------------ framebuffers */
static struct { int blend, bsrc, bdst, bop, bsa, bda, boa, mask, depth, dwrite, dfunc, cull, ccw, scis; GLuint fb, prog; } C;   /* GL-side cache */
static int g_dirty_fb = 1;
struct fbo { GLuint id; GLuint col, dep; };
static struct fbo g_fbos[64]; static int g_nfbo;
static GLuint fbo_for(struct tex *c, struct tex *d) {
    GLuint ci = c ? c->id : 0, di = d ? d->id : 0;
    for (int i = 0; i < g_nfbo; i++) if (g_fbos[i].col == ci && g_fbos[i].dep == di) return g_fbos[i].id;
    if (g_nfbo == 64) { for (int i = 0; i < g_nfbo; i++) glDeleteFramebuffers(1, &g_fbos[i].id); g_nfbo = 0; }
    struct fbo *f = &g_fbos[g_nfbo++]; f->col = ci; f->dep = di;
    glGenFramebuffers(1, &f->id); glBindFramebuffer(GL_FRAMEBUFFER, f->id);
    if (c) glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, ci, 0);
    if (d) glFramebufferTexture2D(GL_FRAMEBUFFER, d->depth == 2 ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, di, 0);
    GLenum db = c ? GL_COLOR_ATTACHMENT0 : GL_NONE; glDrawBuffers(1, &db);
    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE) dlog("framebuffer incomplete (%#x) for color %u depth %u", st, ci, di);
    return f->id;
}
static void fbo_forget(GLuint tex) {
    for (int i = 0; i < g_nfbo; i++) if (g_fbos[i].col == tex || g_fbos[i].dep == tex) {
        if (g_fbos[i].id == C.fb) { C.fb = 0; g_dirty_fb = 1; }   /* deleting the bound framebuffer binds 0 */
        glDeleteFramebuffers(1, &g_fbos[i].id); g_fbos[i--] = g_fbos[--g_nfbo];
    }
}

/* ------------------------------------------------------------------ shaders */
/* texture bindings of one shader: GLSL sampler name, stage slot (VS slots use units 8+), sampler slot or -1 (Load only) */
struct tuse { const char *name; int slot, samp; };
struct shdef { const char *entry; int ps; int depth3d; const char *src; struct tuse tex[4]; };

/* the game's cbuffer B (int4 texinfo; int fmt, flags; int2 pad; float4 fogc … gmisc) as two uniform arrays, uploaded from the
 * buffer's memory copy: much cheaper per draw on a mobile driver than re-filling a uniform buffer */
#define CB_DECL "uniform ivec4 Ui[2]; uniform vec4 Uf[10];\n#define texinfo Ui[0]\n#define fogc Uf[0]\n#define spe Uf[1]\n#define spr Uf[2]\n#define spc Uf[3]\n#define spf Uf[4]\n#define fgr Uf[5]\n#define gproj Uf[6]\n#define gsun Uf[7]\n#define gamb Uf[8]\n#define gmisc Uf[9]\n"

static const char vs3d_src[] =
    CB_DECL
    "layout(location=0) in vec4 a_pos; layout(location=1) in vec2 a_uv; layout(location=2) in vec4 a_col;\n"
    "layout(location=3) in vec2 a_fs; layout(location=4) in vec4 a_lc;\n"
    "out vec2 v_uv; out vec4 v_col; out vec2 v_fs; out vec4 v_lc;\n"
    "void main() { v_uv = a_uv; v_col = a_col; v_fs = a_fs; v_lc = a_lc; gl_Position = outpos(a_pos); }\n";

static const char ps_core[] =
    CB_DECL
    "uniform usampler2D sheet; uniform sampler2D atlas;\n"
    "layout(location=0) out vec4 o_col;\n"
    "const vec2 atl_inv = vec2(1.0 / 8192.0, 1.0 / 4096.0);\n"
    "vec4 atl(int lv, vec2 m0, ivec4 ta, int w, int h) {\n"
    "  vec2 o = vec2(ta.xy);\n"
    "  if (lv > 0) o += vec2(float(2 * w - 4 * w / (1 << lv) + 4 * (lv - 1)), float(2 * h + 4));\n"
    "  return textureLod(atlas, (o + 2.0 + 2.0 * m0 / float(1 << lv)) * atl_inv, 0.0);\n"
    "}\n"
    "const uint mipx[11] = uint[11](0u, 1024u, 1536u, 1792u, 1920u, 1984u, 2016u, 2032u, 2040u, 2044u, 2046u);\n"
    "const uint mipy[11] = uint[11](0u, 512u, 768u, 896u, 960u, 992u, 1008u, 1016u, 1020u, 1022u, 1023u);\n"
    "vec4 texel(uint v, int fmt) {\n"
    "  vec4 c = vec4(0.0); uint r;\n"
    "  if (fmt >= 8) { r = (v >> uint(4 * (fmt - 8))) & 15u; c.rgb = vec3(float(r) / 15.0); c.a = (r == 15u) ? 0.0 : 1.0; return c; }\n"
    "  if (fmt == 0) { c.r = float((v >> 10) & 31u) / 31.0; c.g = float((v >> 5) & 31u) / 31.0; c.b = float(v & 31u) / 31.0; c.a = ((v & 0x8000u) != 0u) ? 0.0 : 1.0; return c; }\n"
    "  if (fmt == 1) { c.rgb = vec3(float(v & 15u) / 15.0); c.a = float((v >> 4) & 15u) / 15.0; return c; }\n"
    "  if (fmt == 2) { c.a = float(v & 15u) / 15.0; c.rgb = vec3(float((v >> 4) & 15u) / 15.0); return c; }\n"
    "  if (fmt == 3) { c.rgb = vec3(float((v >> 8) & 15u) / 15.0); c.a = float((v >> 12) & 15u) / 15.0; return c; }\n"
    "  if (fmt == 4) { c.a = float((v >> 8) & 15u) / 15.0; c.rgb = vec3(float((v >> 12) & 15u) / 15.0); return c; }\n"
    "  if (fmt == 5) { r = v & 255u; c.rgb = vec3(float(r) / 255.0); c.a = (r == 255u) ? 0.0 : 1.0; return c; }\n"
    "  if (fmt == 6) { r = (v >> 8) & 255u; c.rgb = vec3(float(r) / 255.0); c.a = (r == 255u) ? 0.0 : 1.0; return c; }\n"
    "  c.r = float((v >> 12) & 15u) / 15.0; c.g = float((v >> 8) & 15u) / 15.0; c.b = float((v >> 4) & 15u) / 15.0; c.a = float(v & 15u) / 15.0; return c;\n"
    "}\n"
    "uint wrapc(int i, uint size, bool mir) { uint u = uint(i), m = size - 1u; if (mir && (u & size) != 0u) return m - (u & m); return u & m; }\n"
    "vec4 fetch(int lv, int ix, int iy, ivec4 ti, int fmt, int flags) {\n"
    "  uint w = max(uint(ti.z) >> uint(lv), 1u), h = max(uint(ti.w) >> uint(lv), 1u);\n"
    "  uint x = wrapc(ix, w, (flags & 2) != 0), y = wrapc(iy, h, (flags & 4) != 0);\n"
    "  uint bx = mipx[lv] + (uint(ti.x) & 2047u) / (1u << uint(lv));\n"
    "  uint by = mipy[lv] + (uint(ti.y) & 1023u) / (1u << uint(lv));\n"
    "  uint row = (uint(ti.y) & 3072u) | ((by + y) & 1023u);\n"
    "  uint col = (bx + x) & 2047u;\n"
    "  return texel(texelFetch(sheet, ivec2(int(col), int(row)), 0).r, fmt);\n"
    "}\n"
    "vec4 bilin(int lv, vec2 uv, ivec4 ti, int fmt, int flags) {\n"
    "  vec2 f = uv / float(1 << lv) - 0.5; vec2 b = floor(f); vec2 a = f - b; ivec2 i = ivec2(b);\n"
    "  vec4 c00 = fetch(lv, i.x, i.y, ti, fmt, flags), c10 = fetch(lv, i.x + 1, i.y, ti, fmt, flags), c01 = fetch(lv, i.x, i.y + 1, ti, fmt, flags), c11 = fetch(lv, i.x + 1, i.y + 1, ti, fmt, flags);\n"
    "  if ((flags & 8) != 0) { vec4 m = c00; if (c10.a > m.a) m = c10; if (c01.a > m.a) m = c01; if (c11.a > m.a) m = c11;\n"
    "    if (c00.a < m.a) c00.rgb = m.rgb; if (c10.a < m.a) c10.rgb = m.rgb; if (c01.a < m.a) c01.rgb = m.rgb; if (c11.a < m.a) c11.rgb = m.rgb; }\n"
    "  return mix(mix(c00, c10, a.x), mix(c01, c11, a.x), a.y);\n"
    "}\n"
    "void spotl(vec4 p, out float le, out float lf) {\n"
    "  float vz = 1.0 / p.w;\n"
    "  vec2 q = (p.xy - spe.xy) / spe.zw; float e = max(0.0, 1.0 - dot(q, q));\n"
    "  float en = (spr.x <= vz) ? 1.0 : 0.0; float rg = 0.0;\n"
    "  if (spr.y != 0.0) { float ax = abs(spr.y); float d = min(spr.x + ax - vz, 0.0); float t = d / (1.0 + ax) - 1.0; rg = en / (t * t); }\n"
    "  le = rg * e; lf = en * e;\n"
    "}\n"
    "vec4 pscore(vec4 ipos, vec2 uv, vec4 col, vec2 fs, vec4 lc, ivec4 ti, int fmt, int flags, ivec4 ta) {\n"
    "  vec4 c = col; vec3 texrgb = vec3(1.0);\n"
    "  if ((flags & 1) != 0) {\n"
    "    vec2 dx = dFdx(uv), dy = dFdy(uv);\n"
    "    float lod = 0.5 * log2(max(dot(dx, dx), dot(dy, dy)) + 1e-12);\n"
    "    uint m = min(uint(ti.z), uint(ti.w)); int maxlv = 0;\n"
    "    while ((m >> uint(maxlv + 1)) >= 2u && maxlv < 10) maxlv++;\n"
    "    lod = clamp(lod, 0.0, float(maxlv));\n"
    "    int l0 = int(lod); float t = lod - float(l0);\n"
    "    vec4 tc;\n"
    "    if (ta.z != 0) {\n"
    "      vec2 wh = vec2(ti.zw), q = floor(uv / wh), m0 = uv - q * wh;\n"
    "      ivec2 qi = ivec2(q);\n"
    "      if ((flags & 2) != 0 && (qi.x & 1) != 0) m0.x = wh.x - m0.x;\n"
    "      if ((flags & 4) != 0 && (qi.y & 1) != 0) m0.y = wh.y - m0.y;\n"
    "      tc = atl(l0, m0, ta, ti.z, ti.w);\n"
    "#if TEXQ >= 2\n"
    "      if (t > 0.01 && l0 < maxlv) tc = mix(tc, atl(l0 + 1, m0, ta, ti.z, ti.w), t);\n"
    "#endif\n"
    "    } else {\n"
    "#if TEXQ == 0\n"            /* point sampling, nearest mip: one fetch */
    "      if (t > 0.5 && l0 < maxlv) l0++;\n"
    "      ivec2 pi = ivec2(floor(uv / float(1 << l0)));\n"
    "      tc = fetch(l0, pi.x, pi.y, ti, fmt, flags);\n"
    "#else\n"
    "      tc = bilin(l0, uv, ti, fmt, flags);\n"
    "#if TEXQ >= 2\n"
    "      if (t > 0.01 && l0 < maxlv) tc = mix(tc, bilin(l0 + 1, uv, ti, fmt, flags), t);\n"
    "#endif\n"
    "#endif\n"
    "    }\n"
    "    if ((flags & 8) != 0 && tc.a < 32.0 / 255.0) discard;\n"
    "    if ((flags & 40) == 0) tc.a = 1.0;\n"
    "    c *= tc; texrgb = tc.rgb;\n"
    "  }\n"
    "  if (c.a < 1.0 / 32.0) discard;\n"
    "  float le = 0.0, lf = 0.0;\n"
    "  if (spr.w > 0.0) {\n"
    "    spotl(ipos, le, lf);\n"
    "    if (lc.w >= 0.0) { vec3 L = lc.w + spc.rgb * le; if (spc.w > 0.0) L = min(L, vec3(1.0)); c.rgb = texrgb * lc.rgb * L; }\n"
    "  }\n"
    "  c.rgb = min(c.rgb + fs.y, vec3(1.0));\n"
    "  c.rgb = mix(c.rgb, fogc.rgb + spf.rgb * spr.z * fgr.rgb * lf, clamp(fs.x, 0.0, 1.0));\n"
    "  if ((flags & 16) == 0) c.a = 1.0;\n"
    "  return c;\n"
    "}\n"
    "vec4 svpos() { return vec4(gl_FragCoord.xy, gl_FragCoord.z, 1.0 / gl_FragCoord.w); }\n";

static const char ps3d_main[] =
    "in vec2 v_uv; in vec4 v_col; in vec2 v_fs; in vec4 v_lc;\n"
    "void main() { fragdepth(); o_col = pscore(svpos(), v_uv, v_col, v_fs, v_lc, Ui[0], Ui[1].x, Ui[1].y, ivec4(0)); }\n";

static const char vs3dg_src[] =
    CB_DECL
    "uniform usampler2D ctabtex; uniform usampler2D tctl;\n"
    "layout(location=0) in vec4 p0; layout(location=1) in vec4 n0; layout(location=2) in vec4 f0; layout(location=3) in vec4 s0;\n"
    "layout(location=4) in vec2 uv; layout(location=5) in uvec4 pk; layout(location=6) in vec4 r0; layout(location=7) in vec4 r1;\n"
    "layout(location=8) in vec4 r2; layout(location=9) in vec4 im; layout(location=10) in uvec4 iu;\n"
    "out vec2 v_uv; out vec4 v_col; out vec2 v_fs; out vec4 v_lc; flat out ivec4 v_ti; flat out ivec2 v_ff; flat out ivec4 v_ta;\n"
    "void main() {\n"
    "  vec3 pos = p0.xyz; float shade = p0.w; vec3 nm = n0.xyz; float nvf = n0.w; vec3 fn = f0.xyz; float sgn = f0.w;\n"
    "  vec3 S = s0.xyz; float pa = s0.w; vec3 R0 = r0.xyz, R1 = r1.xyz, R2 = r2.xyz; vec3 T = vec3(r0.w, r1.w, r2.w);\n"
    "  float ia = im.x, invs = im.y; uint f = pk.x, t = pk.y;\n"
    "  float alpha = pa * ia;\n"
    "  vec3 vv = vec3(dot(R0, pos), dot(R1, pos), dot(R2, pos)) + T;\n"
    "  vec3 fvn = sgn * vec3(dot(R0, fn), dot(R1, fn), dot(R2, fn));\n"
    "  vec3 sv = vec3(dot(R0, S), dot(R1, S), dot(R2, S)) + nvf * T;\n"
    "  v_uv = uv; v_ti = ivec4(0); v_ff = ivec2(0); v_ta = ivec4(0); v_lc = vec4(0.0, 0.0, 0.0, -1.0); v_fs = vec2(0.0); v_col = vec4(0.0);\n"
    "  if (alpha <= 0.0 || dot(sv, fvn) > 0.0) { gl_Position = outpos(vec4(0.0, 0.0, 0.0, 1.0)); return; }\n"
    "  vec4 P = vec4(gproj.x * vv.x + gproj.y * vv.z, gproj.z * vv.y + gproj.w * vv.z, 0.001, -vv.z);\n"
    "  uint c = ((f & 1024u) != 0u) ? texelFetch(ctabtex, ivec2(int(pk.z), int(iu.x)), 0).r : pk.w;\n"
    "  vec3 col = vec3(float((c >> 16) & 255u), float((c >> 8) & 255u), float(c & 255u)) / 255.0;\n"
    "  vec3 vn = sgn * vec3(dot(R0, nm), dot(R1, nm), dot(R2, nm)) * invs;\n"
    "  float a = alpha, spec = 0.0; vec3 rgb = col;\n"
    "  bool smooth_ = (f & 4u) != 0u, fixed_ = (f & 2u) != 0u;\n"
    "  if ((f & 1u) != 0u) {\n"
    "    float sun = fixed_ ? shade : dot(gsun.xyz, vn);\n"
    "    sun = clamp(sun, -1.0, 1.0);\n"
    "    if (gamb.y > 0.0 || a < 0.999) sun = max(sun, 0.0);\n"
    "    float I = sun * gsun.w + gamb.x;\n"
    "    if (gmisc.y > 0.0) v_lc = vec4(col, I);\n"
    "    if (gmisc.x < 17.0 && I > 1.0) I = 1.0;\n"
    "    rgb = col * I;\n"
    "    if ((f & 8u) != 0u) {\n"
    "      uint sh = (f >> 12) & 3u;\n"
    "      float kexp = sh == 0u ? 8.0 : sh == 1u ? 16.0 : sh == 2u ? 32.0 : 64.0;\n"
    "      float kmul = sh < 2u ? 1.6 : sh == 2u ? 2.4 : 3.2;\n"
    "      float svv = float((f >> 19) & 63u) / 63.0;\n"
    "      if (smooth_) { float nl = max(sun, 0.0); spec = (nl > 0.0 ? pow(nl, kexp) : 0.0) * kmul; }\n"
    "      else { float d = dot(gsun.xyz, vn); float rz = 2.0 * d * vn.z - gsun.z; spec = max(rz, 0.0); }\n"
    "      spec *= svv * gsun.w;\n"
    "      if (a < 1.0 && spec > a) a = spec;\n"
    "    }\n"
    "  } else if (gmisc.x > 20.0) {\n"
    "    float k = fixed_ ? 1.0 + shade + gamb.x : 1.5;\n"
    "    rgb = col * k;\n"
    "  }\n"
    "  v_col = vec4(rgb, a);\n"
    "  float fog = clamp(gamb.w + (-vv.z) * gamb.z, 0.0, 1.0);\n"
    "  v_fs = vec2(fog * float((f >> 14) & 31u) / 16.0, spec);\n"
    "  int fl = 0;\n"
    "  if ((f & 16u) != 0u) {\n"
    "    uint tx = ((t & 2047u) + iu.y) & 2047u, ty = (((t >> 11) & 1023u) + iu.z) & 1023u;\n"
    "    uint page = ((iu.w & 2u) != 0u) ? 2u : (((t >> 21) & 1u) ^ (iu.w & 1u));\n"
    "    v_ti = ivec4(int(tx), int(ty + page * 1024u), 32 << int((t >> 26) & 7u), 32 << int((t >> 29) & 7u));\n"
    "    fl = 1 | (int((f >> 5) & 1u) << 1) | (int((f >> 6) & 1u) << 2) | (int((f >> 7) & 1u) << 3) | (int((f >> 8) & 1u) << 5);\n"
    "    v_ff.x = int((t >> 22) & 15u);\n"
    "    if (gmisc.z > 0.5 && page < 2u && ((tx & 31u) | (ty & 31u)) == 0u && v_ff.x < 12) {\n"
    "      uint tcol = (tx >> 5) + ((ty + page * 1024u) >> 5) * 64u, trow = uint(v_ff.x) * 8u + ((f >> 5) & 1u) * 4u + ((f >> 6) & 1u) * 2u + ((f >> 7) & 1u);\n"
    "      uint e = texelFetch(tctl, ivec2(int(tcol), int(trow)), 0).r;\n"
    "      if ((e >> 31) != 0u && ((e >> 25) & 7u) == ((t >> 26) & 7u) && ((e >> 28) & 7u) == ((t >> 29) & 7u)) v_ta = ivec4(int(e & 8191u), int((e >> 13) & 4095u), 1, 0);\n"
    "    }\n"
    "  }\n"
    "  if ((f & 512u) != 0u || alpha < 0.999) fl |= 16;\n"
    "  v_ff.y = fl;\n"
    "  gl_Position = outpos(P);\n"
    "}\n";

static const char ps3dg_main[] =
    "in vec2 v_uv; in vec4 v_col; in vec2 v_fs; in vec4 v_lc; flat in ivec4 v_ti; flat in ivec2 v_ff; flat in ivec4 v_ta;\n"
    "void main() { fragdepth(); o_col = pscore(svpos(), v_uv, v_col, v_fs, v_lc, v_ti, v_ff.x, v_ff.y, v_ta); }\n";

static const char vs2d_src[] =
    "out vec2 v_uv;\n"
    "void main() { int id = gl_VertexID; vec2 t = vec2(float((id << 1) & 2), float(id & 2)); v_uv = t; gl_Position = outpos(vec4(t * vec2(2.0, -2.0) + vec2(-1.0, 1.0), 0.0, 1.0)); }\n";
static const char vs2dc_src[] =
    "out vec2 v_uv; out vec4 v_col;\n"
    "void main() { int id = gl_VertexID; vec2 t = vec2(float((id << 1) & 2), float(id & 2)); v_uv = t; v_col = vec4(1.0); gl_Position = outpos(vec4(t * vec2(2.0, -2.0) + vec2(-1.0, 1.0), 0.0, 1.0)); }\n";
static const char ps2d_src[] =
    "uniform sampler2D layer; in vec2 v_uv; layout(location=0) out vec4 o_col;\n"
    "void main() { o_col = texture(layer, v_uv); }\n";

static const struct shdef k_shaders[] = {
    { "vs3d", 0, 1, vs3d_src, { { 0 } } },
    { "ps3d", 1, 1, NULL /* ps_core + ps3d_main */, { { "sheet", 0, -1 }, { "atlas", 4, 1 } } },
    { "vs3dg", 0, 1, vs3dg_src, { { "ctabtex", 2, -1 }, { "tctl", 3, -1 } } },
    { "ps3dg", 1, 1, NULL /* ps_core + ps3dg_main */, { { "sheet", 0, -1 }, { "atlas", 4, 1 } } },
    { "vs2d", 0, 0, vs2d_src, { { 0 } } },
    { "vs2dc", 0, 0, vs2dc_src, { { 0 } } },
    { "ps2d", 1, 0, ps2d_src, { { "layer", 1, 0 } } },
};
#define NSHADERS ((int)(sizeof k_shaders / sizeof k_shaders[0]))

static GLuint compile_glsl(int entry) {
    const struct shdef *d = &k_shaders[entry];
    const char *parts[6]; int n = 0;
    parts[n++] = GLSL_HEAD;
    char defs[256];
    snprintf(defs, sizeof defs, "#define CLIPZ01 %d\n#define FRAGDEPTH %d\n#define TEXQ %d\n", g_clipz01, d->depth3d && !g_clipz01, g_texq);
    parts[n++] = defs;
    if (!d->ps) {
        parts[n++] = d->depth3d && !g_clipz01 ? "out float v_dz;\nvec4 outpos(vec4 p) { v_dz = p.z; return vec4(p.x, -p.y, 2.0 * p.z - p.w, p.w); }\n"
                   : g_clipz01 ? "vec4 outpos(vec4 p) { return vec4(p.x, -p.y, p.z, p.w); }\n"
                   : "vec4 outpos(vec4 p) { return vec4(p.x, -p.y, 2.0 * p.z - p.w, p.w); }\n";
        parts[n++] = d->src;
    } else {
        if (d->depth3d) parts[n++] = g_clipz01 ? "void fragdepth() {}\n" : "in float v_dz;\nvoid fragdepth() { gl_FragDepth = v_dz * gl_FragCoord.w; }\n";
        if (d->src) parts[n++] = d->src;
        else { parts[n++] = ps_core; parts[n++] = !strcmp(d->entry, "ps3d") ? ps3d_main : ps3dg_main; }
    }
    GLuint s = glCreateShader(d->ps ? GL_FRAGMENT_SHADER : GL_VERTEX_SHADER);
    glShaderSource(s, n, parts, NULL); glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[4096]; glGetShaderInfoLog(s, sizeof log, NULL, log); dlog("GLSL %s did not compile:\n%s", d->entry, log); glDeleteShader(s); return 0; }
    return s;
}

struct prog { GLuint vs, ps, id; int vse, pse; GLint loc_i, loc_f; const struct buf *cb; u32 cbgen; };
static struct prog g_progs[64]; static int g_nprog;
static struct prog *prog_for(struct shader *vs, struct shader *ps) {
    for (int i = 0; i < g_nprog; i++) if (g_progs[i].vs == vs->sh && g_progs[i].ps == (ps ? ps->sh : 0)) return &g_progs[i];
    if (g_nprog == 64) { dlog("too many shader programs"); return &g_progs[0]; }
    struct prog *p = &g_progs[g_nprog++];
    p->vs = vs->sh; p->ps = ps ? ps->sh : 0; p->vse = vs->entry; p->pse = ps ? ps->entry : -1;
    p->id = glCreateProgram(); glAttachShader(p->id, p->vs); if (p->ps) glAttachShader(p->id, p->ps);
    glLinkProgram(p->id);
    GLint ok = 0; glGetProgramiv(p->id, GL_LINK_STATUS, &ok);
    if (!ok) { char log[4096]; glGetProgramInfoLog(p->id, sizeof log, NULL, log); dlog("program %s+%s did not link:\n%s", k_shaders[p->vse].entry, p->pse >= 0 ? k_shaders[p->pse].entry : "-", log); }
    p->loc_i = glGetUniformLocation(p->id, "Ui"); p->loc_f = glGetUniformLocation(p->id, "Uf");
    glUseProgram(p->id); C.prog = p->id;
    for (int st = 0; st < 2; st++) {
        int e = st ? p->pse : p->vse; if (e < 0) continue;
        for (int k = 0; k < 4 && k_shaders[e].tex[k].name; k++) {
            GLint loc = glGetUniformLocation(p->id, k_shaders[e].tex[k].name);
            if (loc >= 0) glUniform1i(loc, (st ? 0 : 8) + k_shaders[e].tex[k].slot);
        }
    }
    DBG("program %s+%s linked", k_shaders[p->vse].entry, p->pse >= 0 ? k_shaders[p->pse].entry : "-");
    return p;
}

/* ------------------------------------------------------------------ pipeline state set by the context */
static struct {
    struct shader *vs, *ps; struct layout *il;
    struct buf *vb[4]; u32 stride[4], voff[4]; struct buf *ib; GLenum itype; u32 ioff;
    struct buf *vcb, *pcb; struct view *vsrv[8], *psrv[8]; struct samp *ps_samp[4];
    struct view *rtv; struct view *dsv;
    struct blend *bs; struct dss *ds; struct rs *rs;
    float vp[6]; int sc[4]; GLenum topo;
} S;
static GLuint g_vao;

static GLenum g_blendf(u32 b) {
    static const GLenum m[] = { GL_ONE, GL_ZERO, GL_ONE, GL_SRC_COLOR, GL_ONE_MINUS_SRC_COLOR, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA,
        GL_ONE_MINUS_DST_ALPHA, GL_DST_COLOR, GL_ONE_MINUS_DST_COLOR, GL_SRC_ALPHA_SATURATE, GL_ONE, GL_ONE, GL_CONSTANT_COLOR, GL_ONE_MINUS_CONSTANT_COLOR };
    return b < 16 ? m[b] : GL_ONE;
}
static GLenum g_blendop(u32 o) { switch (o) { case 2: return GL_FUNC_SUBTRACT; case 3: return GL_FUNC_REVERSE_SUBTRACT; case 4: return GL_MIN; case 5: return GL_MAX; } return GL_FUNC_ADD; }
static GLenum g_cmp(u32 c) { static const GLenum m[] = { GL_ALWAYS, GL_NEVER, GL_LESS, GL_EQUAL, GL_LEQUAL, GL_GREATER, GL_NOTEQUAL, GL_GEQUAL, GL_ALWAYS }; return c < 9 ? m[c] : GL_ALWAYS; }
static void en(GLenum cap, int on) { if (on) glEnable(cap); else glDisable(cap); }

static void bind_fb(void) {
    if (!g_dirty_fb) return;
    struct tex *c = S.rtv ? (struct tex *)S.rtv->res : NULL, *d = S.dsv ? (struct tex *)S.dsv->res : NULL;
    GLuint f = (c || d) ? fbo_for(c, d) : 0;
    if (f != C.fb) { glBindFramebuffer(GL_FRAMEBUFFER, f); C.fb = f; }
    g_dirty_fb = 0;
}

static void apply_states(void) {
    struct blend *b = S.bs; int be = b ? b->en : 0;
    if (be != C.blend) { en(GL_BLEND, be); C.blend = be; }
    if (be && (b->src != (GLenum)C.bsrc || b->dst != (GLenum)C.bdst || b->srca != (GLenum)C.bsa || b->dsta != (GLenum)C.bda)) {
        glBlendFuncSeparate(b->src, b->dst, b->srca, b->dsta); C.bsrc = b->src; C.bdst = b->dst; C.bsa = b->srca; C.bda = b->dsta;
    }
    if (be && (b->op != (GLenum)C.bop || b->opa != (GLenum)C.boa)) { glBlendEquationSeparate(b->op, b->opa); C.bop = b->op; C.boa = b->opa; }
    int mask = b ? b->mask : 15;
    if (mask != C.mask) { glColorMask(mask & 1, (mask >> 1) & 1, (mask >> 2) & 1, (mask >> 3) & 1); C.mask = mask; }
    struct dss *d = S.ds; int de = d ? d->en : 1, dw = d ? d->write : 1; GLenum df = d ? d->func : GL_LESS;
    if (!S.dsv) de = 0;
    if (de != C.depth) { en(GL_DEPTH_TEST, de); C.depth = de; }
    if (de && dw != C.dwrite) { glDepthMask(dw ? GL_TRUE : GL_FALSE); C.dwrite = dw; }
    if (de && df != (GLenum)C.dfunc) { glDepthFunc(df); C.dfunc = df; }
    struct rs *r = S.rs; int cull = r ? r->cull : 3, ccw = r ? r->ccw : 0, sc = r ? r->scissor : 0;
    if (cull != C.cull) {
        en(GL_CULL_FACE, cull != 1); if (cull != 1) glCullFace(cull == 2 ? GL_FRONT : GL_BACK); C.cull = cull;
    }
    if (ccw != C.ccw) { glFrontFace(ccw ? GL_CW : GL_CCW); C.ccw = ccw; }   /* y is mirrored: D3D's clockwise is GL's counter-clockwise */
    if (sc != C.scis) { en(GL_SCISSOR_TEST, sc); C.scis = sc; }
}

static void apply_textures(struct prog *p) {
    for (int st = 0; st < 2; st++) {
        int e = st ? p->pse : p->vse; if (e < 0) continue;
        for (int k = 0; k < 4 && k_shaders[e].tex[k].name; k++) {
            const struct tuse *u = &k_shaders[e].tex[k];
            struct view *v = st ? S.psrv[u->slot] : S.vsrv[u->slot];
            int unit = (st ? 0 : 8) + u->slot;
            GLuint tid = v ? ((struct tex *)v->res)->id : 0, sid = (u->samp >= 0 && st && S.ps_samp[u->samp]) ? S.ps_samp[u->samp]->id : 0;
            if (tid != g_unit_tex[unit]) { if (g_active_unit != unit) { glActiveTexture(GL_TEXTURE0 + unit); g_active_unit = unit; } glBindTexture(GL_TEXTURE_2D, tid); g_unit_tex[unit] = tid; }
            if (sid != g_unit_samp[unit]) { glBindSampler(unit, sid); g_unit_samp[unit] = sid; }
        }
    }
}

static struct { const struct layout *il; GLuint vb[4]; u32 stride[4], off[4]; int basev, baseinst; } g_in;
/* single-instance draws: per-instance elements become constant attributes read from the buffer's memory copy, so the
 * vertex arrays (and the driver's vertex state) stay the same from one object to the next */
static int inst_const_ok(void) {
    if (!S.il) return 1;
    for (int i = 0; i < S.il->n; i++) if (S.il->e[i].inst && !(S.vb[S.il->e[i].slot] && S.vb[S.il->e[i].slot]->shadow)) return 0;
    return 1;
}
static void set_inst_consts(int inst) {
    for (int i = 0; S.il && i < S.il->n; i++) {
        const struct elem *e = &S.il->e[i]; if (!e->inst) continue;
        const struct buf *b = S.vb[e->slot];
        size_t at = S.voff[e->slot] + e->off + (size_t)inst * S.stride[e->slot];
        if (at + 16 > b->size) continue;
        const uint8_t *p = b->shadow + at;
        if (e->integer) { GLuint v[4] = { 0, 0, 0, 1 }; memcpy(v, p, 4 * (size_t)e->n); glVertexAttribI4uiv(i, v); }
        else if (e->type == GL_FLOAT) { GLfloat v[4] = { 0, 0, 0, 1 }; memcpy(v, p, 4 * (size_t)e->n); glVertexAttrib4fv(i, v); }
    }
}
static void apply_inputs(int basev, int baseinst) {
    static int nen; int n = 0;
    int same = g_in.il == S.il && g_in.basev == basev && g_in.baseinst == baseinst;
    for (int k = 0; k < 4 && same; k++) same = g_in.vb[k] == (S.vb[k] ? S.vb[k]->id : 0) && g_in.stride[k] == S.stride[k] && g_in.off[k] == S.voff[k];
    if (same) return;
    g_in.il = S.il; g_in.basev = basev; g_in.baseinst = baseinst;
    for (int k = 0; k < 4; k++) { g_in.vb[k] = S.vb[k] ? S.vb[k]->id : 0; g_in.stride[k] = S.stride[k]; g_in.off[k] = S.voff[k]; }
    if (S.il) for (int i = 0; i < S.il->n; i++) {
        const struct elem *e = &S.il->e[i]; struct buf *b = S.vb[e->slot];
        if (!b || (e->inst && baseinst < 0)) { glDisableVertexAttribArray(i); n = i + 1; continue; }   /* baseinst < 0: constants */
        glBindBuffer(GL_ARRAY_BUFFER, b->id);
        uintptr_t off = S.voff[e->slot] + e->off + (uintptr_t)(e->inst ? baseinst : basev) * S.stride[e->slot];
        glEnableVertexAttribArray(i);
        if (e->integer) glVertexAttribIPointer(i, e->n, e->type, (GLsizei)S.stride[e->slot], (const void *)off);
        else glVertexAttribPointer(i, e->n, e->type, e->norm, (GLsizei)S.stride[e->slot], (const void *)off);
        glVertexAttribDivisor(i, e->inst ? 1 : 0);
        n = i + 1;
    }
    for (int i = n; i < nen; i++) glDisableVertexAttribArray(i);
    nen = n;
}

static int prepare(int basev, int baseinst) {
    if (!S.vs) return 0;
    bind_fb();
    struct prog *p = prog_for(S.vs, S.ps);
    if (p->id != C.prog) { glUseProgram(p->id); C.prog = p->id; }
    apply_inputs(basev, baseinst);
    if (p->loc_i >= 0 || p->loc_f >= 0) {
        const struct buf *cb = S.pcb ? S.pcb : S.vcb;
        if (cb && cb->shadow && cb->size >= 192 && (cb != p->cb || cb->gen != p->cbgen)) {
            if (p->loc_i >= 0) glUniform4iv(p->loc_i, 2, (const GLint *)cb->shadow);
            if (p->loc_f >= 0) glUniform4fv(p->loc_f, 10, (const GLfloat *)(cb->shadow + 32));
            p->cb = cb; p->cbgen = cb->gen;
        }
    }
    apply_textures(p);
    apply_states();
    return 1;
}

static u64 g_draws;
/* SCUD_HOST_FPS: where the render thread's time goes, per category */
enum { T_TEX, T_BUF, T_DRAW, T_CLEAR, T_SWAP, T_N };
static u64 g_t[T_N]; static int g_timing;
static inline u64 tnow(void) { return g_timing ? clock_gettime_nsec_np(CLOCK_MONOTONIC) : 0; }
#define TB u64 _t0 = tnow()
#define TE(k) do { if (g_timing) g_t[k] += tnow() - _t0; } while (0)
static GLuint g_ib_bound;
static int g_nodraw, g_syncbuf;

/* ------------------------------------------------------------------ IUnknown */
static HRESULT QueryInterface(struct obj *o, const void *iid, void **out) {
    (void)iid; DBG("QueryInterface on kind %d", o->kind); if (out) *out = NULL; return E_NOINTERFACE;
}
static u32 AddRef(struct obj *o) { return (u32)++o->refs; }
static void destroy(struct obj *o) {
    if (!t_current && g_owned) {         /* the render thread holds the context (shutdown): leave the GL objects */
        if (o->kind == K_DEVICE || o->kind == K_CONTEXT || o->kind == K_SWAP) return;
        free(o); return;
    }
    cur();
    switch (o->kind) {
    case K_BUF: { struct buf *b = (void *)o; if (b->id) { glDeleteBuffers(1, &b->id); if (g_ib_bound == b->id) g_ib_bound = 0; memset(&g_in, 0, sizeof g_in); } free(b->shadow); break; }
    case K_TEX: { struct tex *t = (void *)o; if (t->id) { fbo_forget(t->id); for (int u = 0; u < 16; u++) if (g_unit_tex[u] == t->id) g_unit_tex[u] = 0; glDeleteTextures(1, &t->id); } free(t->shadow); break; }
    case K_SRV: case K_RTV: case K_DSV: { struct view *v = (void *)o; if (--v->res->refs == 0) destroy(v->res); break; }
    case K_VS: case K_PS: { struct shader *s = (void *)o; (void)s; break; }   /* GL shaders stay: programs reference them */
    case K_SAMP: { struct samp *s = (void *)o; glDeleteSamplers(1, &s->id); break; }
    case K_DEVICE: case K_CONTEXT: case K_SWAP: return;
    }
    free(o);
}
static u32 Release(struct obj *o) {
    int r = --o->refs;
    if (r == 0) destroy(o);
    return (u32)(r < 0 ? 0 : r);
}

/* ------------------------------------------------------------------ vtables */
struct meth { int idx; const char *name; void *fn; const char *sig; };
static u64 *mkvt(const char *iface, int size, const struct meth *m) {
    u64 *vt = calloc((size_t)size, 8);
    for (int i = 0; i < size; i++) {
        void *fn = NULL; const char *sig = NULL, *nm = NULL;
        if (i == 0) { fn = (void *)QueryInterface; sig = "w:qqq"; nm = "QueryInterface"; }
        else if (i == 1) { fn = (void *)AddRef; sig = "w:q"; nm = "AddRef"; }
        else if (i == 2) { fn = (void *)Release; sig = "w:q"; nm = "Release"; }
        for (const struct meth *p = m; p && p->name; p++) if (p->idx == i) { fn = p->fn; sig = p->sig; nm = p->name; }
        char *full = malloc(96);
        if (nm) snprintf(full, 96, "%s::%s", iface, nm); else snprintf(full, 96, "%s::method #%d (not implemented)", iface, i);
        vt[i] = rt_import_token(full, fn, sig);
    }
    return vt;
}
static u64 *vt_buf, *vt_tex, *vt_view, *vt_misc, *vt_blob, *vt_query;

/* ------------------------------------------------------------------ ID3D11Device */
static HRESULT CreateBuffer(struct obj *dev, const u32 *d, const u64 *init, struct buf **out) {
    (void)dev; cur();
    struct buf *b = mk(sizeof *b, K_BUF, vt_buf);
    b->size = d[0]; b->usage = d[1]; b->bind = d[2]; b->cpu = d[3];
    const void *data = init ? (const void *)(uintptr_t)init[0] : NULL;
    if (b->usage == 2 || b->usage == 3 || (b->bind & 6)) { b->shadow = calloc(1, b->size); if (data) memcpy(b->shadow, data, b->size); }   /* index buffers keep a memory copy too */
    if (!(b->usage == 3 || (b->bind & 4))) {                /* staging and constant buffers live in memory only */
        b->target = (b->bind & 4) ? GL_UNIFORM_BUFFER : (b->bind & 2) ? GL_ELEMENT_ARRAY_BUFFER : GL_ARRAY_BUFFER;
        GLenum tgt = b->target == GL_ELEMENT_ARRAY_BUFFER ? GL_COPY_WRITE_BUFFER : b->target;   /* the VAO keeps the index binding */
        glGenBuffers(1, &b->id); glBindBuffer(tgt, b->id);
        glBufferData(tgt, b->size, data, b->usage == 2 ? GL_STREAM_DRAW : GL_STATIC_DRAW);
        if (b->usage == 2 && !b->shadow) b->shadow = calloc(1, b->size);
    }
    DBG("CreateBuffer %u bytes usage %u bind %#x -> %p", b->size, b->usage, b->bind, (void *)b);
    if (out) *out = b; else Release(&b->o);
    return S_OK;
}
static HRESULT CreateTexture2D(struct obj *dev, const u32 *d, const u64 *init, struct tex **out) {
    (void)dev; cur();
    struct tex *t = mk(sizeof *t, K_TEX, vt_tex);
    t->w = d[0]; t->h = d[1]; t->fmt = d[4]; t->usage = d[7]; t->bind = d[8]; t->cpu = d[9];
    if (d[2] > 1 || d[3] > 1 || d[5] > 1) dlog("CreateTexture2D: %ux%u fmt %#x with %u mips, %u slices, %u samples: only the first is made", t->w, t->h, t->fmt, d[2], d[3], d[5]);
    if (!fmt_info(t->fmt, t)) { dlog("CreateTexture2D: format %#x is not supported", t->fmt); free(t); if (out) *out = NULL; return E_INVALIDARG; }
    const uint8_t *data = init ? (const uint8_t *)(uintptr_t)init[0] : NULL; u32 pitch = init ? ((const u32 *)init)[2] : 0;
    if (t->usage == 3) {
        t->shadow = calloc((size_t)t->w * t->h, t->bpp);
    } else {
        glGenTextures(1, &t->id); tex_bind_any(t->id);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        glTexImage2D(GL_TEXTURE_2D, 0, (GLint)t->ifmt, (GLsizei)t->w, (GLsizei)t->h, 0, t->pfmt, t->ptype, NULL);
        GLenum e = glGetError(); if (e) dlog("CreateTexture2D %ux%u fmt %#x: GL error %#x", t->w, t->h, t->fmt, e);
        if (t->usage == 2) t->shadow = NULL;
    }
    if (data) tex_upload(t, 0, 0, (int)t->w, (int)t->h, data, pitch ? pitch : t->w * t->bpp);
    DBG("CreateTexture2D %ux%u fmt %#x usage %u bind %#x -> %p", t->w, t->h, t->fmt, t->usage, t->bind, (void *)t);
    if (out) *out = t; else Release(&t->o);
    return S_OK;
}
static HRESULT mkview(int kind, struct obj *res, struct view **out) {
    if (!res) return E_INVALIDARG;
    struct view *v = mk(sizeof *v, kind, vt_view); v->res = res; res->refs++;
    if (out) *out = v; else Release(&v->o);
    return S_OK;
}
static HRESULT CreateSRV(struct obj *dev, struct obj *res, const void *d, struct view **out) { (void)dev; (void)d; return mkview(K_SRV, res, out); }
static HRESULT CreateRTV(struct obj *dev, struct obj *res, const void *d, struct view **out) { (void)dev; (void)d; return mkview(K_RTV, res, out); }
static HRESULT CreateDSV(struct obj *dev, struct obj *res, const void *d, struct view **out) { (void)dev; (void)d; return mkview(K_DSV, res, out); }

static HRESULT CreateInputLayout(struct obj *dev, const uint8_t *el, u32 n, const void *code, u64 len, struct layout **out) {
    (void)dev; (void)code; (void)len;
    struct layout *l = mk(sizeof *l, K_LAYOUT, vt_misc); l->n = (int)(n > 16 ? 16 : n);
    int run[8] = { 0 };
    for (int i = 0; i < l->n; i++) {
        const uint8_t *p = el + 32 * i; u32 fmt, slot, off, cls;
        memcpy(&fmt, p + 12, 4); memcpy(&slot, p + 16, 4); memcpy(&off, p + 20, 4); memcpy(&cls, p + 24, 4);
        struct elem *e = &l->e[i]; e->slot = (int)(slot & 3); e->inst = cls == 1; e->norm = GL_FALSE; e->integer = 0;
        switch (fmt) {
        case 0x02: e->n = 4; e->type = GL_FLOAT; break;
        case 0x06: e->n = 3; e->type = GL_FLOAT; break;
        case 0x10: e->n = 2; e->type = GL_FLOAT; break;
        case 0x29: e->n = 1; e->type = GL_FLOAT; break;
        case 0x03: e->n = 4; e->type = GL_UNSIGNED_INT; e->integer = 1; break;
        case 0x07: e->n = 3; e->type = GL_UNSIGNED_INT; e->integer = 1; break;
        case 0x11: e->n = 2; e->type = GL_UNSIGNED_INT; e->integer = 1; break;
        case 0x2a: e->n = 1; e->type = GL_UNSIGNED_INT; e->integer = 1; break;
        case 0x1c: e->n = 4; e->type = GL_UNSIGNED_BYTE; e->norm = GL_TRUE; break;
        case 0x1e: e->n = 4; e->type = GL_UNSIGNED_BYTE; e->integer = 1; break;
        default: dlog("input layout: element format %#x is not supported", fmt); e->n = 4; e->type = GL_FLOAT;
        }
        int bytes = e->n * (e->type == GL_UNSIGNED_BYTE ? 1 : 4);
        e->off = off == 0xffffffffu ? run[e->slot] : (int)off;
        run[e->slot] = e->off + bytes;
    }
    if (out) *out = l; else free(l);
    return S_OK;
}

static HRESULT mkshader(int kind, const void *code, u64 len, struct shader **out) {
    cur();
    if (!code || len < 8 || memcmp(code, "GLSH", 4)) { dlog("Create%sShader: not a blob made by our D3DCompile", kind == K_VS ? "Vertex" : "Pixel"); return E_INVALIDARG; }
    int entry; memcpy(&entry, (const uint8_t *)code + 4, 4);
    if (entry < 0 || entry >= NSHADERS || k_shaders[entry].ps != (kind == K_PS)) return E_INVALIDARG;
    GLuint sh = compile_glsl(entry);
    if (!sh) return E_FAIL;
    struct shader *s = mk(sizeof *s, kind, vt_misc); s->entry = entry; s->sh = sh;
    DBG("shader %s ready", k_shaders[entry].entry);
    if (out) *out = s; else free(s);
    return S_OK;
}
static HRESULT CreateVS(struct obj *dev, const void *code, u64 len, void *link, struct shader **out) { (void)dev; (void)link; return mkshader(K_VS, code, len, out); }
static HRESULT CreatePS(struct obj *dev, const void *code, u64 len, void *link, struct shader **out) { (void)dev; (void)link; return mkshader(K_PS, code, len, out); }

static HRESULT CreateBlendState(struct obj *dev, const u32 *d, struct blend **out) {
    (void)dev; struct blend *b = mk(sizeof *b, K_BLEND, vt_misc);
    const u32 *r = d + 2;                               /* RenderTarget[0] */
    b->en = r[0] != 0; b->src = g_blendf(r[1]); b->dst = g_blendf(r[2]); b->op = g_blendop(r[3]);
    b->srca = g_blendf(r[4]); b->dsta = g_blendf(r[5]); b->opa = g_blendop(r[6]); b->mask = ((const uint8_t *)d)[8 + 28] & 15;
    if (d[1]) DBG("blend state with independent blending: only target 0 is used");
    if (out) *out = b; else free(b);
    return S_OK;
}
static HRESULT CreateDepthStencilState(struct obj *dev, const u32 *d, struct dss **out) {
    (void)dev; struct dss *s = mk(sizeof *s, K_DSS, vt_misc);
    s->en = d[0] != 0; s->write = d[1] != 0; s->func = g_cmp(d[2]);
    if (d[3]) dlog("depth-stencil state with stencil: stencil is ignored");
    if (out) *out = s; else free(s);
    return S_OK;
}
static HRESULT CreateRasterizerState(struct obj *dev, const u32 *d, struct rs **out) {
    (void)dev; struct rs *r = mk(sizeof *r, K_RS, vt_misc);
    r->cull = (int)d[1]; r->ccw = d[2] != 0; r->clip = d[6] != 0; r->scissor = d[7] != 0;
    if (d[0] == 2) dlog("wireframe rasterizer state: drawn solid");
    if (out) *out = r; else free(r);
    return S_OK;
}
static GLenum g_addr(u32 a) { switch (a) { case 1: return GL_REPEAT; case 2: case 5: return GL_MIRRORED_REPEAT; } return GL_CLAMP_TO_EDGE; }
static HRESULT CreateSamplerState(struct obj *dev, const u32 *d, struct samp **out) {
    (void)dev; cur();
    struct samp *s = mk(sizeof *s, K_SAMP, vt_misc);
    glGenSamplers(1, &s->id);
    u32 f = d[0];
    glSamplerParameteri(s->id, GL_TEXTURE_MIN_FILTER, (f & 0x10) ? GL_LINEAR : GL_NEAREST);
    glSamplerParameteri(s->id, GL_TEXTURE_MAG_FILTER, (f & 0x04) ? GL_LINEAR : GL_NEAREST);
    glSamplerParameteri(s->id, GL_TEXTURE_WRAP_S, (GLint)g_addr(d[1]));
    glSamplerParameteri(s->id, GL_TEXTURE_WRAP_T, (GLint)g_addr(d[2]));
    if (out) *out = s; else Release(&s->o);
    return S_OK;
}
static HRESULT CreateQuery(struct obj *dev, const u32 *d, struct query **out) {
    (void)dev; struct query *q = mk(sizeof *q, K_QUERY, vt_query); q->type = d[0];
    if (out) *out = q; else free(q);
    return S_OK;
}
static u32 GetFeatureLevel(struct obj *dev) { (void)dev; return 0xb000; }
static void GetImmediateContext(struct obj *dev, struct obj **out) { (void)dev; g_ctx_o.refs++; *out = &g_ctx_o; }

/* ------------------------------------------------------------------ ID3D11DeviceContext */
static void VSSetConstantBuffers(struct obj *c, u32 start, u32 n, struct buf **b) { (void)c; for (u32 i = 0; i < n; i++) if (start + i == 0) S.vcb = b ? b[i] : NULL; }
static void PSSetConstantBuffers(struct obj *c, u32 start, u32 n, struct buf **b) { (void)c; for (u32 i = 0; i < n; i++) if (start + i == 0) S.pcb = b ? b[i] : NULL; }
static void PSSetShaderResources(struct obj *c, u32 start, u32 n, struct view **v) { (void)c; for (u32 i = 0; i < n && start + i < 8; i++) S.psrv[start + i] = v ? v[i] : NULL; }
static void VSSetShaderResources(struct obj *c, u32 start, u32 n, struct view **v) { (void)c; for (u32 i = 0; i < n && start + i < 8; i++) S.vsrv[start + i] = v ? v[i] : NULL; }
static void PSSetSamplers(struct obj *c, u32 start, u32 n, struct samp **s) { (void)c; for (u32 i = 0; i < n && start + i < 4; i++) S.ps_samp[start + i] = s ? s[i] : NULL; }
static void PSSetShader(struct obj *c, struct shader *s, void *ci, u32 n) { (void)c; (void)ci; (void)n; S.ps = s; }
static void VSSetShader(struct obj *c, struct shader *s, void *ci, u32 n) { (void)c; (void)ci; (void)n; S.vs = s; }
static void IASetInputLayout(struct obj *c, struct layout *l) { (void)c; S.il = l; }
static void IASetVertexBuffers(struct obj *c, u32 start, u32 n, struct buf **b, const u32 *str, const u32 *off) {
    (void)c; for (u32 i = 0; i < n && start + i < 4; i++) { S.vb[start + i] = b ? b[i] : NULL; S.stride[start + i] = str ? str[i] : 0; S.voff[start + i] = off ? off[i] : 0; }
}
static void IASetIndexBuffer(struct obj *c, struct buf *b, u32 fmt, u32 off) {
    (void)c; S.ib = b; S.itype = fmt == 0x39 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT; S.ioff = off;
}
static void IASetPrimitiveTopology(struct obj *c, u32 t) {
    (void)c; switch (t) { case 1: S.topo = GL_POINTS; break; case 2: S.topo = GL_LINES; break; case 3: S.topo = GL_LINE_STRIP; break; case 5: S.topo = GL_TRIANGLE_STRIP; break; default: S.topo = GL_TRIANGLES; }
}
static void Draw(struct obj *c, u32 count, u32 start) {
    (void)c; cur(); TB; if (!prepare(0, 0)) return;
    g_draws++; if (!g_nodraw) glDrawArrays(S.topo, (GLint)start, (GLsizei)count);
    TE(T_DRAW);
}
static void DrawInstanced(struct obj *c, u32 count, u32 ninst, u32 start, u32 sinst) {
    (void)c; cur(); if (!prepare(0, (int)sinst)) return;
    glDrawArraysInstanced(S.topo, (GLint)start, (GLsizei)count, (GLsizei)ninst); g_draws++;
}
static void (*p_DrawElementsBaseVertex)(GLenum, GLsizei, GLenum, const void *, GLint);
static void (*p_DrawRangeElementsBaseVertex)(GLenum, GLuint, GLuint, GLsizei, GLenum, const void *, GLint);
static void (*p_DrawElementsInstancedBaseVertexBaseInstance)(GLenum, GLsizei, GLenum, const void *, GLsizei, GLint, GLuint);
static void draw_idx_(u32 count, u32 ninst, u32 sidx, int32_t basev, u32 sinst);
static void draw_idx(u32 count, u32 ninst, u32 sidx, int32_t basev, u32 sinst) { TB; draw_idx_(count, ninst, sidx, basev, sinst); TE(T_DRAW); }
/* the index range of a draw, from the index buffer's memory copy: given to the driver with glDrawRangeElements, it need
 * not scan the indices itself (Panfrost does, in uncached memory, for every draw its small cache misses) */
static int idx_range(const struct buf *b, uintptr_t off, u32 count, GLenum type, GLuint *mn, GLuint *mx) {
    if (!b->shadow || off + (uintptr_t)count * (type == GL_UNSIGNED_SHORT ? 2 : 4) > b->size || !count) return 0;
    u32 lo = 0xffffffffu, hi = 0;
    if (type == GL_UNSIGNED_SHORT) { const uint16_t *p = (const uint16_t *)(b->shadow + off); for (u32 i = 0; i < count; i++) { u32 v = p[i]; lo = v < lo ? v : lo; hi = v > hi ? v : hi; } }
    else { const u32 *p = (const u32 *)(b->shadow + off); for (u32 i = 0; i < count; i++) { u32 v = p[i]; lo = v < lo ? v : lo; hi = v > hi ? v : hi; } }
    *mn = lo; *mx = hi; return 1;
}
static void draw_idx_(u32 count, u32 ninst, u32 sidx, int32_t basev, u32 sinst) {
    cur(); if (!S.ib) return;
    uintptr_t off = S.ioff + (uintptr_t)sidx * (S.itype == GL_UNSIGNED_SHORT ? 2 : 4);
    GLuint mn, mx;
    if (ninst == 1 && p_DrawRangeElementsBaseVertex && inst_const_ok() && idx_range(S.ib, off, count, S.itype, &mn, &mx)) {
        if (!prepare(0, -1)) return;
        set_inst_consts((int)sinst);
        if (S.ib->id != g_ib_bound) { glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, S.ib->id); g_ib_bound = S.ib->id; }
        g_draws++;
        if (!g_nodraw) p_DrawRangeElementsBaseVertex(S.topo, mn, mx, (GLsizei)count, S.itype, (const void *)off, basev);
        return;
    }
    /* base vertex / base instance go to the draw call when the driver can, so the vertex inputs stay set between draws */
    int bi = p_DrawElementsInstancedBaseVertexBaseInstance && (sinst || ninst != 1);
    int bv = !bi && p_DrawElementsBaseVertex && ninst == 1;
    if (!prepare(bi || bv ? 0 : basev, bi ? 0 : (int)sinst)) return;
    if (S.ib->id != g_ib_bound) { glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, S.ib->id); g_ib_bound = S.ib->id; }
    g_draws++;
    if (g_nodraw) return;
    if (bi) p_DrawElementsInstancedBaseVertexBaseInstance(S.topo, (GLsizei)count, S.itype, (const void *)off, (GLsizei)ninst, basev, sinst);
    else if (bv) p_DrawElementsBaseVertex(S.topo, (GLsizei)count, S.itype, (const void *)off, basev);
    else if (ninst == 1) glDrawElements(S.topo, (GLsizei)count, S.itype, (const void *)off);
    else glDrawElementsInstanced(S.topo, (GLsizei)count, S.itype, (const void *)off, (GLsizei)ninst);
}
static void DrawIndexed(struct obj *c, u32 count, u32 sidx, u32 basev) { (void)c; draw_idx(count, 1, sidx, (int32_t)basev, 0); }
static void DrawIndexedInstanced(struct obj *c, u32 count, u32 ninst, u32 sidx, u32 basev, u32 sinst) { (void)c; draw_idx(count, ninst, sidx, (int32_t)basev, sinst); }

static HRESULT Map(struct obj *c, struct obj *res, u32 sub, u32 type, u32 flags, u64 *out) {
    (void)c; (void)sub; (void)flags; (void)type;
    if (res->kind == K_BUF) {
        struct buf *b = (void *)res;
        if (!b->shadow) { dlog("Map of a buffer without CPU access"); return E_INVALIDARG; }
        out[0] = (u64)(uintptr_t)b->shadow; ((u32 *)out)[2] = b->size; ((u32 *)out)[3] = b->size;
        return S_OK;
    }
    if (res->kind == K_TEX) {
        struct tex *t = (void *)res;
        if (!t->shadow) { if (t->usage != 2) { dlog("Map of a texture without CPU access"); return E_INVALIDARG; } t->shadow = calloc((size_t)t->w * t->h, t->bpp); }
        out[0] = (u64)(uintptr_t)t->shadow; ((u32 *)out)[2] = t->w * t->bpp; ((u32 *)out)[3] = t->w * t->h * t->bpp;
        return S_OK;
    }
    return E_INVALIDARG;
}
static void Unmap_(struct obj *res);
static void Unmap(struct obj *c, struct obj *res, u32 sub) { (void)c; (void)sub; cur(); TB; Unmap_(res); TE(res->kind == K_BUF ? T_BUF : T_TEX); }
static void Unmap_(struct obj *res) {
    if (res->kind == K_BUF) {
        struct buf *b = (void *)res; b->gen++; if (!b->id) return;
        GLenum tgt = b->target == GL_ELEMENT_ARRAY_BUFFER ? GL_COPY_WRITE_BUFFER : b->target;
        glBindBuffer(tgt, b->id); glBufferData(tgt, b->size, b->shadow, GL_STREAM_DRAW);   /* orphan: no wait on draws in flight */
    } else if (res->kind == K_TEX) {
        struct tex *t = (void *)res;
        if (t->id) { uint8_t *s = t->shadow; t->shadow = NULL; tex_upload(t, 0, 0, (int)t->w, (int)t->h, s, t->w * t->bpp); t->shadow = s; }
    }
}

static void Begin(struct obj *c, struct query *q) { (void)c; (void)q; }
static void End(struct obj *c, struct query *q) { (void)c; if (q && q->type == 2) q->ts = clock_gettime_nsec_np(CLOCK_MONOTONIC); }
static HRESULT GetData(struct obj *c, struct query *q, void *data, u32 size, u32 flags) {
    (void)c; (void)flags; if (!q) return E_INVALIDARG;
    if (data && size) {
        memset(data, 0, size);
        if (q->type == 0 && size >= 4) *(u32 *)data = 1;                          /* event: done */
        else if (q->type == 2 && size >= 8) memcpy(data, &q->ts, 8);               /* timestamp, ns */
        else if (q->type == 3 && size >= 12) { u64 f = 1000000000ull; memcpy(data, &f, 8); ((u32 *)data)[2] = 1; }   /* disjoint: tell it not to trust them */
    }
    return S_OK;
}
static u32 QueryGetDataSize(struct query *q) { return q->type == 3 ? 16 : q->type == 2 ? 8 : 4; }

static void OMSetRenderTargets(struct obj *c, u32 n, struct view **rtv, struct view *dsv) {
    (void)c; S.rtv = (n && rtv) ? rtv[0] : NULL; S.dsv = dsv; g_dirty_fb = 1;
}
static void OMGetRenderTargets(struct obj *c, u32 n, struct view **rtv, struct view **dsv) {
    (void)c;
    if (rtv) for (u32 i = 0; i < n; i++) { rtv[i] = i == 0 ? S.rtv : NULL; if (rtv[i]) rtv[i]->o.refs++; }
    if (dsv) { *dsv = S.dsv; if (S.dsv) S.dsv->o.refs++; }
}
static void OMSetBlendState(struct obj *c, struct blend *b, const float *f, u32 mask) {
    (void)c; (void)mask; S.bs = b; if (f) { cur(); glBlendColor(f[0], f[1], f[2], f[3]); }
}
static void OMSetDepthStencilState(struct obj *c, struct dss *d, u32 ref) { (void)c; (void)ref; S.ds = d; }
static void RSSetState(struct obj *c, struct rs *r) { (void)c; S.rs = r; }
static void RSSetViewports(struct obj *c, u32 n, const float *v) {
    (void)c; if (!n || !v) return; cur(); memcpy(S.vp, v, sizeof S.vp);
    glViewport((GLint)v[0], (GLint)v[1], (GLsizei)v[2], (GLsizei)v[3]); glDepthRangef(v[4], v[5]);
}
static void RSSetScissorRects(struct obj *c, u32 n, const int32_t *r) {
    (void)c; if (!n || !r) return; cur(); glScissor(r[0], r[1], r[2] - r[0], r[3] - r[1]);
}

/* read a texture back into memory (rows of w*bpp bytes) */
static void tex_read(struct tex *t, uint8_t *dst) {
    if (t->depth) { dlog("read-back of a depth texture is not supported"); return; }
    GLuint f; glGenFramebuffers(1, &f); glBindFramebuffer(GL_READ_FRAMEBUFFER, f);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->id, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    if (t->pfmt == GL_RED_INTEGER) {                      /* only RGBA_INTEGER/UNSIGNED_INT is guaranteed for integer targets */
        u32 *tmp = (u32 *)tmpbuf((size_t)t->w * t->h * 16);
        glReadPixels(0, 0, (GLsizei)t->w, (GLsizei)t->h, GL_RGBA_INTEGER, GL_UNSIGNED_INT, tmp);
        for (size_t i = 0; i < (size_t)t->w * t->h; i++) { u32 v = tmp[4 * i]; if (t->bpp == 2) ((uint16_t *)dst)[i] = (uint16_t)v; else ((u32 *)dst)[i] = v; }
    } else {
        glReadPixels(0, 0, (GLsizei)t->w, (GLsizei)t->h, t->pfmt, t->ptype, dst);
        if (t->swap_rb) for (size_t i = 0; i < (size_t)t->w * t->h; i++) { u32 v = ((u32 *)dst)[i]; ((u32 *)dst)[i] = (v & 0xff00ff00u) | ((v >> 16) & 0xff) | ((v & 0xff) << 16); }
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, C.fb); glDeleteFramebuffers(1, &f);
}
static void copy_tex(struct tex *d, int dx, int dy, struct tex *s, int sx, int sy, int w, int h) {
    if (d->shadow && !d->id) {                            /* GPU -> staging */
        if (s->shadow && !s->id) { for (int r = 0; r < h; r++) memcpy(d->shadow + ((size_t)(dy + r) * d->w + dx) * d->bpp, s->shadow + ((size_t)(sy + r) * s->w + sx) * s->bpp, (size_t)w * d->bpp); return; }
        if (sx == 0 && sy == 0 && dx == 0 && dy == 0 && (u32)w == s->w && (u32)h == s->h && d->w == s->w) { tex_read(s, d->shadow); return; }
        uint8_t *all = malloc((size_t)s->w * s->h * s->bpp); tex_read(s, all);
        for (int r = 0; r < h; r++) memcpy(d->shadow + ((size_t)(dy + r) * d->w + dx) * d->bpp, all + ((size_t)(sy + r) * s->w + sx) * s->bpp, (size_t)w * d->bpp);
        free(all); return;
    }
    if (s->shadow && !s->id) { tex_upload(d, dx, dy, w, h, s->shadow + ((size_t)sy * s->w + sx) * s->bpp, s->w * s->bpp); return; }
    GLuint fr, fd; glGenFramebuffers(1, &fr); glGenFramebuffers(1, &fd);
    GLenum att = s->depth ? GL_DEPTH_ATTACHMENT : GL_COLOR_ATTACHMENT0;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fr); glFramebufferTexture2D(GL_READ_FRAMEBUFFER, att, GL_TEXTURE_2D, s->id, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fd); glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, att, GL_TEXTURE_2D, d->id, 0);
    if (C.scis) glDisable(GL_SCISSOR_TEST);
    glBlitFramebuffer(sx, sy, sx + w, sy + h, dx, dy, dx + w, dy + h, s->depth ? GL_DEPTH_BUFFER_BIT : GL_COLOR_BUFFER_BIT, GL_NEAREST);
    if (C.scis) glEnable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, C.fb); glDeleteFramebuffers(1, &fr); glDeleteFramebuffers(1, &fd);
}
static void copy_buf(struct buf *d, u32 doff, struct buf *s, u32 soff, u32 n) {
    if (d->id && s->id) {
        glBindBuffer(GL_COPY_READ_BUFFER, s->id); glBindBuffer(GL_COPY_WRITE_BUFFER, d->id);
        glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, soff, doff, n);
        if (d->shadow && s->shadow) memcpy(d->shadow + doff, s->shadow + soff, n);
        else if (d->shadow) dlog("buffer copy into a memory-backed buffer from one without: its copy is stale");
    } else if (!d->id && s->id) {                         /* -> staging */
        glBindBuffer(GL_COPY_READ_BUFFER, s->id);
        void *p = glMapBufferRange(GL_COPY_READ_BUFFER, soff, n, GL_MAP_READ_BIT);
        if (p) { memcpy(d->shadow + doff, p, n); glUnmapBuffer(GL_COPY_READ_BUFFER); }
    } else if (d->id) { glBindBuffer(GL_COPY_WRITE_BUFFER, d->id); glBufferSubData(GL_COPY_WRITE_BUFFER, doff, n, s->shadow + soff); }
    else memcpy(d->shadow + doff, s->shadow + soff, n);
}
static void CopySubresourceRegion(struct obj *c, struct obj *dst, u32 dsub, u32 x, u32 y, u32 z, struct obj *src, u32 ssub, const u32 *box) {
    (void)c; (void)dsub; (void)z; (void)ssub; cur();
    if (dst->kind == K_BUF && src->kind == K_BUF) {
        struct buf *s = (void *)src; u32 l = box ? box[0] : 0, r = box ? box[3] : s->size;
        copy_buf((void *)dst, x, s, l, r - l);
    } else if (dst->kind == K_TEX && src->kind == K_TEX) {
        struct tex *s = (void *)src; u32 l = box ? box[0] : 0, t = box ? box[1] : 0, r = box ? box[3] : s->w, b = box ? box[4] : s->h;
        copy_tex((void *)dst, (int)x, (int)y, s, (int)l, (int)t, (int)(r - l), (int)(b - t));
    }
}
static void CopyResource(struct obj *c, struct obj *dst, struct obj *src) {
    (void)c; cur();
    if (dst->kind == K_BUF) { struct buf *d = (void *)dst, *s = (void *)src; copy_buf(d, 0, s, 0, d->size < s->size ? d->size : s->size); }
    else { struct tex *d = (void *)dst, *s = (void *)src; copy_tex(d, 0, 0, s, 0, 0, (int)s->w, (int)s->h); }
}
static void UpdateSubresource_(struct obj *dst, const u32 *box, const uint8_t *data, u32 pitch);
static void UpdateSubresource(struct obj *c, struct obj *dst, u32 sub, const u32 *box, const uint8_t *data, u32 pitch, u32 dpitch) {
    (void)c; (void)sub; (void)dpitch; cur(); TB; UpdateSubresource_(dst, box, data, pitch); TE(dst->kind == K_BUF ? T_BUF : T_TEX);
}
static void UpdateSubresource_(struct obj *dst, const u32 *box, const uint8_t *data, u32 pitch) {
    if (dst->kind == K_BUF) {
        struct buf *b = (void *)dst; u32 l = box ? box[0] : 0, r = box ? box[3] : b->size; b->gen++;
        if (b->shadow) memcpy(b->shadow + l, data, r - l);
        if (b->id) {
            GLenum tgt = b->target == GL_ELEMENT_ARRAY_BUFFER ? GL_COPY_WRITE_BUFFER : b->target; glBindBuffer(tgt, b->id);
            /* the game's geometry arena only writes ranges no queued draw reads (D3D's no-overwrite pattern): writing them
             * unsynchronised spares the driver a wait for the GPU, or a copy of the whole buffer, on every update */
            void *m = g_syncbuf ? NULL : glMapBufferRange(tgt, l, r - l, GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT | GL_MAP_INVALIDATE_RANGE_BIT);
            if (m) { memcpy(m, data, r - l); glUnmapBuffer(tgt); }
            else glBufferSubData(tgt, l, r - l, data);
        }
    } else {
        struct tex *t = (void *)dst;
        int x = box ? (int)box[0] : 0, y = box ? (int)box[1] : 0, w = box ? (int)(box[3] - box[0]) : (int)t->w, h = box ? (int)(box[4] - box[1]) : (int)t->h;
        if (w > 0 && h > 0) tex_upload(t, x, y, w, h, data, pitch ? pitch : (u32)w * t->bpp);
    }
}
static void ClearRenderTargetView(struct obj *c, struct view *v, const float *col) {
    (void)c; cur(); if (!v) return;
    GLuint f = fbo_for((struct tex *)v->res, NULL); glBindFramebuffer(GL_FRAMEBUFFER, f);
    if (C.scis) glDisable(GL_SCISSOR_TEST);
    if (C.mask != 15) glColorMask(1, 1, 1, 1);
    glClearColor(col[0], col[1], col[2], col[3]); glClear(GL_COLOR_BUFFER_BIT);
    if (C.mask != 15) glColorMask(C.mask & 1, (C.mask >> 1) & 1, (C.mask >> 2) & 1, (C.mask >> 3) & 1);
    if (C.scis) glEnable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, C.fb);
}
static void ClearDepthStencilView_(struct view *v, u32 flags, float depth, uint8_t stencil);
static void ClearDepthStencilView(struct obj *c, struct view *v, u32 flags, float depth, uint8_t stencil) { (void)c; cur(); TB; ClearDepthStencilView_(v, flags, depth, stencil); TE(T_CLEAR); }
static void ClearDepthStencilView_(struct view *v, u32 flags, float depth, uint8_t stencil) {
    if (!v) return;
    struct tex *d = (void *)v->res;
    GLuint f = (S.dsv == v) ? (bind_fb(), C.fb) : fbo_for(NULL, d);
    if (f != C.fb) glBindFramebuffer(GL_FRAMEBUFFER, f);
    if (C.scis) glDisable(GL_SCISSOR_TEST);
    GLbitfield bits = 0;
    if (flags & 1) { bits |= GL_DEPTH_BUFFER_BIT; glDepthMask(GL_TRUE); glClearDepthf(depth); }
    if ((flags & 2) && d->depth == 2) { bits |= GL_STENCIL_BUFFER_BIT; glClearStencil(stencil); }
    glClear(bits);
    if ((flags & 1) && !C.dwrite) glDepthMask(GL_FALSE);
    if (C.scis) glEnable(GL_SCISSOR_TEST);
    if (f != C.fb) glBindFramebuffer(GL_FRAMEBUFFER, C.fb);
}
static void Flush(struct obj *c) { (void)c; cur(); glFlush(); }
static void ClearState(struct obj *c) { (void)c; memset(&S, 0, sizeof S); S.topo = GL_TRIANGLES; g_dirty_fb = 1; }

/* ------------------------------------------------------------------ resources and views */
static void TexGetDesc(struct tex *t, u32 *d) {
    memset(d, 0, 44); d[0] = t->w; d[1] = t->h; d[2] = 1; d[3] = 1; d[4] = t->fmt; d[5] = 1; d[7] = t->usage; d[8] = t->bind; d[9] = t->cpu;
}
static void BufGetDesc(struct buf *b, u32 *d) { memset(d, 0, 24); d[0] = b->size; d[1] = b->usage; d[2] = b->bind; d[3] = b->cpu; }
static void ResGetType(struct obj *o, u32 *t) { *t = o->kind == K_BUF ? 1 : 3; }
static void ViewGetResource(struct view *v, struct obj **out) { v->res->refs++; *out = v->res; }
static void ChildGetDevice(struct obj *o, struct obj **out) { (void)o; g_device_o.refs++; *out = &g_device_o; }
static void *BlobPtr(struct blob *b) { return b->data; }
static u64 BlobSize(struct blob *b) { return b->size; }

/* ------------------------------------------------------------------ IDXGISwapChain */
static u32 g_bb_w, g_bb_h, g_bb_fmt;
static void make_backbuf(void) {
    cur();
    if (g_backbuf) { g_backbuf->o.refs--; if (g_backbuf->o.refs <= 0) destroy(&g_backbuf->o); g_backbuf = NULL; }
    u32 d[11] = { g_bb_w, g_bb_h, 1, 1, g_bb_fmt, 1, 0, 0, 0x28, 0, 0 };
    CreateTexture2D(&g_device_o, d, NULL, &g_backbuf);
}
static u64 g_frames, g_fps_t0, g_fps_n; static int g_fps_on = -1;
static HRESULT Present(struct obj *sw, u32 sync, u32 flags) {
    (void)sw; (void)flags; cur();
    int iv = sync ? 1 : 0;
    if (iv != g_swap_interval) { if (SDL_GL_SetSwapInterval(iv)) DBG("swap interval %d: %s", iv, SDL_GetError()); g_swap_interval = iv; }
    int dw = 0, dh = 0; SDL_GL_GetDrawableSize(g_win, &dw, &dh);
    GLuint f = fbo_for(g_backbuf, NULL);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, f); glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    if (C.scis) glDisable(GL_SCISSOR_TEST);
    /* keep the picture's aspect: the game renders at the swap chain's size, the window may be bigger */
    int ow = dw, oh = dh;
    if (g_bb_w && g_bb_h) { if ((u64)dw * g_bb_h > (u64)dh * g_bb_w) ow = (int)((u64)dh * g_bb_w / g_bb_h); else oh = (int)((u64)dw * g_bb_h / g_bb_w); }
    int ox = (dw - ow) / 2, oy = (dh - oh) / 2;
    if (ow != dw || oh != dh) { glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT); }
    glBlitFramebuffer(0, 0, (GLint)g_bb_w, (GLint)g_bb_h, ox, oy + oh, ox + ow, oy, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    if (C.scis) glEnable(GL_SCISSOR_TEST);
    { TB; SDL_GL_SwapWindow(g_win); TE(T_SWAP); }
    glBindFramebuffer(GL_FRAMEBUFFER, C.fb);
    g_frames++;
    if (g_fps_on < 0) g_timing = g_fps_on = getenv("SCUD_HOST_FPS") != NULL;
    if (g_fps_on) {
        u64 now = SDL_GetTicks64(); if (!g_fps_t0) g_fps_t0 = now; g_fps_n++;
        if (now - g_fps_t0 >= 1000) {
            double n = g_fps_n ? (double)g_fps_n : 1;
            fprintf(stderr, "[fps] presented %.1f frames/s (%.0f draws/frame; ms/frame: draws %.1f, buffers %.1f, textures %.1f, clears %.1f, swap %.1f)\n", g_fps_n * 1000.0 / (now - g_fps_t0), g_draws / n,
                    g_t[T_DRAW] / n / 1e6, g_t[T_BUF] / n / 1e6, g_t[T_TEX] / n / 1e6, g_t[T_CLEAR] / n / 1e6, g_t[T_SWAP] / n / 1e6);
            g_fps_t0 = now; g_fps_n = 0; g_draws = 0; memset(g_t, 0, sizeof g_t);
        }
    }
    GLenum e = glGetError(); if (e) { static int n; if (n++ < 20) dlog("GL error %#x in frame %llu", e, (unsigned long long)g_frames); }
    return S_OK;
}
static HRESULT GetBuffer(struct obj *sw, u32 idx, const void *iid, struct tex **out) { (void)sw; (void)idx; (void)iid; g_backbuf->o.refs++; *out = g_backbuf; return S_OK; }
static HRESULT ResizeBuffers(struct obj *sw, u32 n, u32 w, u32 h, u32 fmt, u32 flags) {
    (void)sw; (void)n; (void)flags;
    if (!w || !h) SDL_GL_GetDrawableSize(g_win, (int *)&w, (int *)&h);
    g_bb_w = w; g_bb_h = h; if (fmt) g_bb_fmt = fmt;
    dlog("swap chain resized to %ux%u", w, h);
    make_backbuf(); return S_OK;
}
static HRESULT GetFrameStatistics(struct obj *sw, void *st) { (void)sw; (void)st; return (HRESULT)0x887A000B; /* DXGI_ERROR_FRAME_STATISTICS_DISJOINT */ }
static HRESULT SetFullscreenState(struct obj *sw, u32 fs, void *out) { (void)sw; (void)fs; (void)out; return S_OK; }
static HRESULT GetFullscreenState(struct obj *sw, u32 *fs, void **out) { (void)sw; if (fs) *fs = 0; if (out) *out = NULL; return S_OK; }
static HRESULT SwapGetDesc(struct obj *sw, uint8_t *d) {
    (void)sw; memset(d, 0, 72); u32 v[2] = { g_bb_w, g_bb_h }; memcpy(d, v, 8); memcpy(d + 16, &g_bb_fmt, 4);
    u32 one = 1, two = 2; memcpy(d + 28, &one, 4); memcpy(d + 40, &two, 4); memcpy(d + 56, &one, 4); return S_OK;
}

/* ------------------------------------------------------------------ D3DCompile */
static HRESULT D3DCompile(const char *src, u64 len, const char *name, const void *defs, void *inc, const char *entry, const char *target,
                          u64 f1, u64 f2, struct blob **code, struct blob **err) {
    (void)src; (void)len; (void)name; (void)defs; (void)inc; (void)f1; (void)f2;
    if (err) *err = NULL;
    for (int i = 0; i < NSHADERS; i++) if (entry && !strcmp(k_shaders[i].entry, entry)) {
        struct blob *b = mk(sizeof *b, K_BLOB, vt_blob); memcpy(b->data, "GLSH", 4); memcpy(b->data + 4, &i, 4); b->size = 8;
        *code = b; return S_OK;
    }
    DBG("D3DCompile %s (%s, %s): no GLSL port", entry ? entry : "?", target ? target : "?", name ? name : "?");
    if (code) *code = NULL;
    return E_FAIL;
}
static u64 g_tok_compile;
u64 d3dgl_getproc(const char *name) { return (name && !strcmp(name, "D3DCompile")) ? g_tok_compile : 0; }

/* ------------------------------------------------------------------ setup */
static u64 *vt_dev, *vt_ctx, *vt_swap;
static void build_vtables(void) {
    static const struct meth dev[] = {
        { 3, "CreateBuffer", (void *)CreateBuffer, "w:qqqq" }, { 5, "CreateTexture2D", (void *)CreateTexture2D, "w:qqqq" },
        { 7, "CreateShaderResourceView", (void *)CreateSRV, "w:qqqq" }, { 9, "CreateRenderTargetView", (void *)CreateRTV, "w:qqqq" },
        { 10, "CreateDepthStencilView", (void *)CreateDSV, "w:qqqq" }, { 11, "CreateInputLayout", (void *)CreateInputLayout, "w:qqwqqq" },
        { 12, "CreateVertexShader", (void *)CreateVS, "w:qqqqq" }, { 15, "CreatePixelShader", (void *)CreatePS, "w:qqqqq" },
        { 20, "CreateBlendState", (void *)CreateBlendState, "w:qqq" }, { 21, "CreateDepthStencilState", (void *)CreateDepthStencilState, "w:qqq" },
        { 22, "CreateRasterizerState", (void *)CreateRasterizerState, "w:qqq" }, { 23, "CreateSamplerState", (void *)CreateSamplerState, "w:qqq" },
        { 24, "CreateQuery", (void *)CreateQuery, "w:qqq" }, { 37, "GetFeatureLevel", (void *)GetFeatureLevel, "w:q" },
        { 40, "GetImmediateContext", (void *)GetImmediateContext, "v:qq" }, { 0, 0, 0, 0 } };
    static const struct meth ctx[] = {
        { 3, "GetDevice", (void *)ChildGetDevice, "v:qq" },
        { 7, "VSSetConstantBuffers", (void *)VSSetConstantBuffers, "v:qwwq" }, { 8, "PSSetShaderResources", (void *)PSSetShaderResources, "v:qwwq" },
        { 9, "PSSetShader", (void *)PSSetShader, "v:qqqw" }, { 10, "PSSetSamplers", (void *)PSSetSamplers, "v:qwwq" },
        { 11, "VSSetShader", (void *)VSSetShader, "v:qqqw" }, { 12, "DrawIndexed", (void *)DrawIndexed, "v:qwww" },
        { 13, "Draw", (void *)Draw, "v:qww" }, { 14, "Map", (void *)Map, "w:qqwwwq" }, { 15, "Unmap", (void *)Unmap, "v:qqw" },
        { 16, "PSSetConstantBuffers", (void *)PSSetConstantBuffers, "v:qwwq" }, { 17, "IASetInputLayout", (void *)IASetInputLayout, "v:qq" },
        { 18, "IASetVertexBuffers", (void *)IASetVertexBuffers, "v:qwwqqq" }, { 19, "IASetIndexBuffer", (void *)IASetIndexBuffer, "v:qqww" },
        { 20, "DrawIndexedInstanced", (void *)DrawIndexedInstanced, "v:qwwwww" }, { 21, "DrawInstanced", (void *)DrawInstanced, "v:qwwww" },
        { 24, "IASetPrimitiveTopology", (void *)IASetPrimitiveTopology, "v:qw" }, { 25, "VSSetShaderResources", (void *)VSSetShaderResources, "v:qwwq" },
        { 27, "Begin", (void *)Begin, "v:qq" }, { 28, "End", (void *)End, "v:qq" }, { 29, "GetData", (void *)GetData, "w:qqqww" },
        { 33, "OMSetRenderTargets", (void *)OMSetRenderTargets, "v:qwqq" }, { 35, "OMSetBlendState", (void *)OMSetBlendState, "v:qqqw" },
        { 36, "OMSetDepthStencilState", (void *)OMSetDepthStencilState, "v:qqw" }, { 43, "RSSetState", (void *)RSSetState, "v:qq" },
        { 44, "RSSetViewports", (void *)RSSetViewports, "v:qwq" }, { 45, "RSSetScissorRects", (void *)RSSetScissorRects, "v:qwq" },
        { 46, "CopySubresourceRegion", (void *)CopySubresourceRegion, "v:qqwwwwqwq" }, { 47, "CopyResource", (void *)CopyResource, "v:qqq" },
        { 48, "UpdateSubresource", (void *)UpdateSubresource, "v:qqwqqww" }, { 50, "ClearRenderTargetView", (void *)ClearRenderTargetView, "v:qqq" },
        { 53, "ClearDepthStencilView", (void *)ClearDepthStencilView, "v:qqwfb" }, { 89, "OMGetRenderTargets", (void *)OMGetRenderTargets, "v:qwqq" },
        { 110, "ClearState", (void *)ClearState, "v:q" }, { 111, "Flush", (void *)Flush, "v:q" }, { 0, 0, 0, 0 } };
    static const struct meth swap[] = {
        { 8, "Present", (void *)Present, "w:qww" }, { 9, "GetBuffer", (void *)GetBuffer, "w:qwqq" },
        { 10, "SetFullscreenState", (void *)SetFullscreenState, "w:qwq" }, { 11, "GetFullscreenState", (void *)GetFullscreenState, "w:qqq" },
        { 12, "GetDesc", (void *)SwapGetDesc, "w:qq" }, { 13, "ResizeBuffers", (void *)ResizeBuffers, "w:qwwwww" },
        { 16, "GetFrameStatistics", (void *)GetFrameStatistics, "w:qq" }, { 0, 0, 0, 0 } };
    static const struct meth tex[] = { { 3, "GetDevice", (void *)ChildGetDevice, "v:qq" }, { 7, "GetType", (void *)ResGetType, "v:qq" }, { 10, "GetDesc", (void *)TexGetDesc, "v:qq" }, { 0, 0, 0, 0 } };
    static const struct meth buf[] = { { 3, "GetDevice", (void *)ChildGetDevice, "v:qq" }, { 7, "GetType", (void *)ResGetType, "v:qq" }, { 10, "GetDesc", (void *)BufGetDesc, "v:qq" }, { 0, 0, 0, 0 } };
    static const struct meth view[] = { { 3, "GetDevice", (void *)ChildGetDevice, "v:qq" }, { 7, "GetResource", (void *)ViewGetResource, "v:qq" }, { 0, 0, 0, 0 } };
    static const struct meth misc[] = { { 3, "GetDevice", (void *)ChildGetDevice, "v:qq" }, { 0, 0, 0, 0 } };
    static const struct meth query[] = { { 3, "GetDevice", (void *)ChildGetDevice, "v:qq" }, { 7, "GetDataSize", (void *)QueryGetDataSize, "w:q" }, { 0, 0, 0, 0 } };
    static const struct meth blob[] = { { 3, "GetBufferPointer", (void *)BlobPtr, "q:q" }, { 4, "GetBufferSize", (void *)BlobSize, "q:q" }, { 0, 0, 0, 0 } };
    vt_dev = mkvt("ID3D11Device", 43, dev); vt_ctx = mkvt("ID3D11DeviceContext", 115, ctx); vt_swap = mkvt("IDXGISwapChain", 18, swap);
    vt_tex = mkvt("ID3D11Texture2D", 12, tex); vt_buf = mkvt("ID3D11Buffer", 12, buf); vt_view = mkvt("ID3D11View", 9, view);
    vt_misc = mkvt("ID3D11DeviceChild", 12, misc); vt_query = mkvt("ID3D11Query", 9, query); vt_blob = mkvt("ID3DBlob", 5, blob);
    g_device_o = (struct obj){ vt_dev, 1, K_DEVICE }; g_ctx_o = (struct obj){ vt_ctx, 1, K_CONTEXT }; g_swap_o = (struct obj){ vt_swap, 1, K_SWAP };
}

/* SCUD_HOST_GL_SCALE < 1: the game is told the window is that much smaller, so it sizes its swap chain and 3D picture
 * to match; Present scales the picture up to the real window */
void d3dgl_window_size(int *w, int *h) {
    static int init; if (!init) { init = 1; const char *sc = getenv("SCUD_HOST_GL_SCALE"); if (sc) { float f = strtof(sc, NULL); if (f >= 0.25f && f <= 1.0f) g_scale = f; } }
    if (g_scale >= 1.0f || !g_gl) return;
    if (w) *w = (int)(*w * g_scale + 0.5f);
    if (h) *h = (int)(*h * g_scale + 0.5f);
}

/* called by misc.c before the window is made: what SDL needs to give it a GL surface */
int d3dgl_wanted(void) {
    static int w = -1;
    if (w < 0) { const char *e = getenv("SCUD_HOST_GL"); w = !(e && *e == '0') && !getenv("M3_SOFT3D"); }
    return w;
}
void d3dgl_window_attrs(void) {
#ifdef __APPLE__
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4); SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
#else
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3); SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
#endif
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0); SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 0);
}

static int has_ext(const char *name) {
    GLint n = 0; glGetIntegerv(GL_NUM_EXTENSIONS, &n);
    for (GLint i = 0; i < n; i++) { const char *e = (const char *)glGetStringi(GL_EXTENSIONS, (GLuint)i); if (e && !strcmp(e, name)) return 1; }
    return 0;
}

HRESULT d3dgl_D3D11CreateDeviceAndSwapChain(void *adapter, u32 dtype, void *sw, u32 flags, const u32 *levels, u32 nlev, u32 sdk,
                                            const uint8_t *desc, struct obj **swap, struct obj **dev, u32 *level, struct obj **ctx) {
    (void)adapter; (void)sw; (void)flags; (void)levels; (void)nlev; (void)sdk;
    if (g_dbg < 0) { const char *e = getenv("SCUD_HOST_D3D_DEBUG"); g_dbg = e && *e && *e != '0'; }
    if (dtype != 1) return E_FAIL;                         /* only "hardware"; the WARP retry gets the same answer */
    if (!d3dgl_wanted() || !g_win) { dlog("no GL window: the game draws in software"); return E_FAIL; }
    if (!g_gl) {
        g_gl = SDL_GL_CreateContext(g_win);
        if (!g_gl) { dlog("no GL context: %s; the game draws in software", SDL_GetError()); return E_FAIL; }
        t_current = 1; g_owner = pthread_self(); g_owned = 1;
        g_gles = strncmp((const char *)glGetString(GL_VERSION), "OpenGL ES", 9) == 0;
        dlog("%s / %s / %s", glGetString(GL_VENDOR), glGetString(GL_RENDERER), glGetString(GL_VERSION));
        const char *cc = getenv("SCUD_HOST_CLIPCTL");
        if (!(cc && *cc == '0') && (has_ext("GL_EXT_clip_control") || has_ext("GL_ARB_clip_control"))) {
            p_glClipControl = (void (*)(GLenum, GLenum))SDL_GL_GetProcAddress(has_ext("GL_EXT_clip_control") ? "glClipControlEXT" : "glClipControl");
            if (!p_glClipControl) p_glClipControl = (void (*)(GLenum, GLenum))SDL_GL_GetProcAddress("glClipControl");
            if (p_glClipControl) { p_glClipControl(0x8CA1 /* LOWER_LEFT */, 0x935F /* ZERO_TO_ONE */); g_clipz01 = 1; }
        }
        dlog("depth: %s", g_clipz01 ? "glClipControl (0..1)" : "written by the fragment shaders");
        int desk = !g_gles;
        if (desk || has_ext("GL_OES_draw_elements_base_vertex") || has_ext("GL_EXT_draw_elements_base_vertex")) {
            p_DrawElementsBaseVertex = SDL_GL_GetProcAddress("glDrawElementsBaseVertex");
            if (!p_DrawElementsBaseVertex) p_DrawElementsBaseVertex = SDL_GL_GetProcAddress("glDrawElementsBaseVertexOES");
            if (!p_DrawElementsBaseVertex) p_DrawElementsBaseVertex = SDL_GL_GetProcAddress("glDrawElementsBaseVertexEXT");
            p_DrawRangeElementsBaseVertex = SDL_GL_GetProcAddress("glDrawRangeElementsBaseVertex");
            if (!p_DrawRangeElementsBaseVertex) p_DrawRangeElementsBaseVertex = SDL_GL_GetProcAddress("glDrawRangeElementsBaseVertexOES");
            if (!p_DrawRangeElementsBaseVertex) p_DrawRangeElementsBaseVertex = SDL_GL_GetProcAddress("glDrawRangeElementsBaseVertexEXT");
            if (getenv("SCUD_HOST_GL_NORANGE")) p_DrawRangeElementsBaseVertex = NULL;
        }
        if (has_ext("GL_EXT_base_instance") || has_ext("GL_ARB_base_instance")) {
            p_DrawElementsInstancedBaseVertexBaseInstance = SDL_GL_GetProcAddress("glDrawElementsInstancedBaseVertexBaseInstance");
            if (!p_DrawElementsInstancedBaseVertexBaseInstance) p_DrawElementsInstancedBaseVertexBaseInstance = SDL_GL_GetProcAddress("glDrawElementsInstancedBaseVertexBaseInstanceEXT");
        }
        dlog("draws: base vertex %s, base instance %s", p_DrawElementsBaseVertex ? "yes" : "no", p_DrawElementsInstancedBaseVertexBaseInstance ? "yes" : "no");
        g_nodraw = getenv("SCUD_HOST_GL_NODRAW") != NULL;
        g_syncbuf = getenv("SCUD_HOST_GL_SYNCBUF") != NULL;
        const char *q = getenv("SCUD_HOST_GL_TEXQ"); if (q && *q >= '0' && *q <= '2') g_texq = *q - '0';
        dlog("3D texture filtering %s, picture at %.0f%% of the window", g_texq == 0 ? "point" : g_texq == 1 ? "bilinear" : "trilinear", g_scale * 100);
        if (g_dbg) { GLint n = 0; glGetIntegerv(GL_NUM_EXTENSIONS, &n); for (GLint i = 0; i < n; i++) dlog("ext %s", glGetStringi(GL_EXTENSIONS, (GLuint)i)); }
        build_vtables();
        g_tok_compile = rt_import_token("D3DCompile", (void *)D3DCompile, "w:qqqqqqqwwqq");
        glGenVertexArrays(1, &g_vao); glBindVertexArray(g_vao);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        memset(&C, 0xff, sizeof C); C.fb = 0; C.prog = 0;
        S.topo = GL_TRIANGLES;
    }
    u32 w, h, fmt; memcpy(&w, desc, 4); memcpy(&h, desc + 4, 4); memcpy(&fmt, desc + 16, 4);
    if (!w || !h) SDL_GL_GetDrawableSize(g_win, (int *)&w, (int *)&h);
    g_bb_w = w; g_bb_h = h; g_bb_fmt = fmt ? fmt : 0x57;
    make_backbuf();
    if (swap) *swap = &g_swap_o;
    if (dev) *dev = &g_device_o;
    if (ctx) *ctx = &g_ctx_o;
    if (level) *level = 0xb000;
    dlog("Direct3D 11 device on GL: back buffer %ux%u", w, h);
    return S_OK;
}
