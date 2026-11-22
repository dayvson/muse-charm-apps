#include "luapp_image.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "esp32s3/rom/tjpgd.h"
#include "miniz.h"
#define TMP_ALLOC(n) heap_caps_malloc((n), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define TMP_CALLOC(n) heap_caps_calloc(1, (n), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define TMP_FREE(p) heap_caps_free(p)
#else
#include <zlib.h>
#define TMP_ALLOC(n) malloc(n)
#define TMP_CALLOC(n) calloc(1, (n))
#define TMP_FREE(p) free(p)
#endif

#define FAIL(msg) do { *err = (msg); goto done; } while (0)

/* ---- base64 ---- */

size_t luapp_base64_decode(const char *in, size_t n, uint8_t *out)
{
    uint32_t acc = 0;
    int bits = 0;
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)in[i];
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+' || c == '-') v = 62;
        else if (c == '/' || c == '_') v = 63;
        else if (c == '=') break;
        else if (c == ' ' || c == '\n' || c == '\r' || c == '\t') continue;
        else return (size_t)-1;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[o++] = (uint8_t)(acc >> bits);
        }
    }
    return o;
}

/* ---- streaming inflate: the ROM's tinfl on the board, zlib on the host ---- */

typedef bool (*put_fn)(const uint8_t *p, size_t n, void *ud);

static bool inflate_stream(const uint8_t *in, size_t len, put_fn put, void *ud)
{
#ifdef ESP_PLATFORM
    tinfl_decompressor *d = TMP_ALLOC(sizeof(*d));
    uint8_t *dict = TMP_ALLOC(TINFL_LZ_DICT_SIZE);
    bool ok = false;
    if (d && dict) {
        tinfl_init(d);
        size_t in_ofs = 0, dict_ofs = 0;
        for (;;) {
            size_t in_n = len - in_ofs, out_n = TINFL_LZ_DICT_SIZE - dict_ofs;
            tinfl_status st = tinfl_decompress(d, in + in_ofs, &in_n, dict, dict + dict_ofs, &out_n,
                                               TINFL_FLAG_PARSE_ZLIB_HEADER);
            in_ofs += in_n;
            if (out_n && !put(dict + dict_ofs, out_n, ud)) break;
            if (st != TINFL_STATUS_HAS_MORE_OUTPUT) {
                ok = st == TINFL_STATUS_DONE;
                break;
            }
            dict_ofs = (dict_ofs + out_n) & (TINFL_LZ_DICT_SIZE - 1);
        }
    }
    TMP_FREE(d);
    TMP_FREE(dict);
    return ok;
#else
    z_stream z;
    memset(&z, 0, sizeof(z));
    if (inflateInit(&z) != Z_OK) return false;
    uint8_t buf[4096];
    z.next_in = (Bytef *)in;
    z.avail_in = (uInt)len;
    int rc;
    do {
        z.next_out = buf;
        z.avail_out = sizeof(buf);
        rc = inflate(&z, Z_NO_FLUSH);
        if (rc != Z_OK && rc != Z_STREAM_END) break;
        size_t n = sizeof(buf) - z.avail_out;
        if (n && !put(buf, n, ud)) {
            rc = Z_DATA_ERROR;
            break;
        }
    } while (rc != Z_STREAM_END && (z.avail_in || z.avail_out == 0));
    inflateEnd(&z);
    return rc == Z_STREAM_END;
#endif
}

/* ---- PNG ---- */

typedef struct {
    uint32_t w, h;
    int depth, ctype, bpp;       /* bpp: bytes per pixel for the filters (at least 1) */
    size_t rowbytes;
    uint8_t pal[256][4];
    int npal;
    bool trns;
    uint16_t tg, tr, tgg, tb;    /* tRNS key for grey / RGB images */
    uint8_t *cur, *prev;         /* filter byte + rowbytes */
    size_t fill;
    uint32_t y;
    bool bad;
    luapp_pixels_t *out;
} png_t;

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static uint8_t paeth(int a, int b, int c)
{
    int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    return (uint8_t)(pa <= pb && pa <= pc ? a : pb <= pc ? b : c);
}

