// GS local memory (4 MB, swizzled) helpers: pixel addressing for every PSM, texel read/write,
// CLUT loading and host->local transfers. Used by the GPU renderer's shadow copy of VRAM.
#pragma once

#include <cstdint>
#include <cstring>

namespace gsmem
{
    constexpr uint32_t kVramSize = 4u * 1024u * 1024u;

    enum Psm : uint32_t
    {
        CT32 = 0x00, CT24 = 0x01, CT16 = 0x02, CT16S = 0x0A, T8 = 0x13, T4 = 0x14,
        T8H = 0x1B, T4HL = 0x24, T4HH = 0x2C, Z32 = 0x30, Z24 = 0x31, Z16 = 0x32, Z16S = 0x3A,
    };

    inline constexpr uint8_t kBlock32[4][8] = {
        {0, 1, 4, 5, 16, 17, 20, 21}, {2, 3, 6, 7, 18, 19, 22, 23},
        {8, 9, 12, 13, 24, 25, 28, 29}, {10, 11, 14, 15, 26, 27, 30, 31}};
    inline constexpr uint8_t kBlockZ32[4][8] = {
        {24, 25, 28, 29, 8, 9, 12, 13}, {26, 27, 30, 31, 10, 11, 14, 15},
        {16, 17, 20, 21, 0, 1, 4, 5}, {18, 19, 22, 23, 2, 3, 6, 7}};
    inline constexpr uint8_t kColumn32[8][8] = {
        {0, 1, 4, 5, 8, 9, 12, 13}, {2, 3, 6, 7, 10, 11, 14, 15},
        {16, 17, 20, 21, 24, 25, 28, 29}, {18, 19, 22, 23, 26, 27, 30, 31},
        {32, 33, 36, 37, 40, 41, 44, 45}, {34, 35, 38, 39, 42, 43, 46, 47},
        {48, 49, 52, 53, 56, 57, 60, 61}, {50, 51, 54, 55, 58, 59, 62, 63}};
    inline constexpr uint8_t kBlock16[8][4] = {
        {0, 2, 8, 10}, {1, 3, 9, 11}, {4, 6, 12, 14}, {5, 7, 13, 15},
        {16, 18, 24, 26}, {17, 19, 25, 27}, {20, 22, 28, 30}, {21, 23, 29, 31}};
    inline constexpr uint8_t kBlock16S[8][4] = {
        {0, 2, 16, 18}, {1, 3, 17, 19}, {8, 10, 24, 26}, {9, 11, 25, 27},
        {4, 6, 20, 22}, {5, 7, 21, 23}, {12, 14, 28, 30}, {13, 15, 29, 31}};
    inline constexpr uint8_t kBlockZ16[8][4] = {
        {24, 26, 16, 18}, {25, 27, 17, 19}, {28, 30, 20, 22}, {29, 31, 21, 23},
        {8, 10, 0, 2}, {9, 11, 1, 3}, {12, 14, 4, 6}, {13, 15, 5, 7}};
    inline constexpr uint8_t kBlockZ16S[8][4] = {
        {24, 26, 8, 10}, {25, 27, 9, 11}, {16, 18, 0, 2}, {17, 19, 1, 3},
        {28, 30, 12, 14}, {29, 31, 13, 15}, {20, 22, 4, 6}, {21, 23, 5, 7}};
    inline constexpr uint8_t kColumn16[2][16] = {
        {0, 2, 8, 10, 16, 18, 24, 26, 1, 3, 9, 11, 17, 19, 25, 27},
        {4, 6, 12, 14, 20, 22, 28, 30, 5, 7, 13, 15, 21, 23, 29, 31}};
    inline constexpr uint8_t kColumn8[16][16] = {
        {0, 4, 16, 20, 32, 36, 48, 52, 2, 6, 18, 22, 34, 38, 50, 54},
        {8, 12, 24, 28, 40, 44, 56, 60, 10, 14, 26, 30, 42, 46, 58, 62},
        {33, 37, 49, 53, 1, 5, 17, 21, 35, 39, 51, 55, 3, 7, 19, 23},
        {41, 45, 57, 61, 9, 13, 25, 29, 43, 47, 59, 63, 11, 15, 27, 31},
        {96, 100, 112, 116, 64, 68, 80, 84, 98, 102, 114, 118, 66, 70, 82, 86},
        {104, 108, 120, 124, 72, 76, 88, 92, 106, 110, 122, 126, 74, 78, 90, 94},
        {65, 69, 81, 85, 97, 101, 113, 117, 67, 71, 83, 87, 99, 103, 115, 119},
        {73, 77, 89, 93, 105, 109, 121, 125, 75, 79, 91, 95, 107, 111, 123, 127},
        {128, 132, 144, 148, 160, 164, 176, 180, 130, 134, 146, 150, 162, 166, 178, 182},
        {136, 140, 152, 156, 168, 172, 184, 188, 138, 142, 154, 158, 170, 174, 186, 190},
        {161, 165, 177, 181, 129, 133, 145, 149, 163, 167, 179, 183, 131, 135, 147, 151},
        {169, 173, 185, 189, 137, 141, 153, 157, 171, 175, 187, 191, 139, 143, 155, 159},
        {224, 228, 240, 244, 192, 196, 208, 212, 226, 230, 242, 246, 194, 198, 210, 214},
        {232, 236, 248, 252, 200, 204, 216, 220, 234, 238, 250, 254, 202, 206, 218, 222},
        {193, 197, 209, 213, 225, 229, 241, 245, 195, 199, 211, 215, 227, 231, 243, 247},
        {201, 205, 217, 221, 233, 237, 249, 253, 203, 207, 219, 223, 235, 239, 251, 255}};
    // PSMT4 column table: nibble offset inside a 512-nibble block.
    inline constexpr uint16_t kColumn4[16][32] = {
        {0, 8, 32, 40, 64, 72, 96, 104, 2, 10, 34, 42, 66, 74, 98, 106, 4, 12, 36, 44, 68, 76, 100, 108, 6, 14, 38, 46, 70, 78, 102, 110},
        {16, 24, 48, 56, 80, 88, 112, 120, 18, 26, 50, 58, 82, 90, 114, 122, 20, 28, 52, 60, 84, 92, 116, 124, 22, 30, 54, 62, 86, 94, 118, 126},
        {65, 73, 97, 105, 1, 9, 33, 41, 67, 75, 99, 107, 3, 11, 35, 43, 69, 77, 101, 109, 5, 13, 37, 45, 71, 79, 103, 111, 7, 15, 39, 47},
        {81, 89, 113, 121, 17, 25, 49, 57, 83, 91, 115, 123, 19, 27, 51, 59, 85, 93, 117, 125, 21, 29, 53, 61, 87, 95, 119, 127, 23, 31, 55, 63},
        {192, 200, 224, 232, 128, 136, 160, 168, 194, 202, 226, 234, 130, 138, 162, 170, 196, 204, 228, 236, 132, 140, 164, 172, 198, 206, 230, 238, 134, 142, 166, 174},
        {208, 216, 240, 248, 144, 152, 176, 184, 210, 218, 242, 250, 146, 154, 178, 186, 212, 220, 244, 252, 148, 156, 180, 188, 214, 222, 246, 254, 150, 158, 182, 190},
        {129, 137, 161, 169, 193, 201, 225, 233, 131, 139, 163, 171, 195, 203, 227, 235, 133, 141, 165, 173, 197, 205, 229, 237, 135, 143, 167, 175, 199, 207, 231, 239},
        {145, 153, 177, 185, 209, 217, 241, 249, 147, 155, 179, 187, 211, 219, 243, 251, 149, 157, 181, 189, 213, 221, 245, 253, 151, 159, 183, 191, 215, 223, 247, 255},
        {256, 264, 288, 296, 320, 328, 352, 360, 258, 266, 290, 298, 322, 330, 354, 362, 260, 268, 292, 300, 324, 332, 356, 364, 262, 270, 294, 302, 326, 334, 358, 366},
        {272, 280, 304, 312, 336, 344, 368, 376, 274, 282, 306, 314, 338, 346, 370, 378, 276, 284, 308, 316, 340, 348, 372, 380, 278, 286, 310, 318, 342, 350, 374, 382},
        {321, 329, 353, 361, 257, 265, 289, 297, 323, 331, 355, 363, 259, 267, 291, 299, 325, 333, 357, 365, 261, 269, 293, 301, 327, 335, 359, 367, 263, 271, 295, 303},
        {337, 345, 369, 377, 273, 281, 305, 313, 339, 347, 371, 379, 275, 283, 307, 315, 341, 349, 373, 381, 277, 285, 309, 317, 343, 351, 375, 383, 279, 287, 311, 319},
        {448, 456, 480, 488, 384, 392, 416, 424, 450, 458, 482, 490, 386, 394, 418, 426, 452, 460, 484, 492, 388, 396, 420, 428, 454, 462, 486, 494, 390, 398, 422, 430},
        {464, 472, 496, 504, 400, 408, 432, 440, 466, 474, 498, 506, 402, 410, 434, 442, 468, 476, 500, 508, 404, 412, 436, 444, 470, 478, 502, 510, 406, 414, 438, 446},
        {385, 393, 417, 425, 449, 457, 481, 489, 387, 395, 419, 427, 451, 459, 483, 491, 389, 397, 421, 429, 453, 461, 485, 493, 391, 399, 423, 431, 455, 463, 487, 495},
        {401, 409, 433, 441, 465, 473, 497, 505, 403, 411, 435, 443, 467, 475, 499, 507, 405, 413, 437, 445, 469, 477, 501, 509, 407, 415, 439, 447, 471, 479, 503, 511}};

