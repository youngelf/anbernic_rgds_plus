/*
 * Anbernic RG DS Plus - Custom Boot Logo Changer (boot_logo.c)
 *
 * Dual-screen native 1024x768 on-device tool built on the RG DS Plus SDK (`../sdk`) that:
 *   1. Previews custom 1024x768 Top & Bottom boot logos across both screens at native
 *      hardware resolution (including a 1:1 unobstructed dual-screen preview mode).
 *   2. Includes 3 whimsical custom themes ("Wario Crunch DS", "Wario & Tux Arcade",
 *      "Bootloader Stew: Chef Wario & Sous-Chef Tux") plus the original Factory
 *      Stock Anbernic boot logo, and supports mixing & matching Top/Bottom screens.
 *   3. Safely flashes BOTH boot logo stages on the RG DS Plus:
 *      - Stage 1 (U-Boot & Linux Kernel DRM): Repacks `logo.bmp` (Top Screen,
 *        route-dsi1) and `logo_kernel.bmp` (Bottom Screen, route-dsi0) inside the
 *        Rockchip `RSCE` (`resource`) container in `/dev/disk/by-partlabel/boot`,
 *        recomputes the SHA-256 digest, updates `/images/resource/data-size`,
 *        `/images/resource/hash/value`, and `/totalsize` in the U-Boot FIT header,
 *        and verifies the written bytes by reading back and checking SHA-256.
 *      - Stage 2 (Userspace Launcher `dmenu.bin` / `muos2.bin`): Updates
 *        `/mnt/vendor/resource/res1/boot/logo.png` & `logo2.png` and
 *        `/mnt/vendor/resource/res2/boot/logo.png` & `logo2.png`.
 *   4. Automatically backs up the original `RSCE` container, FIT header, and
 *      vendor PNGs to `./backup/` before the first modification.
 */

#include "rgds_sdk.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* ============================================================================
 * Pure-C SHA-256 Implementation (for U-Boot FIT `/images/resource/hash/value`)
 * ============================================================================ */

typedef struct {
    uint32_t state[8];
    uint64_t bitcount;
    uint8_t buffer[64];
} sha256_ctx_t;

static const uint32_t k_sha256[64] = {
    0x428a2f98UL, 0x71374491UL, 0xb5c0fbcfUL, 0xe9b5dba5UL,
    0x3956c25bUL, 0x59f111f1UL, 0x923f82a4UL, 0xab1c5ed5UL,
    0xd807aa98UL, 0x12835b01UL, 0x243185beUL, 0x550c7dc3UL,
    0x72be5d74UL, 0x80deb1feUL, 0x9bdc06a7UL, 0xc19bf174UL,
    0xe49b69c1UL, 0xefbe4786UL, 0x0fc19dc6UL, 0x240ca1ccUL,
    0x2de92c6fUL, 0x4a7484aaUL, 0x5cb0a9dcUL, 0x76f988daUL,
    0x983e5152UL, 0xa831c66dUL, 0xb00327c8UL, 0xbf597fc7UL,
    0xc6e00bf3UL, 0xd5a79147UL, 0x06ca6351UL, 0x14292967UL,
    0x27b70a85UL, 0x2e1b2138UL, 0x4d2c6dfcUL, 0x53380d13UL,
    0x650a7354UL, 0x766a0abbUL, 0x81c2c92eUL, 0x92722c85UL,
    0xa2bfe8a1UL, 0xa81a664bUL, 0xc24b8b70UL, 0xc76c51a3UL,
    0xd192e819UL, 0xd6990624UL, 0xf40e3585UL, 0x106aa070UL,
    0x19a4c116UL, 0x1e376c08UL, 0x2748774cUL, 0x34b0bcb5UL,
    0x391c0cb3UL, 0x4ed8aa4aUL, 0x5b9cca4fUL, 0x682e6ff3UL,
    0x748f82eeUL, 0x78a5636fUL, 0x84c87814UL, 0x8cc70208UL,
    0x90befffaUL, 0xa4506cebUL, 0xbef9a3f7UL, 0xc67178f2UL
};

static inline uint32_t rotr32(uint32_t x, uint32_t n) {
    return (x >> n) | (x << (32U - n));
}

static void sha256_transform(sha256_ctx_t *ctx, const uint8_t data[64]) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)data[i * 4 + 0] << 24) |
               ((uint32_t)data[i * 4 + 1] << 16) |
               ((uint32_t)data[i * 4 + 2] << 8)  |
               ((uint32_t)data[i * 4 + 3]);
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = ctx->state[0];
    uint32_t b = ctx->state[1];
    uint32_t c = ctx->state[2];
    uint32_t d = ctx->state[3];
    uint32_t e = ctx->state[4];
    uint32_t f = ctx->state[5];
    uint32_t g = ctx->state[6];
    uint32_t h = ctx->state[7];

    for (int i = 0; i < 64; i++) {
        uint32_t S1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t temp1 = h + S1 + ch + k_sha256[i] + w[i];
        uint32_t S0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2 = S0 + maj;

        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
    ctx->state[5] += f;
    ctx->state[6] += g;
    ctx->state[7] += h;
}

static void sha256_compute(const uint8_t *data, size_t len, uint8_t out_hash[32]) {
    sha256_ctx_t ctx;
    ctx.state[0] = 0x6a09e667UL;
    ctx.state[1] = 0xbb67ae85UL;
    ctx.state[2] = 0x3c6ef372UL;
    ctx.state[3] = 0xa54ff53aUL;
    ctx.state[4] = 0x510e527fUL;
    ctx.state[5] = 0x9b05688cUL;
    ctx.state[6] = 0x1f83d9abUL;
    ctx.state[7] = 0x5be0cd19UL;
    ctx.bitcount = 0;

    size_t idx = 0;
    while (len >= 64) {
        sha256_transform(&ctx, data + idx);
        ctx.bitcount += 512;
        idx += 64;
        len -= 64;
    }
    memcpy(ctx.buffer, data + idx, len);
    ctx.bitcount += (uint64_t)len * 8ULL;

    ctx.buffer[len++] = 0x80;
    if (len > 56) {
        memset(ctx.buffer + len, 0, 64 - len);
        sha256_transform(&ctx, ctx.buffer);
        len = 0;
    }
    memset(ctx.buffer + len, 0, 56 - len);
    for (int i = 0; i < 8; i++) {
        ctx.buffer[56 + i] = (uint8_t)(ctx.bitcount >> (56 - i * 8));
    }
    sha256_transform(&ctx, ctx.buffer);

    for (int i = 0; i < 8; i++) {
        out_hash[i * 4 + 0] = (uint8_t)(ctx.state[i] >> 24);
        out_hash[i * 4 + 1] = (uint8_t)(ctx.state[i] >> 16);
        out_hash[i * 4 + 2] = (uint8_t)(ctx.state[i] >> 8);
        out_hash[i * 4 + 3] = (uint8_t)(ctx.state[i]);
    }
}

/* ============================================================================
 * Byte-order helpers & BMP Loader
 * ============================================================================ */

static inline uint32_t read_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | ((uint32_t)p[3]);
}

static inline void write_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)(v);
}

static inline uint32_t read_le32(const uint8_t *p) {
    return ((uint32_t)p[0]) | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline uint16_t read_le16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0]) | ((uint16_t)p[1] << 8));
}

static inline void write_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

typedef struct {
    const char *title;
    const char *subtitle;
    const char *top_bmp_path;
    const char *bot_bmp_path;
    const char *top_png_path;
    const char *bot_png_path;
    bool is_factory_stock;
    uint32_t *top_pixels; /* RGDS_NATIVE_W * RGDS_NATIVE_H (1024x768) */
    uint32_t *bot_pixels; /* RGDS_NATIVE_W * RGDS_NATIVE_H (1024x768) */
    bool loaded;
} logo_theme_t;