static bool unfilter(png_t *g)
{
    uint8_t *r = g->cur + 1, *p = g->prev + 1;
    size_t n = g->rowbytes, bpp = (size_t)g->bpp;
    switch (g->cur[0]) {
    case 0: break;
    case 1: for (size_t i = bpp; i < n; i++) r[i] += r[i - bpp]; break;
    case 2: for (size_t i = 0; i < n; i++) r[i] += p[i]; break;
    case 3:
        for (size_t i = 0; i < n; i++) r[i] += (uint8_t)(((i >= bpp ? r[i - bpp] : 0) + p[i]) >> 1);
        break;
    case 4:
        for (size_t i = 0; i < n; i++) r[i] += paeth(i >= bpp ? r[i - bpp] : 0, p[i], i >= bpp ? p[i - bpp] : 0);
        break;
    default: return false;
    }
    return true;
}

/* Sample x (channel ch) of the row: 8 bits, and the raw value for tRNS. */
static inline uint8_t sample(const png_t *g, const uint8_t *r, uint32_t x, int ch, int nch, uint16_t *raw)
{
    if (g->depth == 16) {
        const uint8_t *s = r + ((size_t)x * nch + ch) * 2;
        if (raw) *raw = (uint16_t)(s[0] << 8 | s[1]);
        return s[0];
    }
    if (g->depth == 8) {
        uint8_t v = r[(size_t)x * nch + ch];
        if (raw) *raw = v;
        return v;
    }
    size_t bit = (size_t)x * g->depth;
    unsigned v = (r[bit >> 3] >> (8 - g->depth - (bit & 7))) & ((1u << g->depth) - 1);
    if (raw) *raw = (uint16_t)v;
    return g->ctype == 3 ? (uint8_t)v : (uint8_t)(v * 255 / ((1u << g->depth) - 1));
}

static void emit_row(png_t *g)
{
    const uint8_t *r = g->cur + 1;
    luapp_pixels_t *o = g->out;
    uint16_t *c = (uint16_t *)o->px + (size_t)g->y * o->w;
    uint8_t *a = o->alpha ? o->px + (size_t)o->w * o->h * 2 + (size_t)g->y * o->w : NULL;
    for (uint32_t x = 0; x < g->w; x++) {
        uint8_t R, G, B, A = 255;
        uint16_t k0, k1, k2;
        switch (g->ctype) {
        case 0:
            R = G = B = sample(g, r, x, 0, 1, &k0);
            if (g->trns && k0 == g->tg) A = 0;
            break;
        case 2:
            R = sample(g, r, x, 0, 3, &k0);
            G = sample(g, r, x, 1, 3, &k1);
            B = sample(g, r, x, 2, 3, &k2);
            if (g->trns && k0 == g->tr && k1 == g->tgg && k2 == g->tb) A = 0;
            break;
        case 3: {
            uint8_t i = sample(g, r, x, 0, 1, NULL);
            R = g->pal[i][0];
            G = g->pal[i][1];
            B = g->pal[i][2];
            A = g->pal[i][3];
            break;
        }
        case 4:
            R = G = B = sample(g, r, x, 0, 2, NULL);
            A = sample(g, r, x, 1, 2, NULL);
            break;
        default:
            R = sample(g, r, x, 0, 4, NULL);
            G = sample(g, r, x, 1, 4, NULL);
            B = sample(g, r, x, 2, 4, NULL);
            A = sample(g, r, x, 3, 4, NULL);
            break;
        }
        c[x] = luapp_rgb565(R, G, B);
        if (a) a[x] = A;
    }
}

