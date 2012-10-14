/* Copyright 2012 Dietrich Epp <depp@zdome.net> */
#define LFR_SSE2 1

#include "cpu.h"
#if defined(LFR_CPU_X86)
#include "resample.h"
#include <string.h>

void
lfr_resample_s16n2s16_sse2(
    lfr_fixed_t *pos, lfr_fixed_t inv_ratio, unsigned *dither,
    void *out, int outlen, const void *in, int inlen,
    const struct lfr_filter *filter)
{
    const __m128i *fd;
    int flen, log2nfilt;
    lfr_fixed_t x;

    __m128i acc, acc0, acc1, fir0, fir1, fir_interp, dat0, dat1;
    __m128i dsv, lcg_a, lcg_c;
    int fn, ff0, ff1, off, fidx0, fidx1;
    int accs0, accs1, i, j, f;
    unsigned ds;

    union {
        unsigned d[4];
        __m128i x;
    } u;

    /* fd: Pointer to beginning of filter coefficients, aligned.  */
    fd = (const __m128i *) filter->data;
    /* flen: Length of filter, measured in 128-bit words.  */
    flen = filter->nsamp >> 3;
    /* log2nfilt: Base 2 logarithm of the number of filters.  */
    log2nfilt = filter->log2nfilt;
    x = *pos;

    ds = *dither;
    for (i = 0; i < 4; ++i) {
        u.d[i] = ds;
        ds = ds * LCG_A + LCG_C;
    }
    dsv = u.x;
    lcg_a = _mm_set1_epi32(LCG_A4);
    lcg_c = _mm_set1_epi32(LCG_C4);

    acc0 = _mm_set1_epi32(0);
    acc1 = _mm_set1_epi32(0);
    for (i = 0; i < outlen; ++i) {
        /* fn: filter number
           ff0: filter factor for filter fn
           ff1: filter factor for filter fn+1 */
        fn = (((unsigned) x >> 1) >> (31 - log2nfilt)) &
            ((1u << log2nfilt) - 1);
        ff1 = ((unsigned) x >> (32 - log2nfilt - INTERP_BITS)) &
            ((1u << INTERP_BITS) - 1);
        ff0 = (1u << INTERP_BITS) - ff1;
        fir_interp = _mm_set1_epi32(ff0 | (ff1 << 16));

        /* acc: FIR accumulator, used for accumulating 32-bit values
           in the format L R L R.  This corresponds to one frame of
           output, so the pair of values for left and the pair for
           right have to be summed later.  */
        acc = _mm_set1_epi32(0);
        /* off: offset in input corresponding to first sample in
           filter */
        off = (int) (x >> 32);
        /* fixd0, fidx1: start, end indexes of 8-word (16-byte) chunks
           of whole FIR data we will use */
        fidx0 = (-off + 7) >> 3;
        fidx1 = (inlen - off) >> 3;
        if (fidx0 > 0) {
            if (fidx0 > flen)
                goto accumulate;
            accs0 = 0;
            accs1 = 0;
            for (j = -off; j < fidx0 * 8; ++j) {
                f = (((const short *) fd)[(fn+0) * (flen*8) + j] * ff0 +
                     ((const short *) fd)[(fn+1) * (flen*8) + j] * ff1)
                    >> INTERP_BITS;
                accs0 += ((const short *) in)[(j + off)*2 + 0] * f;
                accs1 += ((const short *) in)[(j + off)*2 + 1] * f;
            }
            acc = _mm_set_epi32(0, 0, accs1, accs0);
        } else {
            fidx0 = 0;
        }
        if (fidx1 < flen) {
            if (fidx1 < 0)
                goto accumulate;
            accs0 = 0;
            accs1 = 0;
            for (j = fidx1 * 8; j < inlen - off; ++j) {
                f = (((const short *) fd)[(fn+0) * (flen*8) + j] * ff0 +
                     ((const short *) fd)[(fn+1) * (flen*8) + j] * ff1)
                    >> INTERP_BITS;
                accs0 += ((const short *) in)[(j + off)*2 + 0] * f;
                accs1 += ((const short *) in)[(j + off)*2 + 1] * f;
            }
            acc = _mm_add_epi32(acc, _mm_set_epi32(0, 0, accs1, accs0));
        } else {
            fidx1 = flen;
        }

        for (j = fidx0; j < fidx1; ++j) {
            dat0 = _mm_loadu_si128(
                (const __m128i *)
                ((const short *) in + off*2 + j*16));
            dat1 = _mm_loadu_si128(
                (const __m128i *)
                ((const short *) in + off*2 + j*16+8));
            fir0 = fd[(fn+0)*flen + j];
            fir1 = fd[(fn+1)*flen + j];

            fir0 = _mm_packs_epi32(
                _mm_srai_epi32(
                    _mm_madd_epi16(
                        _mm_unpacklo_epi16(fir0, fir1),
                        fir_interp),
                    INTERP_BITS),
                _mm_srai_epi32(
                    _mm_madd_epi16(
                        _mm_unpackhi_epi16(fir0, fir1),
                        fir_interp),
                    INTERP_BITS));

            fir1 = _mm_unpackhi_epi16(fir0, fir0);
            fir0 = _mm_unpacklo_epi16(fir0, fir0);
            acc = _mm_add_epi32(
                acc,
                _mm_add_epi32(
                    _mm_madd_epi16(
                        _mm_unpacklo_epi16(dat0, dat1),
                        _mm_unpacklo_epi16(fir0, fir1)),
                    _mm_madd_epi16(
                        _mm_unpackhi_epi16(dat0, dat1),
                        _mm_unpackhi_epi16(fir0, fir1))));
        }

    accumulate:
        switch (i & 3) {
        case 0: case 2:
            acc0 = acc;
            break;

        case 1:
            acc1 = _mm_add_epi32(
                _mm_unpacklo_epi64(acc0, acc),
                _mm_unpackhi_epi64(acc0, acc));
            break;

        case 3:
            acc0 = _mm_add_epi32(
                _mm_unpacklo_epi64(acc0, acc),
                _mm_unpackhi_epi64(acc0, acc));

            /* Apply dither */
            acc1 = _mm_add_epi32(acc1, _mm_srli_epi32(dsv, 17));
            dsv = lfr_rand_epu32(dsv, lcg_a, lcg_c);
            acc0 = _mm_add_epi32(acc0, _mm_srli_epi32(dsv, 17));
            dsv = lfr_rand_epu32(dsv, lcg_a, lcg_c);

            acc = _mm_packs_epi32(
                _mm_srai_epi32(acc1, 15),
                _mm_srai_epi32(acc0, 15));
            _mm_storeu_si128((__m128i *) ((short *) out + (i - 3) * 2), acc);
            break;
        }

        x += inv_ratio;
    }

    ds = _mm_cvtsi128_si32(dsv);
    for (i = 0; i < (outlen & 3) * 2; ++i)
        ds = LCG_A * ds + LCG_C;
    *pos = x;
    *dither = ds;

    /* Store remaing bytes */
    acc = _mm_set1_epi32(0);
    if ((i & 3) == 0)
        return;
    for (i = outlen; ; ++i) {
        switch (i & 3) {
        case 0: case 2:
            acc0 = acc;
            break;

        case 1:
            acc1 = _mm_add_epi32(
                _mm_unpacklo_epi64(acc0, acc),
                _mm_unpackhi_epi64(acc0, acc));
            break;

        case 3:
            acc0 = _mm_add_epi32(
                _mm_unpacklo_epi64(acc0, acc),
                _mm_unpackhi_epi64(acc0, acc));

            /* Apply dither */
            acc1 = _mm_add_epi32(acc1, _mm_srli_epi32(dsv, 17));
            dsv = lfr_rand_epu32(dsv, lcg_a, lcg_c);
            acc0 = _mm_add_epi32(acc0, _mm_srli_epi32(dsv, 17));

            acc = _mm_packs_epi32(
                _mm_srai_epi32(acc1, 15),
                _mm_srai_epi32(acc0, 15));
            /* We don't memcpy acc directly because it messes with the
               optimizer.  */
            u.x = acc;
            memcpy((short *) out + (i - 3) * 2, &u, 4 * (outlen & 3));
            return;
        }
    }
}

#endif