#define NUM_THEMES 5

static logo_theme_t g_themes[NUM_THEMES] = {
    {
        .title = "STARCROSSED KISS",
        .subtitle = "Girl blowing a glowing heart kiss across the screens",
        .top_bmp_path = "logos/theme1_top.bmp",
        .bot_bmp_path = "logos/theme1_bottom.bmp",
        .top_png_path = "logos/theme1_top.png",
        .bot_png_path = "logos/theme1_bottom.png",
        .is_factory_stock = false,
    },
    {
        .title = "COOKING PIXELS",
        .subtitle = "Chef Wario & Sous-Chef Tux (Spoon & Sack) + Garlic Furnace",
        .top_bmp_path = "logos/theme2_top.bmp",
        .bot_bmp_path = "logos/theme2_bottom.bmp",
        .top_png_path = "logos/theme2_top.png",
        .bot_png_path = "logos/theme2_bottom.png",
        .is_factory_stock = false,
    },
    {
        .title = "TUX STARRY SKY",
        .subtitle = "Tux on a glowing twilight cliff + wisps of aurora above",
        .top_bmp_path = "logos/theme3_top.bmp",
        .bot_bmp_path = "logos/theme3_bottom.bmp",
        .top_png_path = "logos/theme3_top.png",
        .bot_png_path = "logos/theme3_bottom.png",
        .is_factory_stock = false,
    },
    {
        .title = "MIDNIGHT POETRY",
        .subtitle = "Minimalist poet under warm lamp + crescent moon & cranes",
        .top_bmp_path = "logos/theme4_top.bmp",
        .bot_bmp_path = "logos/theme4_bottom.bmp",
        .top_png_path = "logos/theme4_top.png",
        .bot_png_path = "logos/theme4_bottom.png",
        .is_factory_stock = false,
    },
    {
        .title = "ANBERNIC STOCK",
        .subtitle = "Original Factory Default Anbernic Boot Logo",
        .top_bmp_path = "logos/stock_top.bmp",
        .bot_bmp_path = "logos/stock_bottom.bmp",
        .top_png_path = "logos/factory_vendor_logo.png",
        .bot_png_path = "logos/factory_vendor_logo2.png",
        .is_factory_stock = true,
    }
};

/* Load a 24-bit or 32-bit BMP into a 1024x768 ARGB8888 pixel buffer */
static bool load_bmp_to_argb(const char *path, uint32_t *out_pixels) {
    for (int i = 0; i < RGDS_NATIVE_W * RGDS_NATIVE_H; i++) {
        out_pixels[i] = RGDS_RGB(0, 0, 0);
    }

    FILE *fp = fopen(path, "rb");
    if (!fp) return false;

    uint8_t hdr[54];
    if (fread(hdr, 1, 54, fp) != 54 || hdr[0] != 'B' || hdr[1] != 'M') {
        fclose(fp);
        return false;
    }

    uint32_t off_bits = read_le32(hdr + 10);
    int32_t width = (int32_t)read_le32(hdr + 18);
    int32_t height_raw = (int32_t)read_le32(hdr + 22);
    uint16_t bpp = read_le16(hdr + 28);
    bool top_down = (height_raw < 0);
    int32_t height = top_down ? -height_raw : height_raw;

    if (width <= 0 || height <= 0 || (bpp != 24 && bpp != 32)) {
        fclose(fp);
        return false;
    }

    int bytes_per_pixel = bpp / 8;
    size_t row_stride = ((size_t)width * bytes_per_pixel + 3U) & ~3U;
    uint8_t *row_buf = (uint8_t *)malloc(row_stride);
    if (!row_buf) {
        fclose(fp);
        return false;
    }

    fseek(fp, (long)off_bits, SEEK_SET);
    int off_x = (RGDS_NATIVE_W - width) / 2;
    int off_y = (RGDS_NATIVE_H - height) / 2;

    for (int row = 0; row < height; row++) {
        if (fread(row_buf, 1, row_stride, fp) != row_stride) break;
        int dst_y = off_y + (top_down ? row : (height - 1 - row));
        if (dst_y < 0 || dst_y >= RGDS_NATIVE_H) continue;
        for (int col = 0; col < width; col++) {
            int dst_x = off_x + col;
            if (dst_x < 0 || dst_x >= RGDS_NATIVE_W) continue;
            uint8_t b = row_buf[col * bytes_per_pixel + 0];
            uint8_t g = row_buf[col * bytes_per_pixel + 1];
            uint8_t r = row_buf[col * bytes_per_pixel + 2];
            out_pixels[dst_y * RGDS_NATIVE_W + dst_x] = RGDS_RGB(r, g, b);
        }
    }

    free(row_buf);
    fclose(fp);
    return true;
}

static uint8_t *read_entire_file(const char *path, size_t *out_size) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return NULL;
    }
    long sz = ftell(fp);
    if (sz <= 0) {
        fclose(fp);
        return NULL;
    }
    fseek(fp, 0, SEEK_SET);
    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) {
        fclose(fp);
        return NULL;
    }
    if (fread(buf, 1, (size_t)sz, fp) != (size_t)sz) {
        free(buf);
        fclose(fp);
        return NULL;
    }
    fclose(fp);
    if (out_size) *out_size = (size_t)sz;
    return buf;
}

static bool write_entire_file(const char *path, const uint8_t *data, size_t size) {
    FILE *fp = fopen(path, "wb");
    if (!fp) return false;
    size_t written = fwrite(data, 1, size, fp);
    fflush(fp);
    int fd = fileno(fp);
    if (fd >= 0) fsync(fd);
    fclose(fp);
    return (written == size);
}

static bool copy_file_binary(const char *src_path, const char *dst_path) {
    size_t sz = 0;
    uint8_t *buf = read_entire_file(src_path, &sz);
    if (!buf) return false;
    bool ok = write_entire_file(dst_path, buf, sz);
    free(buf);
    return ok;
}

/* ============================================================================
 * U-Boot FIT Header & Rockchip RSCE (`resource.img`) Parser and Repacker
 * ============================================================================ */

typedef struct {
    uint32_t totalsize_prop_off;      /* Offset of root `/totalsize` 4B value in FIT hdr */
    uint32_t res_datasize_prop_off;   /* Offset of `/images/resource/data-size` 4B value */
    uint32_t res_datapos_prop_off;    /* Offset of `/images/resource/data-position` 4B value */
    uint32_t res_hash_prop_off;       /* Offset of `/images/resource/hash/value` 32B value */
    uint32_t res_data_pos;
    uint32_t res_data_size;
} fit_resource_info_t;