static bool png_put(const uint8_t *p, size_t n, void *ud)
{
    png_t *g = ud;
    size_t need = g->rowbytes + 1;
    while (n && g->y < g->h) {
        size_t k = need - g->fill < n ? need - g->fill : n;
        memcpy(g->cur + g->fill, p, k);
        g->fill += k;
        p += k;
        n -= k;
        if (g->fill == need) {
            if (!unfilter(g)) {
                g->bad = true;
                return false;
            }
            emit_row(g);
            uint8_t *t = g->prev;
            g->prev = g->cur;
            g->cur = t;
            g->fill = 0;
            g->y++;
        }
    }
    return true;   /* bytes past the last row are ignored */
}

static bool decode_png(const uint8_t *d, size_t len, int max_dim, luapp_px_alloc_fn alloc, void *ud,
                       luapp_pixels_t *out, const char **err)
{
    png_t g;
    memset(&g, 0, sizeof(g));
    uint8_t *idat = NULL;
    size_t idat_len = 0;
    bool ok = false, have_hdr = false;
    for (int i = 0; i < 256; i++) g.pal[i][3] = 255;
    size_t pos = 8;
    while (pos + 12 <= len) {
        uint32_t n = be32(d + pos);
        const uint8_t *type = d + pos + 4, *body = d + pos + 8;
        if (n > len - pos - 12) FAIL("PNG is cut short");
        if (!memcmp(type, "IHDR", 4)) {
            if (n < 13) FAIL("bad PNG header");
            g.w = be32(body);
            g.h = be32(body + 4);
            g.depth = body[8];
            g.ctype = body[9];
            if (body[10] || body[11]) FAIL("bad PNG header");
            if (body[12]) FAIL("interlaced PNGs aren't supported: save it without interlacing");
            static const int nch[7] = {1, 0, 3, 1, 2, 0, 4};
            if (g.ctype > 6 || !nch[g.ctype]) FAIL("bad PNG colour type");
            bool depth_ok = g.depth == 8 || g.depth == 16 ||
                            ((g.ctype == 0 || g.ctype == 3) && (g.depth == 1 || g.depth == 2 || g.depth == 4));
            if (!depth_ok || (g.ctype == 3 && g.depth == 16)) FAIL("bad PNG bit depth");
            if (g.w == 0 || g.h == 0) FAIL("empty PNG");
            if (g.w > (uint32_t)max_dim || g.h > (uint32_t)max_dim) FAIL("image too large (466 x 466 at most)");
            size_t bits = (size_t)nch[g.ctype] * g.depth;
            g.rowbytes = (g.w * bits + 7) / 8;
            g.bpp = bits >= 8 ? (int)(bits / 8) : 1;
            have_hdr = true;
        } else if (!memcmp(type, "PLTE", 4)) {
            if (n % 3 || n > 768) FAIL("bad PNG palette");
            g.npal = (int)(n / 3);
            for (int i = 0; i < g.npal; i++) memcpy(g.pal[i], body + 3 * i, 3);
        } else if (!memcmp(type, "tRNS", 4)) {
            g.trns = true;
            if (g.ctype == 3) {
                for (uint32_t i = 0; i < n && i < 256; i++) g.pal[i][3] = body[i];
            } else if (g.ctype == 0 && n >= 2) {
                g.tg = (uint16_t)(body[0] << 8 | body[1]);
            } else if (g.ctype == 2 && n >= 6) {
                g.tr = (uint16_t)(body[0] << 8 | body[1]);
                g.tgg = (uint16_t)(body[2] << 8 | body[3]);
                g.tb = (uint16_t)(body[4] << 8 | body[5]);
            } else {
                g.trns = false;
            }
        } else if (!memcmp(type, "IDAT", 4)) {
            if (!idat) {
                idat = TMP_ALLOC(len);   /* the IDATs together can't exceed the file */
                if (!idat) FAIL("out of memory decoding the image");
            }
            memcpy(idat + idat_len, body, n);
            idat_len += n;
        } else if (!memcmp(type, "IEND", 4)) {
            break;
        }
        pos += 12 + (size_t)n;
    }
    if (!have_hdr) FAIL("bad PNG: no header");
    if (!idat_len) FAIL("bad PNG: no image data");
    if (g.ctype == 3 && !g.npal) FAIL("bad PNG: no palette");

    bool alpha = g.ctype == 4 || g.ctype == 6 || g.trns;
    if (g.ctype == 3 && alpha) {
        alpha = false;
        for (int i = 0; i < g.npal; i++) alpha |= g.pal[i][3] != 255;
    }
    out->w = (uint16_t)g.w;
    out->h = (uint16_t)g.h;
    out->alpha = alpha;
    out->bytes = (size_t)g.w * g.h * (alpha ? 3 : 2);
    out->px = alloc(out->bytes, ud);
    if (!out->px) FAIL("image memory is full: free images you don't need (img:free()) or use smaller ones");
    g.out = out;
    g.cur = TMP_CALLOC(g.rowbytes + 1);
    g.prev = TMP_CALLOC(g.rowbytes + 1);
    if (!g.cur || !g.prev) FAIL("out of memory decoding the image");
    bool inflated = inflate_stream(idat, idat_len, png_put, &g);
    if (g.bad) FAIL("bad PNG: unknown row filter");
    if (g.y < g.h) FAIL(inflated ? "bad PNG: image data is short" : "bad PNG: image data is damaged");
    ok = true;
done:
    TMP_FREE(idat);
    TMP_FREE(g.cur);
    TMP_FREE(g.prev);
    return ok;   /* on failure the caller frees out->px */
}