    // Byte address of a 32-bit pixel (CT32/CT24/Z32/Z24); bp in 256-byte blocks, bw in 64-pixel units.
    inline uint32_t addr32(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y, bool z = false)
    {
        const uint32_t ppr = bw ? bw : 1u;
        const uint32_t page = (bp >> 5) + (y >> 5) * ppr + (x >> 6);
        const uint32_t blk = (bp & 31u) + (z ? kBlockZ32 : kBlock32)[(y >> 3) & 3][(x >> 3) & 7];
        return ((page << 13) + ((blk >> 5) << 13) + (blk & 31u) * 256u + kColumn32[y & 7][x & 7] * 4u) & (kVramSize - 1u);
    }

    inline uint32_t addr16(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y, const uint8_t (&table)[8][4])
    {
        const uint32_t ppr = bw ? bw : 1u;
        const uint32_t page = (bp >> 5) + (y >> 6) * ppr + (x >> 6);
        const uint32_t blk = (bp & 31u) + table[(y >> 3) & 7][(x >> 4) & 3];
        return ((page << 13) + ((blk >> 5) << 13) + (blk & 31u) * 256u + ((y >> 1) & 3u) * 64u +
                kColumn16[y & 1][x & 15] * 2u) & (kVramSize - 1u);
    }