static bool parse_fit_resource_info(const uint8_t *fit, size_t fit_len, fit_resource_info_t *info) {
    memset(info, 0, sizeof(*info));
    if (fit_len < 64 || read_be32(fit) != 0xd00dfeedUL) {
        return false;
    }
    uint32_t fdt_totalsize = read_be32(fit + 4);
    uint32_t off_dt_struct = read_be32(fit + 8);
    uint32_t off_dt_strings = read_be32(fit + 12);
    uint32_t size_dt_struct = read_be32(fit + 36);

    if (fdt_totalsize > fit_len || off_dt_struct + size_dt_struct > fit_len) {
        return false;
    }

    char node_stack[8][64];
    int depth = 0;
    uint32_t pos = off_dt_struct;

    while (pos + 4 <= off_dt_struct + size_dt_struct) {
        uint32_t token = read_be32(fit + pos);
        pos += 4;
        if (token == 1) { /* FDT_BEGIN_NODE */
            const char *name = (const char *)(fit + pos);
            size_t nlen = strnlen(name, fit_len - pos);
            if (depth < 8) {
                snprintf(node_stack[depth], sizeof(node_stack[depth]), "%s", name);
            }
            depth++;
            pos = (pos + (uint32_t)nlen + 4U) & ~3U;
        } else if (token == 2) { /* FDT_END_NODE */
            if (depth > 0) depth--;
        } else if (token == 3) { /* FDT_PROP */
            if (pos + 8 > fit_len) return false;
            uint32_t plen = read_be32(fit + pos);
            uint32_t nameoff = read_be32(fit + pos + 4);
            pos += 8;
            uint32_t val_off = pos;
            pos = (pos + plen + 3U) & ~3U;
            if (off_dt_strings + nameoff >= fit_len) return false;
            const char *pname = (const char *)(fit + off_dt_strings + nameoff);

            if (depth == 1 && strcmp(pname, "totalsize") == 0 && plen == 4) {
                info->totalsize_prop_off = val_off;
            }
            if (depth == 3 && strcmp(node_stack[1], "images") == 0 &&
                strcmp(node_stack[2], "resource") == 0) {
                if (strcmp(pname, "data-size") == 0 && plen == 4) {
                    info->res_datasize_prop_off = val_off;
                    info->res_data_size = read_be32(fit + val_off);
                } else if (strcmp(pname, "data-position") == 0 && plen == 4) {
                    info->res_datapos_prop_off = val_off;
                    info->res_data_pos = read_be32(fit + val_off);
                }
            }
            if (depth == 4 && strcmp(node_stack[1], "images") == 0 &&
                strcmp(node_stack[2], "resource") == 0 &&
                strcmp(node_stack[3], "hash") == 0) {
                if (strcmp(pname, "value") == 0 && plen == 32) {
                    info->res_hash_prop_off = val_off;
                }
            }
        } else if (token == 4) { /* FDT_NOP */
            continue;
        } else if (token == 9) { /* FDT_END */
            break;
        } else {
            return false;
        }
    }

    return (info->res_datasize_prop_off != 0 &&
            info->res_datapos_prop_off != 0 &&
            info->res_hash_prop_off != 0 &&
            info->res_data_pos != 0 &&
            info->res_data_size != 0);
}

static uint8_t *repack_rsce_container(
    const uint8_t *orig_rsce, size_t orig_rsce_len,
    const uint8_t *new_top_bmp, size_t new_top_len,
    const uint8_t *new_bot_bmp, size_t new_bot_len,
    size_t *out_new_rsce_len,
    char *err_buf, size_t err_buf_sz)
{
    if (orig_rsce_len < 1024 || memcmp(orig_rsce, "RSCE", 4) != 0) {
        snprintf(err_buf, err_buf_sz, "Invalid RSCE magic in resource image");
        return NULL;
    }

    uint8_t header_size_blks = orig_rsce[8];
    uint8_t tbl_offset_blks  = orig_rsce[9];
    uint8_t tbl_ent_blks     = orig_rsce[10];
    uint32_t tbl_entry_num   = read_le32(orig_rsce + 12);

    if (header_size_blks != 1 || tbl_offset_blks != 1 || tbl_ent_blks != 1 ||
        tbl_entry_num == 0 || tbl_entry_num > 64) {
        snprintf(err_buf, err_buf_sz, "Unexpected RSCE table layout (cnt=%u)", tbl_entry_num);
        return NULL;
    }

    uint32_t first_content_blk = read_le32(orig_rsce + 512 + 260);
    if (first_content_blk < 1U + tbl_entry_num) {
        first_content_blk = 1U + tbl_entry_num;
    }

    uint32_t cur_blk = first_content_blk;
    for (uint32_t i = 0; i < tbl_entry_num; i++) {
        const uint8_t *ent = orig_rsce + (size_t)(tbl_offset_blks + i * tbl_ent_blks) * 512U;
        if (memcmp(ent, "ENTR", 4) != 0) {
            snprintf(err_buf, err_buf_sz, "Invalid ENTR tag at index %u", i);
            return NULL;
        }
        const char *fn = (const char *)(ent + 4);
        uint32_t orig_c_off = read_le32(ent + 260);
        uint32_t orig_c_sz  = read_le32(ent + 264);
        if ((size_t)orig_c_off * 512U + orig_c_sz > orig_rsce_len) {
            snprintf(err_buf, err_buf_sz, "Entry %s exceeds RSCE bounds", fn);
            return NULL;
        }
        size_t use_sz = orig_c_sz;
        if (strcmp(fn, "logo.bmp") == 0) {
            use_sz = new_top_len;
        } else if (strcmp(fn, "logo_kernel.bmp") == 0) {
            use_sz = new_bot_len;
        }
        uint32_t blks = (uint32_t)((use_sz + 511U) / 512U);
        cur_blk += blks;
    }

    uint32_t total_blks = (cur_blk + 3U) & ~3U;
    size_t new_rsce_len = (size_t)total_blks * 512U;
    uint8_t *new_rsce = (uint8_t *)calloc(1, new_rsce_len);
    if (!new_rsce) {
        snprintf(err_buf, err_buf_sz, "Out of memory allocating %zu bytes", new_rsce_len);
        return NULL;
    }

    memcpy(new_rsce, orig_rsce, 512);

    bool found_logo = false;
    bool found_logo_kernel = false;
    cur_blk = first_content_blk;

    for (uint32_t i = 0; i < tbl_entry_num; i++) {
        const uint8_t *orig_ent = orig_rsce + (size_t)(tbl_offset_blks + i * tbl_ent_blks) * 512U;
        uint8_t *new_ent = new_rsce + (size_t)(tbl_offset_blks + i * tbl_ent_blks) * 512U;
        memcpy(new_ent, orig_ent, 512);

        const char *fn = (const char *)(orig_ent + 4);
        uint32_t orig_c_off = read_le32(orig_ent + 260);
        uint32_t orig_c_sz  = read_le32(orig_ent + 264);

        const uint8_t *src_data = orig_rsce + (size_t)orig_c_off * 512U;
        size_t src_len = orig_c_sz;

        if (strcmp(fn, "logo.bmp") == 0) {
            src_data = new_top_bmp;
            src_len = new_top_len;
            found_logo = true;
        } else if (strcmp(fn, "logo_kernel.bmp") == 0) {
            src_data = new_bot_bmp;
            src_len = new_bot_len;
            found_logo_kernel = true;
        }

        write_le32(new_ent + 260, cur_blk);
        write_le32(new_ent + 264, (uint32_t)src_len);
        memcpy(new_rsce + (size_t)cur_blk * 512U, src_data, src_len);

        uint32_t blks = (uint32_t)((src_len + 511U) / 512U);
        cur_blk += blks;
    }

    if (!found_logo || !found_logo_kernel) {
        free(new_rsce);
        snprintf(err_buf, err_buf_sz, "Missing logo.bmp or logo_kernel.bmp in RSCE");
        return NULL;
    }

    *out_new_rsce_len = new_rsce_len;
    return new_rsce;
}

/* ============================================================================
 * Desktop Simulation Environment Helper (`#ifdef HOST_X11`)
 * ============================================================================ */

static const char *get_boot_part_path(void) {
    const char *env_boot = getenv("BOOT_LOGO_BOOT_DEV");
    if (env_boot && env_boot[0]) return env_boot;

#ifdef HOST_X11
    return "/tmp/rgds_boot_logo_sim/sim_boot.img";
#else
    if (access("/dev/disk/by-partlabel/boot", R_OK | W_OK) == 0) {
        return "/dev/disk/by-partlabel/boot";
    }
    FILE *fp = fopen("/sys/class/anbernic_misc/boot_device", "r");
    int sd_boot = 1;
    if (fp) {
        char buf[16] = {0};
        if (fgets(buf, sizeof(buf), fp)) sd_boot = atoi(buf);
        fclose(fp);
    }
    return (sd_boot == 1) ? "/dev/mmcblk1p3" : "/dev/mmcblk0p3";
#endif
}