/* ---- JPEG (ROM TJpgDec) ---- */

#ifdef ESP_PLATFORM
typedef struct {
    const uint8_t *data;
    size_t len, pos;
    luapp_pixels_t *out;
} jstream_t;

static UINT jpeg_in(JDEC *jd, BYTE *buf, UINT n)
{
    jstream_t *s = jd->device;
    if (n > s->len - s->pos) n = (UINT)(s->len - s->pos);
    if (buf) memcpy(buf, s->data + s->pos, n);
    s->pos += n;
    return n;
}

static UINT jpeg_out(JDEC *jd, void *bitmap, JRECT *r)
{
    jstream_t *s = jd->device;
    luapp_pixels_t *o = s->out;
    const uint8_t *rgb = bitmap;
    int bw = r->right - r->left + 1;
    for (int y = r->top; y <= r->bottom; y++) {
        const uint8_t *src = rgb + (size_t)(y - r->top) * bw * 3;
        if (y >= o->h) break;
        uint16_t *dst = (uint16_t *)o->px + (size_t)y * o->w;
        for (int x = r->left; x <= r->right; x++, src += 3) {
            if (x < o->w) dst[x] = luapp_rgb565(src[0], src[1], src[2]);
        }
    }
    return 1;
}

static bool decode_jpeg(const uint8_t *d, size_t len, int max_dim, luapp_px_alloc_fn alloc, void *ud,
                        luapp_pixels_t *out, const char **err)
{
    enum { POOL = 4096 };
    void *pool = TMP_ALLOC(POOL);
    JDEC *jd = TMP_CALLOC(sizeof(JDEC));
    bool ok = false;
    jstream_t s = {.data = d, .len = len, .out = out};
    if (!pool || !jd) FAIL("out of memory decoding the image");
    JRESULT rc = jd_prepare(jd, jpeg_in, pool, POOL, &s);
    if (rc == JDR_FMT3) FAIL("progressive JPEGs aren't supported: use a baseline JPEG or a PNG");
    if (rc != JDR_OK) FAIL("bad or unsupported JPEG");
    int scale = 0;
    while (scale < 3 && ((int)(jd->width >> scale) > max_dim || (int)(jd->height >> scale) > max_dim)) scale++;
    if ((int)(jd->width >> scale) > max_dim || (int)(jd->height >> scale) > max_dim) FAIL("image too large");
    out->w = (uint16_t)(jd->width >> scale);
    out->h = (uint16_t)(jd->height >> scale);
    if (!out->w || !out->h) FAIL("empty JPEG");
    out->alpha = false;
    out->bytes = (size_t)out->w * out->h * 2;
    out->px = alloc(out->bytes, ud);
    if (!out->px) FAIL("image memory is full: free images you don't need (img:free()) or use smaller ones");
    memset(out->px, 0, out->bytes);
    rc = jd_decomp(jd, jpeg_out, (BYTE)scale);
    if (rc != JDR_OK) FAIL("bad JPEG data");
    ok = true;
done:
    TMP_FREE(pool);
    TMP_FREE(jd);
    return ok;
}
#endif