    inline uint32_t addr8(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y)
    {
        const uint32_t ppr = (bw >> 1) ? (bw >> 1) : 1u;
        const uint32_t page = (bp >> 5) + (y >> 6) * ppr + (x >> 7);
        const uint32_t blk = (bp & 31u) + kBlock32[(y >> 4) & 3][(x >> 4) & 7];
        return ((page << 13) + ((blk >> 5) << 13) + (blk & 31u) * 256u + kColumn8[y & 15][x & 15]) & (kVramSize - 1u);
    }

    // Nibble address for PSMT4.
    inline uint32_t addr4(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y)
    {
        const uint32_t ppr = (bw >> 1) ? (bw >> 1) : 1u;
        const uint32_t page = (bp >> 5) + (y >> 7) * ppr + (x >> 7);
        const uint32_t blk = (bp & 31u) + kBlock16[(y >> 4) & 7][(x >> 5) & 3];
        return ((page << 14) + ((blk >> 5) << 14) + (blk & 31u) * 512u + kColumn4[y & 15][x & 31]) & (kVramSize * 2u - 1u);
    }

    inline uint32_t rd32(const uint8_t *m, uint32_t a) { uint32_t v; std::memcpy(&v, m + a, 4); return v; }
    inline uint16_t rd16(const uint8_t *m, uint32_t a) { uint16_t v; std::memcpy(&v, m + a, 2); return v; }