static const char *get_vendor_root_path(void) {
    const char *env_vendor = getenv("BOOT_LOGO_VENDOR_DIR");
    if (env_vendor && env_vendor[0]) return env_vendor;
#ifdef HOST_X11
    return "/tmp/rgds_boot_logo_sim/vendor";
#else
    return "/mnt/vendor";
#endif
}

#ifdef HOST_X11
static void ensure_desktop_sim_env(void) {
    mkdir("/tmp/rgds_boot_logo_sim", 0755);
    mkdir("/tmp/rgds_boot_logo_sim/vendor", 0755);
    mkdir("/tmp/rgds_boot_logo_sim/vendor/resource", 0755);
    mkdir("/tmp/rgds_boot_logo_sim/vendor/resource/res1", 0755);
    mkdir("/tmp/rgds_boot_logo_sim/vendor/resource/res1/boot", 0755);
    mkdir("/tmp/rgds_boot_logo_sim/vendor/resource/res2", 0755);
    mkdir("/tmp/rgds_boot_logo_sim/vendor/resource/res2/boot", 0755);

    const char *sim_boot = "/tmp/rgds_boot_logo_sim/sim_boot.img";
    if (access(sim_boot, R_OK | W_OK) == 0) {
        return;
    }

    const char *fw_img = "../../ROM/RG-DS-PLUS-EN16GB-V1.0-20260915.IMG";
    if (access(fw_img, R_OK) != 0) {
        fw_img = "/usr/local/google/home/viki/xkdr/a/ROM/RG-DS-PLUS-EN16GB-V1.0-20260915.IMG";
    }
    FILE *in_fp = fopen(fw_img, "rb");
    if (in_fp) {
        uint8_t fit_hdr[2048];
        fseeko(in_fp, (off_t)32768 * 512, SEEK_SET);
        if (fread(fit_hdr, 1, sizeof(fit_hdr), in_fp) == sizeof(fit_hdr)) {
            fit_resource_info_t info;
            if (parse_fit_resource_info(fit_hdr, sizeof(fit_hdr), &info)) {
                uint32_t sim_res_pos = 0x1000;
                write_be32(fit_hdr + info.res_datapos_prop_off, sim_res_pos);
                write_be32(fit_hdr + info.totalsize_prop_off, sim_res_pos + info.res_data_size);

                uint8_t *rsce_buf = (uint8_t *)malloc(info.res_data_size);
                if (rsce_buf) {
                    fseeko(in_fp, (off_t)32768 * 512 + (off_t)info.res_data_pos, SEEK_SET);
                    if (fread(rsce_buf, 1, info.res_data_size, in_fp) == info.res_data_size) {
                        FILE *out_fp = fopen(sim_boot, "wb");
                        if (out_fp) {
                            fwrite(fit_hdr, 1, sizeof(fit_hdr), out_fp);
                            fseek(out_fp, (long)sim_res_pos, SEEK_SET);
                            fwrite(rsce_buf, 1, info.res_data_size, out_fp);
                            fclose(out_fp);
                        }
                    }
                    free(rsce_buf);
                }
            }
        }
        fclose(in_fp);
    }
}
#endif

/* ============================================================================
 * Flash / Apply Boot Logo (Stage 1 FIT+RSCE + Stage 2 Vendor PNGs)
 * ============================================================================ */

static bool flash_boot_logos(
    int top_theme_idx, int bot_theme_idx,
    char *status_msg, size_t status_msg_sz,
    char *detail_msg, size_t detail_msg_sz)
{
#ifdef HOST_X11
    ensure_desktop_sim_env();
#endif

    const logo_theme_t *top_t = &g_themes[top_theme_idx];
    const logo_theme_t *bot_t = &g_themes[bot_theme_idx];

    const char *top_bmp_file = top_t->is_factory_stock ? "logos/factory_logo.bmp" : top_t->top_bmp_path;
    const char *bot_bmp_file = bot_t->is_factory_stock ? "logos/factory_logo_kernel.bmp" : bot_t->bot_bmp_path;

    size_t top_bmp_len = 0, bot_bmp_len = 0;
    uint8_t *top_bmp_data = read_entire_file(top_bmp_file, &top_bmp_len);
    uint8_t *bot_bmp_data = read_entire_file(bot_bmp_file, &bot_bmp_len);

    if (!top_bmp_data || !bot_bmp_data) {
        free(top_bmp_data);
        free(bot_bmp_data);
        snprintf(status_msg, status_msg_sz, "ERROR: MISSING BMP ASSETS");
        snprintf(detail_msg, detail_msg_sz, "Could not read %s or %s", top_bmp_file, bot_bmp_file);
        return false;
    }

    const char *boot_dev = get_boot_part_path();
    int fd = open(boot_dev, O_RDWR);
    if (fd < 0) {
        free(top_bmp_data);
        free(bot_bmp_data);
        snprintf(status_msg, status_msg_sz, "ERROR: CANNOT OPEN BOOT DEV");
        snprintf(detail_msg, detail_msg_sz, "%s: %s", boot_dev, strerror(errno));
        return false;
    }

    uint8_t fit_hdr[4096];
    if (pread(fd, fit_hdr, sizeof(fit_hdr), 0) != (ssize_t)sizeof(fit_hdr)) {
        close(fd);
        free(top_bmp_data);
        free(bot_bmp_data);
        snprintf(status_msg, status_msg_sz, "ERROR: FIT HEADER READ FAILED");
        snprintf(detail_msg, detail_msg_sz, "Failed reading FIT header from %s", boot_dev);
        return false;
    }

    fit_resource_info_t fit_info;
    if (!parse_fit_resource_info(fit_hdr, sizeof(fit_hdr), &fit_info)) {
        close(fd);
        free(top_bmp_data);
        free(bot_bmp_data);
        snprintf(status_msg, status_msg_sz, "ERROR: INVALID FIT IMAGE");
        snprintf(detail_msg, detail_msg_sz, "Could not locate /images/resource in %s", boot_dev);
        return false;
    }

    uint8_t *orig_rsce = (uint8_t *)malloc(fit_info.res_data_size);
    if (!orig_rsce || pread(fd, orig_rsce, fit_info.res_data_size, (off_t)fit_info.res_data_pos) != (ssize_t)fit_info.res_data_size) {
        free(orig_rsce);
        close(fd);
        free(top_bmp_data);
        free(bot_bmp_data);
        snprintf(status_msg, status_msg_sz, "ERROR: RSCE READ FAILED");
        snprintf(detail_msg, detail_msg_sz, "Could not read RSCE at 0x%x", fit_info.res_data_pos);
        return false;
    }

    mkdir("backup", 0755);
    if (access("backup/orig_resource.bin", F_OK) != 0) {
        write_entire_file("backup/orig_fit_hdr.bin", fit_hdr, sizeof(fit_hdr));
        write_entire_file("backup/orig_resource.bin", orig_rsce, fit_info.res_data_size);
    }

    char err_buf[128] = {0};
    size_t new_rsce_len = 0;
    uint8_t *new_rsce = repack_rsce_container(
        orig_rsce, fit_info.res_data_size,
        top_bmp_data, top_bmp_len,
        bot_bmp_data, bot_bmp_len,
        &new_rsce_len, err_buf, sizeof(err_buf));

    free(orig_rsce);
    free(top_bmp_data);
    free(bot_bmp_data);

    if (!new_rsce) {
        close(fd);
        snprintf(status_msg, status_msg_sz, "ERROR: RSCE REPACK FAILED");
        snprintf(detail_msg, detail_msg_sz, "%s", err_buf);
        return false;
    }

    uint8_t sha256_digest[32];
    sha256_compute(new_rsce, new_rsce_len, sha256_digest);

    write_be32(fit_hdr + fit_info.res_datasize_prop_off, (uint32_t)new_rsce_len);
    if (fit_info.totalsize_prop_off != 0) {
        uint32_t new_totalsize = fit_info.res_data_pos + (uint32_t)new_rsce_len;
        write_be32(fit_hdr + fit_info.totalsize_prop_off, new_totalsize);
    }
    memcpy(fit_hdr + fit_info.res_hash_prop_off, sha256_digest, 32);

    ssize_t w_rsce = pwrite(fd, new_rsce, new_rsce_len, (off_t)fit_info.res_data_pos);
    ssize_t w_fit  = pwrite(fd, fit_hdr, sizeof(fit_hdr), 0);
    fsync(fd);

    if (w_rsce != (ssize_t)new_rsce_len || w_fit != (ssize_t)sizeof(fit_hdr)) {
        free(new_rsce);
        close(fd);
        snprintf(status_msg, status_msg_sz, "ERROR: WRITE TO BOOT FAILED");
        snprintf(detail_msg, detail_msg_sz, "Wrote %zd/%zu bytes to %s", w_rsce, new_rsce_len, boot_dev);
        return false;
    }

    uint8_t *verify_buf = (uint8_t *)malloc(new_rsce_len);
    bool verified = false;
    if (verify_buf && pread(fd, verify_buf, new_rsce_len, (off_t)fit_info.res_data_pos) == (ssize_t)new_rsce_len) {
        uint8_t verify_hash[32];
        sha256_compute(verify_buf, new_rsce_len, verify_hash);
        if (memcmp(verify_hash, sha256_digest, 32) == 0) {
            verified = true;
        }
    }
    free(verify_buf);
    free(new_rsce);
    close(fd);

    if (!verified) {
        snprintf(status_msg, status_msg_sz, "ERROR: SHA256 VERIFY FAILED");
        snprintf(detail_msg, detail_msg_sz, "Read-back verification failed on %s", boot_dev);
        return false;
    }

    const char *vendor_root = get_vendor_root_path();
    const char *res_dirs[2] = {"res1", "res2"};
    int vendor_updated = 0;
    for (int i = 0; i < 2; i++) {
        char p1[256], p2[256];
        snprintf(p1, sizeof(p1), "%s/resource/%s/boot/logo.png", vendor_root, res_dirs[i]);
        snprintf(p2, sizeof(p2), "%s/resource/%s/boot/logo2.png", vendor_root, res_dirs[i]);
        if (i == 0 && access("backup/orig_vendor_logo.png", F_OK) != 0) {
            copy_file_binary(p1, "backup/orig_vendor_logo.png");
            copy_file_binary(p2, "backup/orig_vendor_logo2.png");
        }
        if (copy_file_binary(top_t->top_png_path, p1) &&
            copy_file_binary(bot_t->bot_png_path, p2)) {
            vendor_updated++;
        }
    }
    sync();

    snprintf(status_msg, status_msg_sz, "FLASHED & VERIFIED OK!");
    snprintf(detail_msg, detail_msg_sz,
             "RSCE %u KB | SHA256 %02x%02x%02x%02x.. | Vendor res1/res2 (%d/2) updated",
             (unsigned)(new_rsce_len / 1024U),
             sha256_digest[0], sha256_digest[1], sha256_digest[2], sha256_digest[3],
             vendor_updated);
    return true;
}