/* ---- entry points ---- */

static bool is_png(const uint8_t *d, size_t n) { return n >= 8 && !memcmp(d, "\x89PNG\r\n\x1a\n", 8); }
static bool is_jpeg(const uint8_t *d, size_t n) { return n >= 3 && d[0] == 0xff && d[1] == 0xd8 && d[2] == 0xff; }

bool luapp_img_decode(const uint8_t *data, size_t len, int max_dim, luapp_px_alloc_fn alloc, void *ud,
                      luapp_pixels_t *out, const char **err)
{
    memset(out, 0, sizeof(*out));
    uint8_t *raw = NULL;
    bool ok = false;
    if (!is_png(data, len) && !is_jpeg(data, len)) {
        /* Base64, maybe as a data: URL. */
        const char *s = (const char *)data;
        size_t n = len;
        if (n > 5 && !memcmp(s, "data:", 5)) {
            const char *comma = memchr(s, ',', n);
            if (!comma) FAIL("bad data: URL");
            n -= (size_t)(comma + 1 - s);
            s = comma + 1;
        }
        raw = TMP_ALLOC(n * 3 / 4 + 4);
        if (!raw) FAIL("out of memory decoding the image");
        size_t m = luapp_base64_decode(s, n, raw);
        if (m == (size_t)-1 || !(is_png(raw, m) || is_jpeg(raw, m))) {
            FAIL("not a PNG or JPEG (raw bytes or base64)");
        }
        data = raw;
        len = m;
    }
    if (is_png(data, len)) {
        ok = decode_png(data, len, max_dim, alloc, ud, out, err);
    } else {
#ifdef ESP_PLATFORM
        ok = decode_jpeg(data, len, max_dim, alloc, ud, out, err);
#else
        *err = "JPEG needs the board's ROM decoder";
#endif
    }
done:
    TMP_FREE(raw);
    return ok;
}

bool luapp_img_sprite(const char *const *rows, const size_t *lens, int nrows, const uint32_t *pal,
                      const bool *has, int scale, int max_dim, luapp_px_alloc_fn alloc, void *ud,
                      luapp_pixels_t *out, const char **err)
{
    memset(out, 0, sizeof(*out));
    size_t w = 0;
    for (int i = 0; i < nrows; i++) w = lens[i] > w ? lens[i] : w;
    if (!nrows || !w) {
        *err = "a sprite needs rows of characters";
        return false;
    }
    if (scale < 1) scale = 1;
    if (w * scale > (size_t)max_dim || (size_t)nrows * scale > (size_t)max_dim) {
        *err = "sprite too large (466 x 466 pixels at most, after scale)";
        return false;
    }
    out->w = (uint16_t)(w * scale);
    out->h = (uint16_t)(nrows * scale);
    out->alpha = true;
    out->bytes = (size_t)out->w * out->h * 3;
    out->px = alloc(out->bytes, ud);
    if (!out->px) {
        *err = "image memory is full: free images you don't need (img:free()) or use smaller ones";
        return false;
    }
    uint16_t *c = (uint16_t *)out->px;
    uint8_t *a = out->px + (size_t)out->w * out->h * 2;
    for (int y = 0; y < out->h; y++) {
        const char *row = rows[y / scale];
        size_t rn = lens[y / scale];
        for (int x = 0; x < out->w; x++) {
            size_t sx = (size_t)(x / scale);
            unsigned char ch = sx < rn ? (unsigned char)row[sx] : ' ';
            size_t i = (size_t)y * out->w + x;
            if (has[ch]) {
                uint32_t v = pal[ch];
                c[i] = luapp_rgb565((v >> 16) & 255, (v >> 8) & 255, v & 255);
                a[i] = 255;
            } else {
                c[i] = 0;
                a[i] = 0;
            }
        }
    }
    return true;
}