    // Raw pixel value (index for T8/T4 formats).
    inline uint32_t readPixel(const uint8_t *m, uint32_t psm, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y)
    {
        switch (psm)
        {
        case CT32: return rd32(m, addr32(bp, bw, x, y));
        case CT24: return rd32(m, addr32(bp, bw, x, y)) & 0xFFFFFFu;
        case Z32: return rd32(m, addr32(bp, bw, x, y, true));
        case Z24: return rd32(m, addr32(bp, bw, x, y, true)) & 0xFFFFFFu;
        case CT16: return rd16(m, addr16(bp, bw, x, y, kBlock16));
        case CT16S: return rd16(m, addr16(bp, bw, x, y, kBlock16S));
        case Z16: return rd16(m, addr16(bp, bw, x, y, kBlockZ16));
        case Z16S: return rd16(m, addr16(bp, bw, x, y, kBlockZ16S));
        case T8: return m[addr8(bp, bw, x, y)];
        case T4: { const uint32_t n = addr4(bp, bw, x, y); return (m[n >> 1] >> ((n & 1u) * 4u)) & 0xFu; }
        case T8H: return rd32(m, addr32(bp, bw, x, y)) >> 24;
        case T4HL: return (rd32(m, addr32(bp, bw, x, y)) >> 24) & 0xFu;
        case T4HH: return rd32(m, addr32(bp, bw, x, y)) >> 28;
        default: return 0;
        }
    }

    inline void wr32(uint8_t *m, uint32_t a, uint32_t v, uint32_t mask = 0xFFFFFFFFu)
    {
        uint32_t o = rd32(m, a);
        o = (o & ~mask) | (v & mask);
        std::memcpy(m + a, &o, 4);
    }

    inline void writePixel(uint8_t *m, uint32_t psm, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y, uint32_t v)
    {
        switch (psm)
        {
        case CT32: wr32(m, addr32(bp, bw, x, y), v); break;
        case CT24: wr32(m, addr32(bp, bw, x, y), v, 0xFFFFFFu); break;
        case Z32: wr32(m, addr32(bp, bw, x, y, true), v); break;
        case Z24: wr32(m, addr32(bp, bw, x, y, true), v, 0xFFFFFFu); break;
        case CT16: { const uint16_t h = static_cast<uint16_t>(v); std::memcpy(m + addr16(bp, bw, x, y, kBlock16), &h, 2); break; }
        case CT16S: { const uint16_t h = static_cast<uint16_t>(v); std::memcpy(m + addr16(bp, bw, x, y, kBlock16S), &h, 2); break; }
        case Z16: { const uint16_t h = static_cast<uint16_t>(v); std::memcpy(m + addr16(bp, bw, x, y, kBlockZ16), &h, 2); break; }
        case Z16S: { const uint16_t h = static_cast<uint16_t>(v); std::memcpy(m + addr16(bp, bw, x, y, kBlockZ16S), &h, 2); break; }
        case T8: m[addr8(bp, bw, x, y)] = static_cast<uint8_t>(v); break;
        case T4:
        {
            const uint32_t n = addr4(bp, bw, x, y);
            uint8_t &b = m[n >> 1];
            b = (n & 1u) ? static_cast<uint8_t>((b & 0x0Fu) | ((v & 0xFu) << 4)) : static_cast<uint8_t>((b & 0xF0u) | (v & 0xFu));
            break;
        }
        case T8H: wr32(m, addr32(bp, bw, x, y), v << 24, 0xFF000000u); break;
        case T4HL: wr32(m, addr32(bp, bw, x, y), (v & 0xFu) << 24, 0x0F000000u); break;
        case T4HH: wr32(m, addr32(bp, bw, x, y), (v & 0xFu) << 28, 0xF0000000u); break;
        default: break;
        }
    }