/* ============================================================================
 * Native 1024x768 UI Drawing Helpers
 * ============================================================================ */

static void draw_alpha_rect(rgds_screen_t scr, int rx, int ry, int rw, int rh, uint32_t rgb, int alpha_255) {
    uint32_t *fb = rgds_get_native_buffer(scr);
    if (!fb) return;
    uint32_t sr = (rgb >> 16) & 0xFF;
    uint32_t sg = (rgb >> 8) & 0xFF;
    uint32_t sb = rgb & 0xFF;
    uint32_t inv_a = 255U - (uint32_t)alpha_255;

    int x0 = (rx < 0) ? 0 : rx;
    int y0 = (ry < 0) ? 0 : ry;
    int x1 = (rx + rw > RGDS_NATIVE_W) ? RGDS_NATIVE_W : (rx + rw);
    int y1 = (ry + rh > RGDS_NATIVE_H) ? RGDS_NATIVE_H : (ry + rh);

    for (int y = y0; y < y1; y++) {
        uint32_t *row = fb + y * RGDS_NATIVE_W;
        for (int x = x0; x < x1; x++) {
            uint32_t dst = row[x];
            uint32_t dr = (dst >> 16) & 0xFF;
            uint32_t dg = (dst >> 8) & 0xFF;
            uint32_t db = dst & 0xFF;
            uint32_t r = (sr * (uint32_t)alpha_255 + dr * inv_a) / 255U;
            uint32_t g = (sg * (uint32_t)alpha_255 + dg * inv_a) / 255U;
            uint32_t b = (sb * (uint32_t)alpha_255 + db * inv_a) / 255U;
            row[x] = RGDS_RGB(r, g, b);
        }
    }
}

static void draw_centered_text(rgds_screen_t scr, int center_x, int y, const char *text, int scale, uint32_t color) {
    int w = rgds_text_width(text, scale);
    rgds_draw_text(scr, center_x - w / 2, y, text, scale, color);
}

static void blit_full_screen(rgds_screen_t scr, const uint32_t *pixels) {
    uint32_t *fb = rgds_get_native_buffer(scr);
    if (!fb || !pixels) return;
    memcpy(fb, pixels, (size_t)RGDS_NATIVE_W * RGDS_NATIVE_H * sizeof(uint32_t));
}

static void blit_scaled_preview(
    rgds_screen_t scr,
    const uint32_t *src_pixels,
    int dx, int dy, int dw, int dh)
{
    uint32_t *fb = rgds_get_native_buffer(scr);
    if (!fb || !src_pixels) return;
    for (int y = 0; y < dh; y++) {
        int py = dy + y;
        if (py < 0 || py >= RGDS_NATIVE_H) continue;
        int sy = (y * RGDS_NATIVE_H) / dh;
        const uint32_t *src_row = src_pixels + sy * RGDS_NATIVE_W;
        uint32_t *dst_row = fb + py * RGDS_NATIVE_W;
        for (int x = 0; x < dw; x++) {
            int px = dx + x;
            if (px < 0 || px >= RGDS_NATIVE_W) continue;
            int sx = (x * RGDS_NATIVE_W) / dw;
            dst_row[px] = src_row[sx];
        }
    }
}

typedef enum {
    MODAL_NONE = 0,
    MODAL_CONFIRM_FLASH,
    MODAL_CONFIRM_STOCK,
    MODAL_RESULT
} ui_modal_t;