/* ---- launcher icons ---- */

void luapp_icon_make(const luapp_pixels_t *src, uint8_t out[LUAPP_ICON_FILE])
{
    const int N = LUAPP_ICON, w = src->w, h = src->h;
    const uint8_t *col = src->px, *al = src->alpha ? src->px + (size_t)w * h * 2 : NULL;
    bool opaque = true;
    for (size_t i = 0; al && i < (size_t)w * h && opaque; i++) opaque = al[i] == 255;

    /* s: icon pixels per image pixel; (ox, oy): where the image's corner lands. */
    double s;
    if (opaque) {
        s = fmax((double)N / w, (double)N / h);
    } else {
        s = fmin(72.0 / w, 72.0 / h);
        if (s >= 1) s = floor(s);
    }
    double ox = (N - w * s) / 2, oy = (N - h * s) / 2;
    uint8_t *rgb = out + 4, *alpha = out + 4 + N * N * 2;
    memcpy(out, "LIC", 3);
    out[3] = opaque ? LUAPP_ICON_FILLS : 0;

    for (int y = 0; y < N; y++) {
        int y0, y1;
        if (s >= 1) {
            y0 = (int)floor((y + 0.5 - oy) / s);
            y1 = y0 + 1;
        } else {
            y0 = (int)floor((y - oy) / s);
            y1 = (int)ceil((y + 1 - oy) / s);
        }
        if (y0 < 0) y0 = 0;
        if (y1 > h) y1 = h;
        for (int x = 0; x < N; x++) {
            int x0, x1;
            if (s >= 1) {
                x0 = (int)floor((x + 0.5 - ox) / s);
                x1 = x0 + 1;
            } else {
                x0 = (int)floor((x - ox) / s);
                x1 = (int)ceil((x + 1 - ox) / s);
            }
            if (x0 < 0) x0 = 0;
            if (x1 > w) x1 = w;
            /* Average, weighted by alpha so transparent pixels don't darken edges. */
            uint32_t r = 0, g = 0, b = 0, a = 0, n = 0;
            for (int sy = y0; sy < y1; sy++) {
                for (int sx = x0; sx < x1; sx++) {
                    size_t i = (size_t)sy * w + sx;
                    uint32_t c = col[2 * i] | (uint32_t)col[2 * i + 1] << 8, pa = al ? al[i] : 255;
                    r += (((c >> 11) & 31) * 255 / 31) * pa;
                    g += (((c >> 5) & 63) * 255 / 63) * pa;
                    b += ((c & 31) * 255 / 31) * pa;
                    a += pa;
                    n++;
                }
            }
            uint16_t c565 = 0;
            double av = 0;
            if (a) {
                c565 = luapp_rgb565(r / a, g / a, b / a);
                av = (double)a / n;
            }
            /* The circle, anti-aliased over its edge pixel. */
            double d = hypot(x + 0.5 - N / 2.0, y + 0.5 - N / 2.0), cover = N / 2.0 - d + 0.5;
            av *= cover <= 0 ? 0 : cover >= 1 ? 1 : cover;
            size_t o = (size_t)y * N + x;
            uint8_t av8 = (uint8_t)lround(av);
            if (!av8) c565 = 0;
            rgb[2 * o] = (uint8_t)c565;
            rgb[2 * o + 1] = (uint8_t)(c565 >> 8);
            alpha[o] = av8;
        }
    }
}