    inline uint32_t bitsPerPixel(uint32_t psm)
    {
        switch (psm)
        {
        case CT32: case Z32: return 32;
        case CT24: case Z24: return 24;
        case CT16: case CT16S: case Z16: case Z16S: return 16;
        case T8: case T8H: return 8;
        case T4: case T4HL: case T4HH: return 4;
        default: return 32;
        }
    }

    // Page geometry (pixels) of a PSM.
    inline void pageSize(uint32_t psm, uint32_t &w, uint32_t &h)
    {
        switch (bitsPerPixel(psm))
        {
        case 16: w = 64; h = 64; break;
        case 8: w = (psm == T8H) ? 64 : 128; h = (psm == T8H) ? 32 : 64; break;
        case 4: w = (psm == T4) ? 128 : 64; h = (psm == T4) ? 128 : 32; break;
        default: w = 64; h = 32; break;
        }
    }

    // Conservative byte range [begin, end) touched by a w x h image at bp/bw.
    inline void extent(uint32_t psm, uint32_t bp, uint32_t bw, uint32_t w, uint32_t h, uint32_t &begin, uint32_t &end)
    {
        uint32_t pw, ph;
        pageSize(psm, pw, ph);
        const uint32_t ppr = (bitsPerPixel(psm) == 8 && psm != T8H) || psm == T4 ? ((bw >> 1) ? (bw >> 1) : 1u) : (bw ? bw : 1u);
        const uint32_t rows = (h + ph - 1) / ph;
        const uint32_t cols = (w + pw - 1) / pw;
        begin = bp * 256u;
        end = begin + ((rows ? rows - 1 : 0) * ppr + (cols ? cols : 1)) * 8192u;
    }

    // Host -> local transfer state machine (BITBLTBUF/TRXPOS/TRXREG + IMAGE data).
    struct Transfer
    {
        uint32_t dbp = 0, dbw = 0, dpsm = 0, dsax = 0, dsay = 0, rrw = 0, rrh = 0;
        uint32_t x = 0, y = 0;
        bool active = false;
        uint8_t partial[8];
        uint32_t partialBytes = 0;

        void begin(uint32_t dbp_, uint32_t dbw_, uint32_t dpsm_, uint32_t dsax_, uint32_t dsay_, uint32_t rrw_, uint32_t rrh_)
        {
            dbp = dbp_; dbw = dbw_; dpsm = dpsm_; dsax = dsax_; dsay = dsay_; rrw = rrw_; rrh = rrh_;
            x = y = 0;
            partialBytes = 0;
            active = rrw && rrh;
        }

        void putPixel(uint8_t *m, uint32_t v)
        {
            if (!active)
                return;
            writePixel(m, dpsm, dbp, dbw, (dsax + x) & 2047u, (dsay + y) & 2047u, v);
            if (++x >= rrw)
            {
                x = 0;
                if (++y >= rrh)
                    active = false;
            }
        }

        void data(uint8_t *m, const uint8_t *p, uint32_t n)
        {
            const uint32_t bpp = bitsPerPixel(dpsm);
            if (bpp == 4)
            {
                for (uint32_t k = 0; k < n && active; ++k)
                {
                    putPixel(m, p[k] & 0xFu);
                    putPixel(m, p[k] >> 4);
                }
                return;
            }
            const uint32_t step = bpp / 8u;
            uint32_t k = 0;
            while (partialBytes && k < n)
            {
                partial[partialBytes++] = p[k++];
                if (partialBytes == step)
                {
                    uint32_t v = 0;
                    std::memcpy(&v, partial, step);
                    putPixel(m, v);
                    partialBytes = 0;
                }
            }
            for (; k + step <= n && active; k += step)
            {
                uint32_t v = 0;
                std::memcpy(&v, p + k, step);
                putPixel(m, v);
            }
            for (; k < n && active; ++k)
                partial[partialBytes++] = p[k];
        }
    };

    // 256-entry CLUT index swizzle for CSM1 (bits 3 and 4 swapped).
    inline uint32_t csm1Index(uint32_t i)
    {
        return (i & 0xE7u) | ((i & 0x08u) << 1) | ((i & 0x10u) >> 1);
    }
}