static void render_ui_frame(
    int top_idx, int bot_idx, bool full_preview_mode,
    ui_modal_t modal, bool last_flash_ok,
    const char *status_headline, const char *status_detail)
{
    /* ========================================================================
     * TOP SCREEN (1024x768)
     * ======================================================================== */
    blit_full_screen(RGDS_SCREEN_TOP, g_themes[top_idx].top_pixels);

    if (!full_preview_mode) {
        draw_alpha_rect(RGDS_SCREEN_TOP, 0, 0, RGDS_NATIVE_W, 56, RGDS_RGB(12, 10, 24), 205);
        rgds_fill_rect(RGDS_SCREEN_TOP, 0, 56, RGDS_NATIVE_W, 3, RGDS_RGB(255, 205, 45));

        char top_hdr[96];
        snprintf(top_hdr, sizeof(top_hdr), "TOP BOOT LOGO  [%d/%d]: %s",
                 top_idx + 1, NUM_THEMES, g_themes[top_idx].title);
        rgds_draw_text(RGDS_SCREEN_TOP, 24, 18, top_hdr, 3, RGDS_RGB(255, 230, 90));
        rgds_draw_text(RGDS_SCREEN_TOP, 810, 22, "[Y/R] FULL VIEW", 2, RGDS_RGB(160, 225, 255));
    }

    /* ========================================================================
     * BOTTOM SCREEN (1024x768)
     * ======================================================================== */
    if (full_preview_mode && modal == MODAL_NONE) {
        blit_full_screen(RGDS_SCREEN_BOTTOM, g_themes[bot_idx].bot_pixels);
    } else {
        rgds_clear(RGDS_SCREEN_BOTTOM, RGDS_RGB(16, 14, 30));

        /* Header bar with Quit Help */
        rgds_fill_rect(RGDS_SCREEN_BOTTOM, 0, 0, RGDS_NATIVE_W, 60, RGDS_RGB(28, 22, 52));
        rgds_fill_rect(RGDS_SCREEN_BOTTOM, 0, 60, RGDS_NATIVE_W, 3, RGDS_RGB(100, 210, 255));
        rgds_draw_text(RGDS_SCREEN_BOTTOM, 28, 19, "RG DS PLUS BOOT LOGO CUSTOMIZER", 3, RGDS_RGB(255, 225, 80));
        rgds_fill_rect(RGDS_SCREEN_BOTTOM, 696, 12, 304, 36, RGDS_RGB(56, 32, 68));
        rgds_draw_rect(RGDS_SCREEN_BOTTOM, 696, 12, 304, 36, 2, RGDS_RGB(255, 150, 175));
        draw_centered_text(RGDS_SCREEN_BOTTOM, 848, 23, "[START / SELECT] QUIT", 2, RGDS_RGB(255, 225, 235));

        /* Large 512x384 Bottom Screen Preview in the center with neon frame */
        int pv_w = 512, pv_h = 384;
        int pv_x = (RGDS_NATIVE_W - pv_w) / 2;
        int pv_y = 82;
        rgds_draw_rect(RGDS_SCREEN_BOTTOM, pv_x - 6, pv_y - 6, pv_w + 12, pv_h + 12, 4, RGDS_RGB(100, 215, 255));
        blit_scaled_preview(RGDS_SCREEN_BOTTOM, g_themes[bot_idx].bot_pixels, pv_x, pv_y, pv_w, pv_h);

        /* Left panel: Theme Presets (x = 20..236, y = 82..466) */
        rgds_fill_rect(RGDS_SCREEN_BOTTOM, 20, 82, 216, 384, RGDS_RGB(24, 20, 44));
        rgds_draw_rect(RGDS_SCREEN_BOTTOM, 20, 82, 216, 384, 3, RGDS_RGB(70, 60, 115));
        rgds_draw_text(RGDS_SCREEN_BOTTOM, 36, 94, "THEME PRESETS", 2, RGDS_RGB(255, 215, 70));

        for (int i = 0; i < NUM_THEMES; i++) {
            int cy = 120 + i * 67;
            bool active = (top_idx == i || bot_idx == i);
            uint32_t bg = active ? RGDS_RGB(52, 42, 96) : RGDS_RGB(30, 26, 54);
            uint32_t brd = (top_idx == i && bot_idx == i) ? RGDS_RGB(255, 215, 60) :
                           (active ? RGDS_RGB(100, 220, 255) : RGDS_RGB(55, 48, 88));
            rgds_fill_rect(RGDS_SCREEN_BOTTOM, 32, cy, 192, 58, bg);
            rgds_draw_rect(RGDS_SCREEN_BOTTOM, 32, cy, 192, 58, 3, brd);

            char num_lbl[32];
            snprintf(num_lbl, sizeof(num_lbl), "#%d %s", i + 1,
                     (i == 0) ? "KISS" :
                     (i == 1) ? "CHEF STEW" :
                     (i == 2) ? "TUX SKY" :
                     (i == 3) ? "POETRY" : "STOCK OEM");
            rgds_draw_text(RGDS_SCREEN_BOTTOM, 44, cy + 12, num_lbl, 2, RGDS_RGB(245, 245, 255));

            const char *badge = (top_idx == i && bot_idx == i) ? "ACTIVE: TOP + BOT" :
                                (top_idx == i) ? "ACTIVE: TOP ONLY" :
                                (bot_idx == i) ? "ACTIVE: BOT ONLY" : "";
            if (badge[0]) {
                rgds_draw_text(RGDS_SCREEN_BOTTOM, 44, cy + 36, badge, 1, RGDS_RGB(120, 255, 160));
            }
        }

        /* Right panel: Technical Boot Targets (x = 788..1004, y = 82..466) */
        rgds_fill_rect(RGDS_SCREEN_BOTTOM, 788, 82, 216, 384, RGDS_RGB(24, 20, 44));
        rgds_draw_rect(RGDS_SCREEN_BOTTOM, 788, 82, 216, 384, 3, RGDS_RGB(70, 60, 115));
        rgds_draw_text(RGDS_SCREEN_BOTTOM, 804, 98, "BOOT TARGETS", 2, RGDS_RGB(110, 225, 255));

        rgds_draw_text(RGDS_SCREEN_BOTTOM, 804, 136, "STAGE 1 BOOT:", 2, RGDS_RGB(255, 210, 80));
        rgds_draw_text(RGDS_SCREEN_BOTTOM, 804, 162, "U-Boot + Kernel DRM", 1, RGDS_RGB(220, 220, 240));
        rgds_draw_text(RGDS_SCREEN_BOTTOM, 804, 180, "Rockchip RSCE BMP", 1, RGDS_RGB(170, 175, 205));
        rgds_draw_text(RGDS_SCREEN_BOTTOM, 804, 198, "Top: logo.bmp", 1, RGDS_RGB(130, 235, 170));
        rgds_draw_text(RGDS_SCREEN_BOTTOM, 804, 216, "Bot: logo_kernel.bmp", 1, RGDS_RGB(130, 235, 170));

        rgds_draw_text(RGDS_SCREEN_BOTTOM, 804, 252, "STAGE 2 VENDOR:", 2, RGDS_RGB(255, 210, 80));
        rgds_draw_text(RGDS_SCREEN_BOTTOM, 804, 278, "dmenu / muos2 Splash", 1, RGDS_RGB(220, 220, 240));
        rgds_draw_text(RGDS_SCREEN_BOTTOM, 804, 296, "/mnt/vendor/resource", 1, RGDS_RGB(170, 175, 205));
        rgds_draw_text(RGDS_SCREEN_BOTTOM, 804, 314, "res1 & res2 PNGs", 1, RGDS_RGB(130, 235, 170));

        rgds_draw_text(RGDS_SCREEN_BOTTOM, 804, 352, "VERIFICATION:", 2, RGDS_RGB(255, 140, 180));
        rgds_draw_text(RGDS_SCREEN_BOTTOM, 804, 378, "FIT SHA256 Digest", 1, RGDS_RGB(220, 220, 240));
        rgds_draw_text(RGDS_SCREEN_BOTTOM, 804, 396, "+ Read-back Check", 1, RGDS_RGB(220, 220, 240));
        rgds_draw_text(RGDS_SCREEN_BOTTOM, 804, 414, "+ Auto Backup", 1, RGDS_RGB(130, 235, 170));

        /* Subtitle & Status Strip (y = 484..574) */
        rgds_fill_rect(RGDS_SCREEN_BOTTOM, 20, 484, 984, 90, RGDS_RGB(24, 22, 46));
        rgds_draw_rect(RGDS_SCREEN_BOTTOM, 20, 484, 984, 90, 3, RGDS_RGB(85, 75, 135));

        char bot_lbl[96];
        snprintf(bot_lbl, sizeof(bot_lbl), "BOTTOM BOOT LOGO  [%d/%d]: %s",
                 bot_idx + 1, NUM_THEMES, g_themes[bot_idx].title);
        draw_centered_text(RGDS_SCREEN_BOTTOM, 512, 502, bot_lbl, 3, RGDS_RGB(115, 230, 255));
        draw_centered_text(RGDS_SCREEN_BOTTOM, 512, 542, g_themes[bot_idx].subtitle, 2, RGDS_RGB(210, 210, 230));

        /* Interactive Button Row 1 (y = 590..662) */
        rgds_fill_rect(RGDS_SCREEN_BOTTOM, 28, 590, 196, 72, RGDS_RGB(42, 36, 78));
        rgds_draw_rect(RGDS_SCREEN_BOTTOM, 28, 590, 196, 72, 3, RGDS_RGB(130, 120, 195));
        draw_centered_text(RGDS_SCREEN_BOTTOM, 126, 616, "[UP] PREV", 3, RGDS_RGB(255, 255, 255));

        rgds_fill_rect(RGDS_SCREEN_BOTTOM, 244, 590, 536, 72, RGDS_RGB(32, 115, 68));
        rgds_draw_rect(RGDS_SCREEN_BOTTOM, 244, 590, 536, 72, 3, RGDS_RGB(110, 255, 160));
        draw_centered_text(RGDS_SCREEN_BOTTOM, 512, 616, "[A] FLASH BOOT LOGO", 3, RGDS_RGB(255, 255, 255));

        rgds_fill_rect(RGDS_SCREEN_BOTTOM, 800, 590, 196, 72, RGDS_RGB(42, 36, 78));
        rgds_draw_rect(RGDS_SCREEN_BOTTOM, 800, 590, 196, 72, 3, RGDS_RGB(130, 120, 195));
        draw_centered_text(RGDS_SCREEN_BOTTOM, 898, 616, "[DN] NEXT", 3, RGDS_RGB(255, 255, 255));

        /* Interactive Button Row 2 (y = 678..744) */
        rgds_fill_rect(RGDS_SCREEN_BOTTOM, 28, 678, 308, 66, RGDS_RGB(36, 52, 88));
        rgds_draw_rect(RGDS_SCREEN_BOTTOM, 28, 678, 308, 66, 3, RGDS_RGB(100, 195, 255));
        draw_centered_text(RGDS_SCREEN_BOTTOM, 182, 704, "[Y/R] 1:1 DUAL PREVIEW", 2, RGDS_RGB(220, 245, 255));

        rgds_fill_rect(RGDS_SCREEN_BOTTOM, 358, 678, 308, 66, RGDS_RGB(56, 38, 86));
        rgds_draw_rect(RGDS_SCREEN_BOTTOM, 358, 678, 308, 66, 3, RGDS_RGB(215, 140, 255));
        draw_centered_text(RGDS_SCREEN_BOTTOM, 512, 704, "[L1/R1] MIX   [X] SWAP", 2, RGDS_RGB(245, 225, 255));

        rgds_fill_rect(RGDS_SCREEN_BOTTOM, 688, 678, 308, 66, RGDS_RGB(82, 36, 48));
        rgds_draw_rect(RGDS_SCREEN_BOTTOM, 688, 678, 308, 66, 3, RGDS_RGB(255, 130, 145));
        draw_centered_text(RGDS_SCREEN_BOTTOM, 842, 704, "[B] RESTORE STOCK", 2, RGDS_RGB(255, 230, 235));
    }

    /* ========================================================================
     * Modal Dialog Overlay on Bottom Screen
     * ======================================================================== */
    if (modal != MODAL_NONE) {
        draw_alpha_rect(RGDS_SCREEN_BOTTOM, 0, 0, RGDS_NATIVE_W, RGDS_NATIVE_H, RGDS_RGB(6, 6, 14), 215);
        int mx = 96, my = 180, mw = 832, mh = 380;
        rgds_fill_rect(RGDS_SCREEN_BOTTOM, mx, my, mw, mh, RGDS_RGB(28, 24, 54));

        if (modal == MODAL_CONFIRM_FLASH) {
            rgds_draw_rect(RGDS_SCREEN_BOTTOM, mx, my, mw, mh, 4, RGDS_RGB(110, 255, 160));
            draw_centered_text(RGDS_SCREEN_BOTTOM, 512, my + 40, "FLASH CUSTOM BOOT LOGO?", 3, RGDS_RGB(110, 255, 160));

            char line1[96], line2[96];
            snprintf(line1, sizeof(line1), "Top Screen:    %s", g_themes[top_idx].title);
            snprintf(line2, sizeof(line2), "Bottom Screen: %s", g_themes[bot_idx].title);
            draw_centered_text(RGDS_SCREEN_BOTTOM, 512, my + 115, line1, 3, RGDS_RGB(255, 235, 120));
            draw_centered_text(RGDS_SCREEN_BOTTOM, 512, my + 165, line2, 3, RGDS_RGB(120, 230, 255));
            draw_centered_text(RGDS_SCREEN_BOTTOM, 512, my + 230,
                               "Updates Stage 1 (boot FIT/RSCE) + Stage 2 (/mnt/vendor)",
                               2, RGDS_RGB(200, 200, 220));
            draw_centered_text(RGDS_SCREEN_BOTTOM, 512, my + 305,
                               "Press [A] to Confirm & Flash   |   Press [B] to Cancel",
                               2, RGDS_RGB(255, 255, 255));
        } else if (modal == MODAL_CONFIRM_STOCK) {
            rgds_draw_rect(RGDS_SCREEN_BOTTOM, mx, my, mw, mh, 4, RGDS_RGB(255, 150, 120));
            draw_centered_text(RGDS_SCREEN_BOTTOM, 512, my + 44, "RESTORE FACTORY BOOT LOGO?", 3, RGDS_RGB(255, 180, 120));
            draw_centered_text(RGDS_SCREEN_BOTTOM, 512, my + 130,
                               "Restores the original Anbernic factory logo.bmp,",
                               2, RGDS_RGB(235, 235, 245));
            draw_centered_text(RGDS_SCREEN_BOTTOM, 512, my + 165,
                               "logo_kernel.bmp, and /mnt/vendor/resource boot PNGs.",
                               2, RGDS_RGB(235, 235, 245));
            draw_centered_text(RGDS_SCREEN_BOTTOM, 512, my + 300,
                               "Press [A] to Restore Stock   |   Press [B] to Cancel",
                               2, RGDS_RGB(255, 255, 255));
        } else if (modal == MODAL_RESULT) {
            uint32_t col = last_flash_ok ? RGDS_RGB(100, 255, 160) : RGDS_RGB(255, 100, 110);
            rgds_draw_rect(RGDS_SCREEN_BOTTOM, mx, my, mw, mh, 4, col);
            draw_centered_text(RGDS_SCREEN_BOTTOM, 512, my + 48, status_headline, 3, col);
            draw_centered_text(RGDS_SCREEN_BOTTOM, 512, my + 140, status_detail, 2, RGDS_RGB(240, 240, 255));
            draw_centered_text(RGDS_SCREEN_BOTTOM, 512, my + 205,
                               "Original factory backup saved in ./backup/",
                               2, RGDS_RGB(185, 225, 200));
            draw_centered_text(RGDS_SCREEN_BOTTOM, 512, my + 300,
                               "Press any button to continue",
                               3, RGDS_RGB(255, 230, 110));
        }
    }
}

int main(int argc, char **argv) {
    int dump_promo_mode = 0;
    const char *dump_prefix = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--dump-promo") == 0 && i + 1 < argc) {
            dump_promo_mode = 1;
            dump_prefix = argv[++i];
        }
    }

    if (rgds_init("Boot Logo Customizer") != 0) {
        fprintf(stderr, "Failed to initialize RG DS Plus SDK\n");
        return 1;
    }

    /* Render directly at native 1024x768 resolution on both screens */
    rgds_set_direct_native_mode(true);

    /* Allocate and load all 4 theme BMP pairs into memory */
    for (int i = 0; i < NUM_THEMES; i++) {
        g_themes[i].top_pixels = (uint32_t *)malloc((size_t)RGDS_NATIVE_W * RGDS_NATIVE_H * sizeof(uint32_t));
        g_themes[i].bot_pixels = (uint32_t *)malloc((size_t)RGDS_NATIVE_W * RGDS_NATIVE_H * sizeof(uint32_t));
        bool ok1 = load_bmp_to_argb(g_themes[i].top_bmp_path, g_themes[i].top_pixels);
        bool ok2 = load_bmp_to_argb(g_themes[i].bot_bmp_path, g_themes[i].bot_pixels);
        g_themes[i].loaded = (ok1 && ok2);
    }

    int top_idx = 0;
    int bot_idx = 0;
    bool full_preview_mode = false;
    ui_modal_t modal = MODAL_NONE;
    bool last_flash_ok = true;
    char status_headline[96] = "READY TO CUSTOMIZE BOOT LOGO";
    char status_detail[128]  = "Select a theme with D-Pad Up/Down, then press A to flash.";

    rgds_input_state_t input;
    while (rgds_poll_input(&input)) {
        if (input.pressed & (RGDS_BTN_START | RGDS_BTN_SELECT | RGDS_BTN_MENU)) {
            break;
        }

        if (modal == MODAL_CONFIRM_FLASH) {
            if (input.pressed & RGDS_BTN_A) {
                last_flash_ok = flash_boot_logos(
                    top_idx, bot_idx,
                    status_headline, sizeof(status_headline),
                    status_detail, sizeof(status_detail));
                modal = MODAL_RESULT;
            } else if (input.pressed & (RGDS_BTN_B | RGDS_BTN_X | RGDS_BTN_Y)) {
                modal = MODAL_NONE;
            }
        } else if (modal == MODAL_CONFIRM_STOCK) {
            if (input.pressed & RGDS_BTN_A) {
                top_idx = NUM_THEMES - 1;
                bot_idx = NUM_THEMES - 1;
                last_flash_ok = flash_boot_logos(
                    top_idx, bot_idx,
                    status_headline, sizeof(status_headline),
                    status_detail, sizeof(status_detail));
                modal = MODAL_RESULT;
            } else if (input.pressed & (RGDS_BTN_B | RGDS_BTN_X | RGDS_BTN_Y)) {
                modal = MODAL_NONE;
            }
        } else if (modal == MODAL_RESULT) {
            if (input.pressed != 0 || input.touch.just_pressed) {
                modal = MODAL_NONE;
            }
        } else {
            if (input.pressed & RGDS_BTN_UP) {
                top_idx = (top_idx + NUM_THEMES - 1) % NUM_THEMES;
                bot_idx = top_idx;
            }
            if (input.pressed & RGDS_BTN_DOWN) {
                top_idx = (top_idx + 1) % NUM_THEMES;
                bot_idx = top_idx;
            }
            if (input.pressed & RGDS_BTN_L1) {
                top_idx = (top_idx + 1) % NUM_THEMES;
            }
            if (input.pressed & RGDS_BTN_R1) {
                bot_idx = (bot_idx + 1) % NUM_THEMES;
            }
            if (input.pressed & RGDS_BTN_X) {
                int tmp = top_idx;
                top_idx = bot_idx;
                bot_idx = tmp;
            }
            if (input.pressed & RGDS_BTN_Y) {
                full_preview_mode = !full_preview_mode;
            }
            if (input.pressed & RGDS_BTN_A) {
                modal = MODAL_CONFIRM_FLASH;
            }
            if (input.pressed & RGDS_BTN_B) {
                modal = MODAL_CONFIRM_STOCK;
            }

            /* Touchscreen buttons on Bottom Screen (using native 1024x768 coordinates) */
            if (input.touch.just_pressed) {
                int tx = input.touch.native_x;
                int ty = input.touch.native_y;
                if (full_preview_mode) {
                    full_preview_mode = false;
                } else {
                    /* Preset cards on left (x = 32..224, y = 120 + i*67) */
                    for (int i = 0; i < NUM_THEMES; i++) {
                        int cy = 120 + i * 67;
                        if (tx >= 32 && tx <= 224 && ty >= cy && ty <= cy + 58) {
                            top_idx = i;
                            bot_idx = i;
                        }
                    }
                    /* [< PREV] (28, 590, 196, 72) */
                    if (tx >= 28 && tx <= 224 && ty >= 590 && ty <= 662) {
                        top_idx = (top_idx + NUM_THEMES - 1) % NUM_THEMES;
                        bot_idx = top_idx;
                    }
                    /* [FLASH BOOT LOGO (A)] (244, 590, 536, 72) */
                    else if (tx >= 244 && tx <= 780 && ty >= 590 && ty <= 662) {
                        modal = MODAL_CONFIRM_FLASH;
                    }
                    /* [NEXT >] (800, 590, 196, 72) */
                    else if (tx >= 800 && tx <= 996 && ty >= 590 && ty <= 662) {
                        top_idx = (top_idx + 1) % NUM_THEMES;
                        bot_idx = top_idx;
                    }
                    /* [1:1 DUAL PREVIEW (Y)] (28, 678, 308, 66) */
                    else if (tx >= 28 && tx <= 336 && ty >= 678 && ty <= 744) {
                        full_preview_mode = true;
                    }
                    /* [MIX / SWAP] (358, 678, 308, 66) */
                    else if (tx >= 358 && tx <= 666 && ty >= 678 && ty <= 744) {
                        bot_idx = (bot_idx + 1) % NUM_THEMES;
                    }
                    /* [RESTORE STOCK (B)] (688, 678, 308, 66) */
                    else if (tx >= 688 && tx <= 996 && ty >= 678 && ty <= 744) {
                        modal = MODAL_CONFIRM_STOCK;
                    }
                }
            }
        }

        render_ui_frame(top_idx, bot_idx, full_preview_mode, modal,
                        last_flash_ok, status_headline, status_detail);
        rgds_present();

        /* Optional promo screenshot dump mode */
        if (dump_promo_mode && dump_prefix) {
            struct {
                int t_idx;
                bool full_pv;
                ui_modal_t m_state;
                const char *suffix;
            } shots[5] = {
                {0, false, MODAL_NONE,   "theme_chef_stew.raw"},
                {1, true,  MODAL_NONE,   "theme_chef_stew_b.raw"},
                {2, false, MODAL_NONE,   "theme_tux_sky.raw"},
                {3, false, MODAL_NONE,   "theme_poetry.raw"},
                {0, false, MODAL_RESULT, "flash_success.raw"},
            };
            for (int s = 0; s < 5; s++) {
                top_idx = shots[s].t_idx;
                bot_idx = shots[s].t_idx;
                full_preview_mode = shots[s].full_pv;
                modal = shots[s].m_state;
                if (modal == MODAL_RESULT) {
                    last_flash_ok = flash_boot_logos(
                        top_idx, bot_idx,
                        status_headline, sizeof(status_headline),
                        status_detail, sizeof(status_detail));
                }
                render_ui_frame(top_idx, bot_idx, full_preview_mode, modal,
                                last_flash_ok, status_headline, status_detail);

                char out_fn[256];
                snprintf(out_fn, sizeof(out_fn), "%s_%s", dump_prefix, shots[s].suffix);
                FILE *df = fopen(out_fn, "wb");
                if (df) {
                    fwrite(rgds_get_native_buffer(RGDS_SCREEN_TOP), sizeof(uint32_t), (size_t)RGDS_NATIVE_W * RGDS_NATIVE_H, df);
                    fwrite(rgds_get_native_buffer(RGDS_SCREEN_BOTTOM), sizeof(uint32_t), (size_t)RGDS_NATIVE_W * RGDS_NATIVE_H, df);
                    fclose(df);
                }
            }
            break;
        }

        rgds_delay_ms(16);
    }

    for (int i = 0; i < NUM_THEMES; i++) {
        free(g_themes[i].top_pixels);
        free(g_themes[i].bot_pixels);
    }
    rgds_shutdown();
    return 0;
}
