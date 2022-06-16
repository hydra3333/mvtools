

#include "ClipFnc.h"
#include "CopyCode.h"
#include	"def.h"
#include	"MDegrainN.h"
#include	"MVDegrain3.h"
#include "MVFrame.h"
#include "MVPlane.h"
#include "MVFilter.h"
#include "profile.h"
#include "SuperParams64Bits.h"
#include "SADFunctions.h"

#include	<emmintrin.h>
#include	<mmintrin.h>

#include	<cassert>
#include	<cmath>
#include <map>
#include <tuple>
#include <stdint.h>
#include "commonfunctions.h"

// out16_type: 
//   0: native 8 or 16
//   1: 8bit in, lsb
//   2: 8bit in, native16 out
template <typename pixel_t, int blockWidth, int blockHeight, int out16_type>
void DegrainN_C(
  BYTE* pDst, BYTE* pDstLsb, int nDstPitch,
  const BYTE* pSrc, int nSrcPitch,
  const BYTE* pRef[], int Pitch[],
  int Wall[], int trad
)
{
  // for less template, see solution in Degrain1to6_C

  constexpr bool lsb_flag = (out16_type == 1);
  constexpr bool out16 = (out16_type == 2);

  if constexpr (lsb_flag || out16)
  {
    // 8 bit base only
    for (int h = 0; h < blockHeight; ++h)
    {
      for (int x = 0; x < blockWidth; ++x)
      {
        int val = pSrc[x] * Wall[0];
        for (int k = 0; k < trad; ++k)
        {
          val += pRef[k * 2][x] * (short)Wall[k * 2 + 1]
            + pRef[k * 2 + 1][x] * (short)Wall[k * 2 + 2]; // to be compatible with 2x16bit 32bit weight
        }
        if constexpr (lsb_flag) {
          pDst[x] = (uint8_t)(val >> 8);
          pDstLsb[x] = (uint8_t)(val & 255);
        }
        else { // out16
          reinterpret_cast<uint16_t*>(pDst)[x] = (uint16_t)val;
        }
      }

      pDst += nDstPitch;
      if constexpr (lsb_flag)
        pDstLsb += nDstPitch;
      pSrc += nSrcPitch;
      for (int k = 0; k < trad; ++k)
      {
        pRef[k * 2] += Pitch[k * 2];
        pRef[k * 2 + 1] += Pitch[k * 2 + 1];
      }
    }
  }

  else
  {
    typedef typename std::conditional < sizeof(pixel_t) <= 2, int, float>::type target_t;
    constexpr target_t rounder = (sizeof(pixel_t) <= 2) ? 128 : 0;
    constexpr float scaleback = 1.0f / (1 << DEGRAIN_WEIGHT_BITS);

    // Wall: 8 bit. rounding: 128
    for (int h = 0; h < blockHeight; ++h)
    {
      for (int x = 0; x < blockWidth; ++x)
      {
        target_t val = reinterpret_cast<const pixel_t*>(pSrc)[x] * (target_t)Wall[0] + rounder;
        for (int k = 0; k < trad; ++k)
        {
          val += reinterpret_cast<const pixel_t*>(pRef[k * 2])[x] * (target_t)Wall[k * 2 + 1]
            + reinterpret_cast<const pixel_t*>(pRef[k * 2 + 1])[x] * (target_t)Wall[k * 2 + 2]; // do it compatible with 2x16bit weight ?
        }
        if constexpr (sizeof(pixel_t) <= 2)
          reinterpret_cast<pixel_t*>(pDst)[x] = (pixel_t)(val >> 8); // 8-16bit
        else
          reinterpret_cast<pixel_t*>(pDst)[x] = val * scaleback; // 32bit float
      }

      pDst += nDstPitch;
      pSrc += nSrcPitch;
      for (int k = 0; k < trad; ++k)
      {
        pRef[k * 2] += Pitch[k * 2];
        pRef[k * 2 + 1] += Pitch[k * 2 + 1];
      }
    }
  }
}

// Debug note: DegrainN filter is calling Degrain1-6 instead if ThSAD(C) == ThSAD(C)2.
// To reach DegrainN_ functions, set the above parameters to different values

// out16_type: 
//   0: native 8 or 16
//   1: 8bit in, lsb
//   2: 8bit in, native16 out
template <int blockWidth, int blockHeight, int out16_type>
void DegrainN_sse2(
  BYTE* pDst, BYTE* pDstLsb, int nDstPitch,
  const BYTE* pSrc, int nSrcPitch,
  const BYTE* pRef[], int Pitch[],
  int Wall[], int trad
)
{
  assert(blockWidth % 4 == 0);
  // only mod4 supported

  constexpr bool lsb_flag = (out16_type == 1);
  constexpr bool out16 = (out16_type == 2);

  const __m128i z = _mm_setzero_si128();

  constexpr bool is_mod8 = blockWidth % 8 == 0;
  constexpr int pixels_at_a_time = is_mod8 ? 8 : 4; // 4 for 4 and 12; 8 for all others 8, 16, 24, 32...

  if constexpr (lsb_flag || out16)
  {
    // no rounding
    const __m128i m = _mm_set1_epi16(255);

    for (int h = 0; h < blockHeight; ++h)
    {
      for (int x = 0; x < blockWidth; x += pixels_at_a_time)
      {
        __m128i src;
        if (is_mod8) // load 8 pixels
          src = _mm_loadl_epi64((__m128i*) (pSrc + x));
        else // load 4 pixels
          src = _mm_cvtsi32_si128(*(uint32_t*)(pSrc + x));
        __m128i val = _mm_mullo_epi16(_mm_unpacklo_epi8(src, z), _mm_set1_epi16(Wall[0]));
        for (int k = 0; k < trad; ++k)
        {
          __m128i src1, src2;
          if constexpr (is_mod8) // load 8-8 pixels
          {
            src1 = _mm_loadl_epi64((__m128i*) (pRef[k * 2] + x));
            src2 = _mm_loadl_epi64((__m128i*) (pRef[k * 2 + 1] + x));
          }
          else { // 4-4 pixels
            src1 = _mm_cvtsi32_si128(*(uint32_t*)(pRef[k * 2] + x));
            src2 = _mm_cvtsi32_si128(*(uint32_t*)(pRef[k * 2 + 1] + x));
          }
          const __m128i	s1 = _mm_mullo_epi16(_mm_unpacklo_epi8(src1, z), _mm_set1_epi16(Wall[k * 2 + 1]));
          const __m128i	s2 = _mm_mullo_epi16(_mm_unpacklo_epi8(src2, z), _mm_set1_epi16(Wall[k * 2 + 2]));
          val = _mm_add_epi16(val, s1);
          val = _mm_add_epi16(val, s2);
        }
        if constexpr (is_mod8) {
          if constexpr (lsb_flag) {
            _mm_storel_epi64((__m128i*)(pDst + x), _mm_packus_epi16(_mm_srli_epi16(val, 8), z));
            _mm_storel_epi64((__m128i*)(pDstLsb + x), _mm_packus_epi16(_mm_and_si128(val, m), z));
          }
          else {
            _mm_storeu_si128((__m128i*)(pDst + x * 2), val);
          }
          }
        else {
          if constexpr (lsb_flag) {
            *(uint32_t*)(pDst + x) = _mm_cvtsi128_si32(_mm_packus_epi16(_mm_srli_epi16(val, 8), z));
            *(uint32_t*)(pDstLsb + x) = _mm_cvtsi128_si32(_mm_packus_epi16(_mm_and_si128(val, m), z));
          }
          else {
            _mm_storel_epi64((__m128i*)(pDst + x * 2), val);
          }
        }
        }
      pDst += nDstPitch;
      if constexpr (lsb_flag)
        pDstLsb += nDstPitch;
      pSrc += nSrcPitch;
      for (int k = 0; k < trad; ++k)
      {
        pRef[k * 2] += Pitch[k * 2];
        pRef[k * 2 + 1] += Pitch[k * 2 + 1];
      }
      }
    }

  else
  {
    // base 8 bit -> 8 bit
    const __m128i o = _mm_set1_epi16(128); // rounding

    for (int h = 0; h < blockHeight; ++h)
    {
      for (int x = 0; x < blockWidth; x += pixels_at_a_time)
      {
        __m128i src;
        if constexpr (is_mod8) // load 8 pixels
          src = _mm_loadl_epi64((__m128i*) (pSrc + x));
        else // load 4 pixels
          src = _mm_cvtsi32_si128(*(uint32_t*)(pSrc + x));

        __m128i val = _mm_add_epi16(_mm_mullo_epi16(_mm_unpacklo_epi8(src, z), _mm_set1_epi16(Wall[0])), o);
        for (int k = 0; k < trad; ++k)
        {
          __m128i src1, src2;
          if constexpr (is_mod8) // load 8-8 pixels
          {
            src1 = _mm_loadl_epi64((__m128i*) (pRef[k * 2] + x));
            src2 = _mm_loadl_epi64((__m128i*) (pRef[k * 2 + 1] + x));
          }
          else { // 4-4 pixels
            src1 = _mm_cvtsi32_si128(*(uint32_t*)(pRef[k * 2] + x));
            src2 = _mm_cvtsi32_si128(*(uint32_t*)(pRef[k * 2 + 1] + x));
          }
          const __m128i s1 = _mm_mullo_epi16(_mm_unpacklo_epi8(src1, z), _mm_set1_epi16(Wall[k * 2 + 1]));
          const __m128i s2 = _mm_mullo_epi16(_mm_unpacklo_epi8(src2, z), _mm_set1_epi16(Wall[k * 2 + 2]));
          val = _mm_add_epi16(val, s1);
          val = _mm_add_epi16(val, s2);
        }
        auto res = _mm_packus_epi16(_mm_srli_epi16(val, 8), z);
        if constexpr (is_mod8) {
          _mm_storel_epi64((__m128i*)(pDst + x), res);
        }
        else {
          *(uint32_t*)(pDst + x) = _mm_cvtsi128_si32(res);
        }
      }

      pDst += nDstPitch;
      pSrc += nSrcPitch;
      for (int k = 0; k < trad; ++k)
      {
        pRef[k * 2] += Pitch[k * 2];
        pRef[k * 2 + 1] += Pitch[k * 2 + 1];
      }
    }
  }
}

// soft edges weighting blending function
template <int blockWidth, int blockHeight, int out16_type>
void DegrainN_sse2_SEWB(
  BYTE* pDst, BYTE* pDstLsb, int nDstPitch,
  const BYTE* pSrc, int nSrcPitch,
  const BYTE* pRef[], int Pitch[],
  uint16_t* pWall, int trad
)
{
  assert(blockWidth % 4 == 0);
  // only mod4 supported

  int iBlkSize = blockWidth * blockHeight; // for 8bit

  constexpr bool lsb_flag = (out16_type == 1);
  constexpr bool out16 = (out16_type == 2);

  const __m128i z = _mm_setzero_si128();

  constexpr bool is_mod8 = blockWidth % 8 == 0;
  constexpr int pixels_at_a_time = is_mod8 ? 8 : 4; // 4 for 4 and 12; 8 for all others 8, 16, 24, 32...

  // base 8 bit -> 8 bit
  const __m128i o = _mm_set1_epi16(128); // rounding
  
  uint16_t* pW = pWall + (uint64_t)iBlkSize; // shift by size of W0 weight buff

  for (int h = 0; h < blockHeight; ++h)
  {
    for (int x = 0; x < blockWidth; x += pixels_at_a_time)
    {
      __m128i src;
      if constexpr (is_mod8) // load 8 pixels
        src = _mm_loadl_epi64((__m128i*) (pSrc + x));
      else // load 4 pixels
        src = _mm_cvtsi32_si128(*(uint32_t*)(pSrc + x));

      __m128i weight_src = _mm_loadu_si128((__m128i*) (&pWall[0] + (uint64_t)h * blockWidth + x));

      //        __m128i val = _mm_add_epi16(_mm_mullo_epi16(_mm_unpacklo_epi8(src, z), _mm_set1_epi16(Wall[0])), o);
      __m128i val = _mm_add_epi16(_mm_mullo_epi16(_mm_unpacklo_epi8(src, z), weight_src), o);

      for (int k = 0; k < trad; ++k)
      {
        __m128i src1, src2;
        __m128i weight1, weight2;
        if constexpr (is_mod8) // load 8-8 pixels
        {
          src1 = _mm_loadl_epi64((__m128i*) (pRef[k * 2] + x));
          src2 = _mm_loadl_epi64((__m128i*) (pRef[k * 2 + 1] + x));

          weight1 = _mm_loadu_si128((__m128i*)(pW + ((uint64_t)k * 2 * (uint64_t)iBlkSize + x)));
          weight2 = _mm_loadu_si128((__m128i*)(pW + (((uint64_t)k * 2 + 1) * (uint64_t)iBlkSize + x))); // +x - not tested for > 8x8 blocks

        }
        else { // 4-4 pixels
          src1 = _mm_cvtsi32_si128(*(uint32_t*)(pRef[k * 2] + x));
          src2 = _mm_cvtsi32_si128(*(uint32_t*)(pRef[k * 2 + 1] + x));
        }
        //          const __m128i s1 = _mm_mullo_epi16(_mm_unpacklo_epi8(src1, z), _mm_set1_epi16(Wall[k * 2 + 1]));
        //          const __m128i s2 = _mm_mullo_epi16(_mm_unpacklo_epi8(src2, z), _mm_set1_epi16(Wall[k * 2 + 2]));
          const __m128i s1 = _mm_mullo_epi16(_mm_unpacklo_epi8(src1, z), weight1);
          const __m128i s2 = _mm_mullo_epi16(_mm_unpacklo_epi8(src2, z), weight2);

        val = _mm_add_epi16(val, s1);
        val = _mm_add_epi16(val, s2);
      }

      auto res = _mm_packus_epi16(_mm_srli_epi16(val, 8), z);

      if constexpr (is_mod8) {
        _mm_storel_epi64((__m128i*)(pDst + x), res);
      }
      else {
        *(uint32_t*)(pDst + x) = _mm_cvtsi128_si32(res);
      }
    }

    pW += blockWidth;

    pDst += nDstPitch;
    pSrc += nSrcPitch;
    for (int k = 0; k < trad; ++k)
    {
      pRef[k * 2] += Pitch[k * 2];
      pRef[k * 2 + 1] += Pitch[k * 2 + 1];
    }
  }

}



template<int blockWidth, int blockHeight, bool lessThan16bits>
void DegrainN_16_sse41(
  BYTE* pDst, BYTE* pDstLsb, int nDstPitch,
  const BYTE* pSrc, int nSrcPitch,
  const BYTE* pRef[], int Pitch[],
  int Wall[], int trad
)
{
  assert(blockWidth % 4 == 0);
  // only mod4 supported

  // able to do madd for real 16 bit uint16_t data
  const auto signed16_shifter = _mm_set1_epi16(-32768);
  const auto signed16_shifter_si32 = _mm_set1_epi32(32768 << DEGRAIN_WEIGHT_BITS);

  const __m128i z = _mm_setzero_si128();
  constexpr int SHIFTBACK = DEGRAIN_WEIGHT_BITS;
  constexpr int rounder_i = (1 << SHIFTBACK) / 2;
  // note: DEGRAIN_WEIGHT_BITS is fixed 8 bits, so no rounding occurs on 8 bit in 16 bit out

  __m128i rounder = _mm_set1_epi32(rounder_i); // rounding: 128 (mul by 8 bit wref scale back)

  for (int h = 0; h < blockHeight; ++h)
  {
    for (int x = 0; x < blockWidth; x += 8 / sizeof(uint16_t)) // up to 4 pixels per cycle
    {
      // load 4 pixels
      auto src = _mm_loadl_epi64((__m128i*)(pSrc + x * sizeof(uint16_t)));

      // weights array structure: center, forward1, backward1, forward2, backward2, etc
      //                          Wall[0] Wall[1]   Wall[2]    Wall[3]   Wall[4] ...
      // inputs structure:        pSrc    pRef[0]   pRef[1]    pRef[2]   pRef[3] ...

      __m128i res;
      // make signed when unsigned 16 bit mode
      if constexpr (!lessThan16bits)
        src = _mm_add_epi16(src, signed16_shifter);

      // Interleave Src 0 Src 0 ...
      src = _mm_cvtepu16_epi32(src); // sse4 unpacklo_epi16 w/ zero

      // interleave 0 and center weight
      auto ws = _mm_set1_epi32((0 << 16) + Wall[0]);
      // pSrc[x] * WSrc + 0 * 0
      res = _mm_madd_epi16(src, ws);

      // pRefF[n][x] * WRefF[n] + pRefB[n][x] * WRefB[n]
      for (int k = 0; k < trad; ++k)
      {
        // Interleave SrcF SrcB
        src = _mm_unpacklo_epi16(
          _mm_loadl_epi64((__m128i*)(pRef[k * 2] + x * sizeof(uint16_t))), // from forward
          _mm_loadl_epi64((__m128i*)(pRef[k * 2 + 1] + x * sizeof(uint16_t)))); // from backward
        if constexpr (!lessThan16bits)
          src = _mm_add_epi16(src, signed16_shifter);

        // Interleave Forward and Backward 16 bit weights for madd
        // backward << 16 | forward in a 32 bit
        auto weightBF = _mm_set1_epi32((Wall[k * 2 + 2] << 16) + Wall[k * 2 + 1]);
        res = _mm_add_epi32(res, _mm_madd_epi16(src, weightBF));
      }

      res = _mm_add_epi32(res, rounder); // round

      res = _mm_packs_epi32(_mm_srai_epi32(res, SHIFTBACK), z);
      // make unsigned when unsigned 16 bit mode
      if constexpr (!lessThan16bits)
        res = _mm_add_epi16(res, signed16_shifter);

      // we are supporting only mod4
      // 4, 8, 12, ...
      _mm_storel_epi64((__m128i*)(pDst + x * sizeof(uint16_t)), res);

#if 0
      // sample from MDegrainX, not only mod4
      if constexpr (blockWidth == 6) {
        // special, 4+2
        if (x == 0)
          _mm_storel_epi64((__m128i*)(pDst + x * sizeof(uint16_t)), res);
        else
          *(uint32_t*)(pDst + x * sizeof(uint16_t)) = _mm_cvtsi128_si32(res);
      }
      else if constexpr (blockWidth >= 8 / sizeof(uint16_t)) { // block 4 is already 8 bytes
        // 4, 8, 12, ...
        _mm_storel_epi64((__m128i*)(pDst + x * sizeof(uint16_t)), res);
      }
      else if constexpr (blockWidth == 3) { // blockwidth 3 is 6 bytes
        // x == 0 always
        *(uint32_t*)(pDst) = _mm_cvtsi128_si32(res); // 1-4 bytes
        uint32_t res32 = _mm_cvtsi128_si32(_mm_srli_si128(res, 4)); // 5-8 byte
        *(uint16_t*)(pDst + sizeof(uint32_t)) = (uint16_t)res32; // 2 bytes needed
      }
      else { // blockwidth 2 is 4 bytes
        *(uint32_t*)(pDst + x * sizeof(uint16_t)) = _mm_cvtsi128_si32(res);
      }
#endif

    }

    pDst += nDstPitch;
    pSrc += nSrcPitch;
    for (int k = 0; k < trad; ++k)
    {
      pRef[k * 2] += Pitch[k * 2];
      pRef[k * 2 + 1] += Pitch[k * 2 + 1];
    }
  }

}

MDegrainN::DenoiseNFunction* MDegrainN::get_denoiseN_function(int BlockX, int BlockY, int _bits_per_pixel, bool _lsb_flag, bool _out16_flag, arch_t arch)
{
  //---------- DENOISE/DEGRAIN
  const int DEGRAIN_TYPE_8BIT = 1;
  const int DEGRAIN_TYPE_8BIT_STACKED = 2;
  const int DEGRAIN_TYPE_8BIT_OUT16 = 4;
  const int DEGRAIN_TYPE_10to14BIT = 8;
  const int DEGRAIN_TYPE_16BIT = 16;
  const int DEGRAIN_TYPE_32BIT = 32;
  // BlkSizeX, BlkSizeY, degrain_type, arch_t
  std::map<std::tuple<int, int, int, arch_t>, DenoiseNFunction*> func_degrain;
  using std::make_tuple;

  int type_to_search;
  if (_bits_per_pixel == 8) {
    if (_out16_flag)
      type_to_search = DEGRAIN_TYPE_8BIT_OUT16;
    else if (_lsb_flag)
      type_to_search = DEGRAIN_TYPE_8BIT_STACKED;
    else
      type_to_search = DEGRAIN_TYPE_8BIT;
  }
  else if (_bits_per_pixel <= 14)
    type_to_search = DEGRAIN_TYPE_10to14BIT;
  else if (_bits_per_pixel == 16)
    type_to_search = DEGRAIN_TYPE_16BIT;
  else if (_bits_per_pixel == 32)
    type_to_search = DEGRAIN_TYPE_32BIT;
  else
    return nullptr;


  // 8bit C, 8bit lsb C, 8bit out16 C, 10-16 bit C, float C (same for all, no blocksize templates)
#define MAKE_FN(x, y) \
func_degrain[make_tuple(x, y, DEGRAIN_TYPE_8BIT, NO_SIMD)] = DegrainN_C<uint8_t, x, y, 0>; \
func_degrain[make_tuple(x, y, DEGRAIN_TYPE_8BIT_STACKED, NO_SIMD)] = DegrainN_C<uint8_t, x, y, 1>; \
func_degrain[make_tuple(x, y, DEGRAIN_TYPE_8BIT_OUT16, NO_SIMD)] = DegrainN_C<uint8_t, x, y, 2>; \
func_degrain[make_tuple(x, y, DEGRAIN_TYPE_10to14BIT, NO_SIMD)] = DegrainN_C<uint16_t, x, y, 0>; \
func_degrain[make_tuple(x, y, DEGRAIN_TYPE_16BIT, NO_SIMD)] = DegrainN_C<uint16_t, x, y, 0>; \
func_degrain[make_tuple(x, y, DEGRAIN_TYPE_32BIT, NO_SIMD)] = DegrainN_C<float, x, y, 0>;
    MAKE_FN(64, 64)
    MAKE_FN(64, 48)
    MAKE_FN(64, 32)
    MAKE_FN(64, 16)
    MAKE_FN(48, 64)
    MAKE_FN(48, 48)
    MAKE_FN(48, 24)
    MAKE_FN(48, 12)
    MAKE_FN(32, 64)
    MAKE_FN(32, 32)
    MAKE_FN(32, 24)
    MAKE_FN(32, 16)
    MAKE_FN(32, 8)
    MAKE_FN(24, 48)
    MAKE_FN(24, 32)
    MAKE_FN(24, 24)
    MAKE_FN(24, 12)
    MAKE_FN(24, 6)
    MAKE_FN(16, 64)
    MAKE_FN(16, 32)
    MAKE_FN(16, 16)
    MAKE_FN(16, 12)
    MAKE_FN(16, 8)
    MAKE_FN(16, 4)
    MAKE_FN(16, 2)
    MAKE_FN(16, 1)
    MAKE_FN(12, 48)
    MAKE_FN(12, 24)
    MAKE_FN(12, 16)
    MAKE_FN(12, 12)
    MAKE_FN(12, 6)
    MAKE_FN(12, 3)
    MAKE_FN(8, 32)
    MAKE_FN(8, 16)
    MAKE_FN(8, 8)
    MAKE_FN(8, 4)
    MAKE_FN(8, 2)
    MAKE_FN(8, 1)
    MAKE_FN(6, 24)
    MAKE_FN(6, 12)
    MAKE_FN(6, 6)
    MAKE_FN(6, 3)
    MAKE_FN(4, 8)
    MAKE_FN(4, 4)
    MAKE_FN(4, 2)
    MAKE_FN(4, 1)
    MAKE_FN(3, 6)
    MAKE_FN(3, 3)
    MAKE_FN(2, 4)
    MAKE_FN(2, 2)
    MAKE_FN(2, 1)
#undef MAKE_FN
#undef MAKE_FN_LEVEL

      // and the SSE2 versions for 8 bit
#define MAKE_FN(x, y) \
func_degrain[make_tuple(x, y, DEGRAIN_TYPE_8BIT, USE_SSE2)] = DegrainN_sse2<x, y, 0>; \
func_degrain[make_tuple(x, y, DEGRAIN_TYPE_8BIT_STACKED, USE_SSE2)] = DegrainN_sse2<x, y, 1>; \
func_degrain[make_tuple(x, y, DEGRAIN_TYPE_8BIT_OUT16, USE_SSE2)] = DegrainN_sse2<x, y, 2>; \
func_degrain[make_tuple(x, y, DEGRAIN_TYPE_10to14BIT, USE_SSE41)] = DegrainN_16_sse41<x, y, true>; \
func_degrain[make_tuple(x, y, DEGRAIN_TYPE_16BIT, USE_SSE41)] = DegrainN_16_sse41<x, y, false>;

  MAKE_FN(64, 64)
    MAKE_FN(64, 48)
    MAKE_FN(64, 32)
    MAKE_FN(64, 16)
    MAKE_FN(48, 64)
    MAKE_FN(48, 48)
    MAKE_FN(48, 24)
    MAKE_FN(48, 12)
    MAKE_FN(32, 64)
    MAKE_FN(32, 32)
    MAKE_FN(32, 24)
    MAKE_FN(32, 16)
    MAKE_FN(32, 8)
    MAKE_FN(24, 48)
    MAKE_FN(24, 32)
    MAKE_FN(24, 24)
    MAKE_FN(24, 12)
    MAKE_FN(24, 6)
    MAKE_FN(16, 64)
    MAKE_FN(16, 32)
    MAKE_FN(16, 16)
    MAKE_FN(16, 12)
    MAKE_FN(16, 8)
    MAKE_FN(16, 4)
    MAKE_FN(16, 2)
    MAKE_FN(16, 1)
    MAKE_FN(12, 48)
    MAKE_FN(12, 24)
    MAKE_FN(12, 16)
    MAKE_FN(12, 12)
    MAKE_FN(12, 6)
    MAKE_FN(12, 3) 
    MAKE_FN(8, 32)
    MAKE_FN(8, 16)
    MAKE_FN(8, 8)
    MAKE_FN(8, 4)
    MAKE_FN(8, 2)
    MAKE_FN(8, 1)
    //MAKE_FN(6, 24) // w is mod4 only supported
    //MAKE_FN(6, 12)
    //MAKE_FN(6, 6)
    //MAKE_FN(6, 3)
    MAKE_FN(4, 8)
    MAKE_FN(4, 4)
    MAKE_FN(4, 2)
    MAKE_FN(4, 1)
    //MAKE_FN(3, 6) // w is mod4 only supported
    //MAKE_FN(3, 3)
    //MAKE_FN(2, 4) // no 2 byte width, only C
    //MAKE_FN(2, 2) // no 2 byte width, only C
    //MAKE_FN(2, 1) // no 2 byte width, only C
#undef MAKE_FN
#undef MAKE_FN_LEVEL

  DenoiseNFunction* result = nullptr;
  arch_t archlist[] = { USE_AVX2, USE_AVX, USE_SSE41, USE_SSE2, NO_SIMD };
  int index = 0;
  while (result == nullptr) {
    arch_t current_arch_try = archlist[index++];
    if (current_arch_try > arch) continue;
    result = func_degrain[make_tuple(BlockX, BlockY, type_to_search, current_arch_try)];
    if (result == nullptr && current_arch_try == NO_SIMD)
      break;
  }
  return result;
}



MDegrainN::MDegrainN(
  PClip child, PClip super, PClip mvmulti, int trad,
  sad_t thsad, sad_t thsadc, int yuvplanes, float nlimit, float nlimitc,
  sad_t nscd1, int nscd2, bool isse_flag, bool planar_flag, bool lsb_flag,
  sad_t thsad2, sad_t thsadc2, bool mt_flag, bool out16_flag, int wpow, float adjSADzeromv, float adjSADcohmv, int thCohMV,
  float MVLPFCutoff, float MVLPFSlope, float MVLPFGauss, int thMVLPFCorr, int UseSubShift, int SEWBWidth,
  IScriptEnvironment* env_ptr
)
  : GenericVideoFilter(child)
  , MVFilter(mvmulti, "MDegrainN", env_ptr, 1, 0)
  , _mv_clip_arr()
  , _trad(trad)
  , _yuvplanes(yuvplanes)
  , _nlimit(nlimit)
  , _nlimitc(nlimitc)
  , _super(super)
  , _planar_flag(planar_flag)
  , _lsb_flag(lsb_flag)
  , _mt_flag(mt_flag)
  , _out16_flag(out16_flag)
  , _height_lsb_or_out16_mul((lsb_flag || out16_flag) ? 2 : 1)
  , _nsupermodeyuv(-1)
  , _dst_planes(nullptr)
  , _src_planes(nullptr)
  , _overwins()
  , _overwins_uv()
  , _oversluma_ptr(0)
  , _overschroma_ptr(0)
  , _oversluma16_ptr(0)
  , _overschroma16_ptr(0)
  , _oversluma32_ptr(0)
  , _overschroma32_ptr(0)
  , _oversluma_lsb_ptr(0)
  , _overschroma_lsb_ptr(0)
  , _degrainluma_ptr(0)
  , _degrainchroma_ptr(0)
  , _dst_short()
  , _dst_short_pitch()
  , _dst_int()
  , _dst_int_pitch()
  //,	_usable_flag_arr ()
  //,	_planes_ptr ()
  //,	_dst_ptr_arr ()
  //,	_src_ptr_arr ()
  //,	_dst_pitch_arr ()
  //,	_src_pitch_arr ()
  //,	_lsb_offset_arr ()
  , _covered_width(0)
  , _covered_height(0)
  , _boundary_cnt_arr()
  , fadjSADzeromv(adjSADzeromv)
  , fadjSADcohmv(adjSADcohmv)
  , ithCohMV(thCohMV) // need to scale to pel value ?
  , fMVLPFCutoff(MVLPFCutoff)
  , fMVLPFSlope(MVLPFSlope)
  , fMVLPFGauss(MVLPFGauss)
  , ithMVLPFCorr(thMVLPFCorr)
  , nUseSubShift(UseSubShift)
  , iSEWBWidth (SEWBWidth)
{
  has_at_least_v8 = true;
  try { env_ptr->CheckVersion(8); }
  catch (const AvisynthError&) { has_at_least_v8 = false; }

  if (trad > MAX_TEMP_RAD)
  {
    env_ptr->ThrowError(
      "MDegrainN: temporal radius too large (max %d)",
      MAX_TEMP_RAD
    );
  }
  else if (trad < 1)
  {
    env_ptr->ThrowError("MDegrainN: temporal radius must be at least 1.");
  }

  if (wpow < 1 || wpow > 7)
  {
    env_ptr->ThrowError("MDegrainN: wpow must be from 1 to 7. 7 = equal weights.");
  }

  _wpow = wpow;

  _mv_clip_arr.resize(_trad * 2);
  for (int k = 0; k < _trad * 2; ++k)
  {
    _mv_clip_arr[k]._clip_sptr = SharedPtr <MVClip>(
      new MVClip(mvmulti, nscd1, nscd2, env_ptr, _trad * 2, k, true) // use MVsArray only, not blocks[]
      );

    static const char *name_0[2] = { "mvbw", "mvfw" };
    char txt_0[127 + 1];
    sprintf(txt_0, "%s%d", name_0[k & 1], 1 + k / 2);
    CheckSimilarity(*(_mv_clip_arr[k]._clip_sptr), txt_0, env_ptr);
  }

  const sad_t mv_thscd1 = _mv_clip_arr[0]._clip_sptr->GetThSCD1();
  thsad = (uint64_t)thsad   * mv_thscd1 / nscd1;	// normalize to block SAD
  thsadc = (uint64_t)thsadc  * mv_thscd1 / nscd1;	// chroma
  thsad2 = (uint64_t)thsad2  * mv_thscd1 / nscd1;
  thsadc2 = (uint64_t)thsadc2 * mv_thscd1 / nscd1;

  const ::VideoInfo &vi_super = _super->GetVideoInfo();

  if (!vi.IsSameColorspace(_super->GetVideoInfo()))
    env_ptr->ThrowError("MDegrainN: source and super clip video format is different!");

  // v2.7.39- make subsampling independent from motion vector's origin:
  // because xRatioUV and yRatioUV: in MVFilter, property of motion vectors
  xRatioUV_super = 1;
  yRatioUV_super = 1;
  if (!vi.IsY() && !vi.IsRGB()) {
    xRatioUV_super = vi.IsYUY2() ? 2 : (1 << vi.GetPlaneWidthSubsampling(PLANAR_U));
    yRatioUV_super = vi.IsYUY2() ? 1 : (1 << vi.GetPlaneHeightSubsampling(PLANAR_U));
  }
  nLogxRatioUV_super = ilog2(xRatioUV_super);
  nLogyRatioUV_super = ilog2(yRatioUV_super);

  pixelsize_super = vi_super.ComponentSize(); // of MVFilter
  bits_per_pixel_super = vi_super.BitsPerComponent();

  _cpuFlags = isse_flag ? env_ptr->GetCPUFlags() : 0;

// get parameters of prepared super clip - v2.0
  SuperParams64Bits params;
  memcpy(&params, &vi_super.num_audio_samples, 8);
  const int nHeightS = params.nHeight;
  const int nSuperHPad = params.nHPad;
  const int nSuperVPad = params.nVPad;
  const int nSuperPel = params.nPel;
  const int nSuperLevels = params.nLevels;
  _nsupermodeyuv = params.nModeYUV;

  // no need for SAD scaling, it is coming from the mv clip analysis. nSCD1 is already scaled in MVClip constructor
  /* must be good from 2.7.13.22
  thsad = sad_t(thsad / 255.0 * ((1 << bits_per_pixel) - 1));
  thsadc = sad_t(thsadc / 255.0 * ((1 << bits_per_pixel) - 1));
  thsad2 = sad_t(thsad2 / 255.0 * ((1 << bits_per_pixel) - 1));
  thsadc2 = sad_t(thsadc2 / 255.0 * ((1 << bits_per_pixel) - 1));
  */

  for (int k = 0; k < _trad * 2; ++k)
  {
    MvClipInfo &c_info = _mv_clip_arr[k];

    c_info._gof_sptr = SharedPtr <MVGroupOfFrames>(new MVGroupOfFrames(
      nSuperLevels,
      nWidth,
      nHeight,
      nSuperPel,
      nSuperHPad,
      nSuperVPad,
      _nsupermodeyuv,
      _cpuFlags,
      xRatioUV_super,
      yRatioUV_super,
      pixelsize_super,
      bits_per_pixel_super,
      mt_flag
    ));

    // Computes the SAD thresholds for this source frame, a cosine-shaped
    // smooth transition between thsad(c) and thsad(c)2.
    const int		d = k / 2 + 1;
    c_info._thsad = ClipFnc::interpolate_thsad(thsad, thsad2, d, _trad);
    c_info._thsadc = ClipFnc::interpolate_thsad(thsadc, thsadc2, d, _trad);
    //    c_info._thsad_sq = double(c_info._thsad) * double(c_info._thsad); // 2.7.46
    //    c_info._thsadc_sq = double(c_info._thsadc) * double(c_info._thsadc);
    c_info._thsad_sq = double(c_info._thsad);
    c_info._thsadc_sq = double(c_info._thsadc);
    for (int i = 0; i < _wpow - 1; i++)
    {
      c_info._thsad_sq *= double(c_info._thsad);
      c_info._thsadc_sq *= double(c_info._thsadc);
    }
  }

  const int nSuperWidth = vi_super.width;
  pixelsize_super_shift = ilog2(pixelsize_super);

  if (nHeight != nHeightS
    || nHeight != vi.height
    || nWidth != nSuperWidth - nSuperHPad * 2
    || nWidth != vi.width
    || nPel != nSuperPel)
  {
    env_ptr->ThrowError("MDegrainN : wrong source or super frame size");
  }

  if(lsb_flag && (pixelsize != 1 || pixelsize_super != 1))
    env_ptr->ThrowError("MDegrainN : lsb_flag only for 8 bit sources");

  if (out16_flag) {
    if (pixelsize != 1 || pixelsize_super != 1)
      env_ptr->ThrowError("MDegrainN : out16 flag only for 8 bit sources");
    if (!vi.IsY8() && !vi.IsYV12() && !vi.IsYV16() && !vi.IsYV24())
      env_ptr->ThrowError("MDegrainN : only YV8, YV12, YV16 or YV24 allowed for out16");
  }

  if (lsb_flag && out16_flag)
    env_ptr->ThrowError("MDegrainN : cannot specify both lsb and out16 flag");

  // output can be different bit depth from input
  pixelsize_output = pixelsize_super;
  bits_per_pixel_output = bits_per_pixel_super;
  pixelsize_output_shift = pixelsize_super_shift;
  if (out16_flag) {
    pixelsize_output = sizeof(uint16_t);
    bits_per_pixel_output = 16;
    pixelsize_output_shift = ilog2(pixelsize_output);
  }

  if ((pixelType & VideoInfo::CS_YUY2) == VideoInfo::CS_YUY2 && !_planar_flag)
  {
    _dst_planes = std::unique_ptr <YUY2Planes>(
      new YUY2Planes(nWidth, nHeight * _height_lsb_or_out16_mul)
      );
    _src_planes = std::unique_ptr <YUY2Planes>(
      new YUY2Planes(nWidth, nHeight)
      );
  }
  _dst_short_pitch = ((nWidth + 15) / 16) * 16;
  _dst_int_pitch = _dst_short_pitch;
  if (nOverlapX > 0 || nOverlapY > 0)
  {
    _overwins = std::unique_ptr <OverlapWindows>(
      new OverlapWindows(nBlkSizeX, nBlkSizeY, nOverlapX, nOverlapY)
      );
    _overwins_uv = std::unique_ptr <OverlapWindows>(new OverlapWindows(
      nBlkSizeX >> nLogxRatioUV_super, nBlkSizeY >> nLogyRatioUV_super,
      nOverlapX >> nLogxRatioUV_super, nOverlapY >> nLogyRatioUV_super
    ));
    if (_lsb_flag || pixelsize_output > 1)
    {
      _dst_int.resize(_dst_int_pitch * nHeight);
    }
    else
    {
      _dst_short.resize(_dst_short_pitch * nHeight);
    }
  }
  if (nOverlapY > 0)
  {
    _boundary_cnt_arr.resize(nBlkY);
  }

    // in overlaps.h
    // OverlapsLsbFunction
    // OverlapsFunction
    // in M(V)DegrainX: DenoiseXFunction
  arch_t arch;
  if ((_cpuFlags & CPUF_AVX2) != 0)
    arch = USE_AVX2;
  else if ((_cpuFlags & CPUF_AVX) != 0)
    arch = USE_AVX;
  else if ((_cpuFlags & CPUF_SSE4_1) != 0)
    arch = USE_SSE41;
  else if ((_cpuFlags & CPUF_SSE2) != 0)
    arch = USE_SSE2;
  else
    arch = NO_SIMD;

  SAD = get_sad_function(nBlkSizeX, nBlkSizeY, bits_per_pixel, arch);
  SADCHROMA = get_sad_function(nBlkSizeX / xRatioUV, nBlkSizeY / yRatioUV, bits_per_pixel, arch);

// C only -> NO_SIMD
  _oversluma_lsb_ptr = get_overlaps_lsb_function(nBlkSizeX, nBlkSizeY, sizeof(uint8_t), NO_SIMD);
  _overschroma_lsb_ptr = get_overlaps_lsb_function(nBlkSizeX / xRatioUV_super, nBlkSizeY / yRatioUV_super, sizeof(uint8_t), NO_SIMD);

  _oversluma_ptr = get_overlaps_function(nBlkSizeX, nBlkSizeY, sizeof(uint8_t), false, arch);
  _overschroma_ptr = get_overlaps_function(nBlkSizeX / xRatioUV_super, nBlkSizeY / yRatioUV_super, sizeof(uint8_t), false, arch);

  _oversluma16_ptr = get_overlaps_function(nBlkSizeX, nBlkSizeY, sizeof(uint16_t), false, arch);
  _overschroma16_ptr = get_overlaps_function(nBlkSizeX >> nLogxRatioUV_super, nBlkSizeY >> nLogyRatioUV_super, sizeof(uint16_t), false, arch);

  _oversluma32_ptr = get_overlaps_function(nBlkSizeX, nBlkSizeY, sizeof(float), false, arch);
  _overschroma32_ptr = get_overlaps_function(nBlkSizeX >> nLogxRatioUV_super, nBlkSizeY >> nLogyRatioUV_super, sizeof(float), false, arch);

  _degrainluma_ptr = get_denoiseN_function(nBlkSizeX, nBlkSizeY, bits_per_pixel_super, lsb_flag, out16_flag, arch);
  _degrainchroma_ptr = get_denoiseN_function(nBlkSizeX / xRatioUV_super, nBlkSizeY / yRatioUV_super, bits_per_pixel_super, lsb_flag, out16_flag, arch);

  if (!_oversluma_lsb_ptr)
    env_ptr->ThrowError("MDegrainN : no valid _oversluma_lsb_ptr function for %dx%d, pixelsize=%d, lsb_flag=%d", nBlkSizeX, nBlkSizeY, pixelsize_super, (int)lsb_flag);
  if (!_overschroma_lsb_ptr)
    env_ptr->ThrowError("MDegrainN : no valid _overschroma_lsb_ptr function for %dx%d, pixelsize=%d, lsb_flag=%d", nBlkSizeX, nBlkSizeY, pixelsize_super, (int)lsb_flag);
  if (!_oversluma_ptr)
    env_ptr->ThrowError("MDegrainN : no valid _oversluma_ptr function for %dx%d, pixelsize=%d, lsb_flag=%d", nBlkSizeX, nBlkSizeY, pixelsize_super, (int)lsb_flag);
  if (!_overschroma_ptr)
    env_ptr->ThrowError("MDegrainN : no valid _overschroma_ptr function for %dx%d, pixelsize=%d, lsb_flag=%d", nBlkSizeX, nBlkSizeY, pixelsize_super, (int)lsb_flag);
  if (!_degrainluma_ptr)
    env_ptr->ThrowError("MDegrainN : no valid _degrainluma_ptr function for %dx%d, pixelsize=%d, lsb_flag=%d", nBlkSizeX, nBlkSizeY, pixelsize_super, (int)lsb_flag);
  if (!_degrainchroma_ptr)
    env_ptr->ThrowError("MDegrainN : no valid _degrainchroma_ptr function for %dx%d, pixelsize=%d, lsb_flag=%d", nBlkSizeX, nBlkSizeY, pixelsize_super, (int)lsb_flag);

  if ((_cpuFlags & CPUF_SSE2) != 0)
  {
    if(out16_flag)
      LimitFunction = LimitChanges_src8_target16_c; // todo SSE2
    else if (pixelsize_super == 1)
      LimitFunction = LimitChanges_sse2_new<uint8_t, 0>;
    else if (pixelsize_super == 2) { // pixelsize_super == 2
      if ((_cpuFlags & CPUF_SSE4_1) != 0)
        LimitFunction = LimitChanges_sse2_new<uint16_t, 1>;
      else
        LimitFunction = LimitChanges_sse2_new<uint16_t, 0>;
    }
    else {
      LimitFunction = LimitChanges_float_c; // no SSE2
    }
  }
  else
  {
    if (out16_flag)
      LimitFunction = LimitChanges_src8_target16_c; // todo SSE2
    else if (pixelsize_super == 1)
      LimitFunction = LimitChanges_c<uint8_t>;
    else if (pixelsize_super == 2)
      LimitFunction = LimitChanges_c<uint16_t>;
    else
      LimitFunction = LimitChanges_float_c;
  }

  //---------- end of functions

  // 16 bit output hack
  if (_lsb_flag)
  {
    vi.height <<= 1;
  }

  if (out16_flag) {
    if (vi.IsY8())
      vi.pixel_type = VideoInfo::CS_Y16;
    else if (vi.IsYV12())
      vi.pixel_type = VideoInfo::CS_YUV420P16;
    else if (vi.IsYV16())
      vi.pixel_type = VideoInfo::CS_YUV422P16;
    else if (vi.IsYV24())
      vi.pixel_type = VideoInfo::CS_YUV444P16;
  }

  pui16Blocks2DWeightsArr = new uint16_t[(_trad + 1) * 2 * (nBlkSizeX*nBlkSizeY) * pixelsize_super * sizeof(uint16_t)]; // pixelsize already set ?
  pui16WeightsFrameArr = new uint16_t[(_trad + 1) * 2 * nBlkX *nBlkY * sizeof(uint16_t)];

  if ((fadjSADzeromv != 1.0f) || (fadjSADcohmv != 1.0f))
  {
    use_block_y_func = &MDegrainN::use_block_y_thSADzeromv_thSADcohmv;
    use_block_uv_func = &MDegrainN::use_block_uv_thSADzeromv_thSADcohmv;
  }
  else // old funcs
  {
    use_block_y_func = &MDegrainN::use_block_y;
    use_block_uv_func = &MDegrainN::use_block_uv;
  }

  // calculate MV LPF filter kernel (from fMVLPFCutoff and (in future) fMVLPFSlope params)
  // for interlaced field-based content the +-0.5 V shift should be added after filtering ?
  if (fMVLPFCutoff < 1.0f || fMVLPFGauss > 0.0f)
    bMVsAddProc = true;
  else
    bMVsAddProc = false;

  float fPi = 3.14159265f;
  int iKS_d2 = MVLPFKERNELSIZE / 2;

  if (fMVLPFGauss == 0.0f)
  {
    for (int i = 0; i < MVLPFKERNELSIZE; i++)
    {
      float fArg = (float)(i - iKS_d2) * fPi * fMVLPFCutoff;
      fMVLPFKernel[i] = fSinc(fArg);

      // Lanczos weighting
      float fArgLz = (float)(i - iKS_d2) * fPi / (float)(iKS_d2);
      fMVLPFKernel[i] *= fSinc(fArgLz);
    }
  }
  else // gaussian impulse kernel
  {
    for (int i = 0; i < MVLPFKERNELSIZE; i++)
    {
      float fArg = (float)(i - iKS_d2) * fMVLPFGauss;
      fMVLPFKernel[i] = powf(2.0, -fArg * fArg);
    }
  }

  float fSum = 0.0f;
  for (int i = 0; i < MVLPFKERNELSIZE; i++)
  {
    fSum += fMVLPFKernel[i];
  }

  for (int i = 0; i < MVLPFKERNELSIZE; i++)
  {
    fMVLPFKernel[i] /= fSum;
  }

  // allocate filtered MVs arrays
  for (int k = 0; k < _trad * 2; ++k)
  {
    uint8_t* pTmp_a;
    VECTOR* pTmp;
#ifdef _WIN32
    // to prevent cache set overloading when accessing fpob MVs arrays - add random L2L3_CACHE_LINE_SIZE-bytes sized offset to different allocations
    size_t random = rand();
    random *= RAND_OFFSET_MAX;
    random /= RAND_MAX;
    random *= L2L3_CACHE_LINE_SIZE;

    SIZE_T stSizeToAlloc = nBlkCount * sizeof(VECTOR) + RAND_OFFSET_MAX * L2L3_CACHE_LINE_SIZE;

    pTmp_a = (BYTE*)VirtualAlloc(0, stSizeToAlloc, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE); // 4KByte page aligned address
    pFilteredMVsPlanesArrays_a[k] = pTmp_a;
    pTmp = (VECTOR*)(pTmp_a + random);
#else
    pTmp = new VECTOR[nBlkCount]; // allocate in heap ?
    pFilteredMVsPlanesArrays_a[k] = pTmp;
#endif
    pFilteredMVsPlanesArrays[k] = pTmp;

  }

  // calculate limits of blx/bly once in constructor
  if (nUseSubShift == 0)
  {
    iMinBlx = -nBlkSizeX * nPel;
    iMaxBlx = nBlkSizeX * nBlkX * nPel;
    iMinBly = -nBlkSizeY * nPel;
    iMaxBly = nBlkSizeY * nBlkY * nPel;
  }
  else
  {
    int iKS_d2 = SHIFTKERNELSIZE_I16 / 2; // need to define current used kernel size for subshift - may be nUseSubShift value ?
    iMinBlx = (-nBlkSizeX + iKS_d2) * nPel;
    iMaxBlx = (nBlkSizeX * nBlkX - iKS_d2) * nPel;
    iMinBly = (-nBlkSizeY + iKS_d2) * nPel;
    iMaxBly = (nBlkSizeY * nBlkY - iKS_d2) * nPel;
  }

}



MDegrainN::~MDegrainN()
{
  // Nothing
  delete pui16Blocks2DWeightsArr;
  delete pui16WeightsFrameArr;
  for (int k = 0; k < _trad * 2; ++k)
  {
#ifdef _WIN32
    VirtualFree((LPVOID)pFilteredMVsPlanesArrays_a[k], 0, MEM_FREE);
#else
    delete pFilteredMVsPlanesArrays_a[k];
#endif
  }

}

static void plane_copy_8_to_16_c(uint8_t *dstp, int dstpitch, const uint8_t *srcp, int srcpitch, int width, int height)
{
  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
      reinterpret_cast<uint16_t *>(dstp)[x] = srcp[x] << 8;
    }
    dstp += dstpitch;
    srcp += srcpitch;
  }
}


::PVideoFrame __stdcall MDegrainN::GetFrame(int n, ::IScriptEnvironment* env_ptr)
{
  _covered_width = nBlkX * (nBlkSizeX - nOverlapX) + nOverlapX;
  _covered_height = nBlkY * (nBlkSizeY - nOverlapY) + nOverlapY;

  const BYTE * pRef[MAX_TEMP_RAD * 2][3];
  int nRefPitches[MAX_TEMP_RAD * 2][3];
  unsigned char *pDstYUY2;
  const unsigned char *pSrcYUY2;
  int nDstPitchYUY2;
  int nSrcPitchYUY2;

  for (int k2 = 0; k2 < _trad * 2; ++k2)
  {
    // reorder ror regular frames order in v2.0.9.2
    const int k = reorder_ref(k2);

    // v2.0.9.2 - it seems we do not need in vectors clip anymore when we
    // finished copying them to fakeblockdatas
    MVClip &mv_clip = *(_mv_clip_arr[k]._clip_sptr);
    ::PVideoFrame mv = mv_clip.GetFrame(n, env_ptr);
    mv_clip.Update(mv, env_ptr);
    _usable_flag_arr[k] = mv_clip.IsUsable();
  }

  PVideoFrame src = child->GetFrame(n, env_ptr);
  PVideoFrame dst = has_at_least_v8 ? env_ptr->NewVideoFrameP(vi, &src) : env_ptr->NewVideoFrame(vi); // frame property support

  if ((pixelType & VideoInfo::CS_YUY2) == VideoInfo::CS_YUY2)
  {
    if (!_planar_flag)
    {
      pDstYUY2 = dst->GetWritePtr();
      nDstPitchYUY2 = dst->GetPitch();
      _dst_ptr_arr[0] = _dst_planes->GetPtr();
      _dst_ptr_arr[1] = _dst_planes->GetPtrU();
      _dst_ptr_arr[2] = _dst_planes->GetPtrV();
      _dst_pitch_arr[0] = _dst_planes->GetPitch();
      _dst_pitch_arr[1] = _dst_planes->GetPitchUV();
      _dst_pitch_arr[2] = _dst_planes->GetPitchUV();

      pSrcYUY2 = src->GetReadPtr();
      nSrcPitchYUY2 = src->GetPitch();
      _src_ptr_arr[0] = _src_planes->GetPtr();
      _src_ptr_arr[1] = _src_planes->GetPtrU();
      _src_ptr_arr[2] = _src_planes->GetPtrV();
      _src_pitch_arr[0] = _src_planes->GetPitch();
      _src_pitch_arr[1] = _src_planes->GetPitchUV();
      _src_pitch_arr[2] = _src_planes->GetPitchUV();

      YUY2ToPlanes(
        pSrcYUY2, nSrcPitchYUY2, nWidth, nHeight,
        _src_ptr_arr[0], _src_pitch_arr[0],
        _src_ptr_arr[1], _src_ptr_arr[2], _src_pitch_arr[1],
        _cpuFlags
      );
    }
    else
    {
      _dst_ptr_arr[0] = dst->GetWritePtr();
      _dst_ptr_arr[1] = _dst_ptr_arr[0] + nWidth;
      _dst_ptr_arr[2] = _dst_ptr_arr[1] + nWidth / 2; //yuy2 xratio
      _dst_pitch_arr[0] = dst->GetPitch();
      _dst_pitch_arr[1] = _dst_pitch_arr[0];
      _dst_pitch_arr[2] = _dst_pitch_arr[0];
      _src_ptr_arr[0] = src->GetReadPtr();
      _src_ptr_arr[1] = _src_ptr_arr[0] + nWidth;
      _src_ptr_arr[2] = _src_ptr_arr[1] + nWidth / 2;
      _src_pitch_arr[0] = src->GetPitch();
      _src_pitch_arr[1] = _src_pitch_arr[0];
      _src_pitch_arr[2] = _src_pitch_arr[0];
    }
  }
  else
  {
    _dst_ptr_arr[0] = YWPLAN(dst);
    _dst_ptr_arr[1] = UWPLAN(dst);
    _dst_ptr_arr[2] = VWPLAN(dst);
    _dst_pitch_arr[0] = YPITCH(dst);
    _dst_pitch_arr[1] = UPITCH(dst);
    _dst_pitch_arr[2] = VPITCH(dst);
    _src_ptr_arr[0] = YRPLAN(src);
    _src_ptr_arr[1] = URPLAN(src);
    _src_ptr_arr[2] = VRPLAN(src);
    _src_pitch_arr[0] = YPITCH(src);
    _src_pitch_arr[1] = UPITCH(src);
    _src_pitch_arr[2] = VPITCH(src);
  }

//  DWORD dwOldProt;
//  BYTE* pbAVS = (BYTE*)_dst_ptr_arr[0];

  _lsb_offset_arr[0] = _dst_pitch_arr[0] * nHeight;
  _lsb_offset_arr[1] = _dst_pitch_arr[1] * (nHeight >> nLogyRatioUV_super);
  _lsb_offset_arr[2] = _dst_pitch_arr[2] * (nHeight >> nLogyRatioUV_super);

  if (_lsb_flag)
  {
    memset(_dst_ptr_arr[0] + _lsb_offset_arr[0], 0, _lsb_offset_arr[0]);
    if (!_planar_flag)
    {
      memset(_dst_ptr_arr[1] + _lsb_offset_arr[1], 0, _lsb_offset_arr[1]);
      memset(_dst_ptr_arr[2] + _lsb_offset_arr[2], 0, _lsb_offset_arr[2]);
    }
  }

  ::PVideoFrame ref[MAX_TEMP_RAD * 2];

  for (int k2 = 0; k2 < _trad * 2; ++k2)
  {
    // reorder ror regular frames order in v2.0.9.2
    const int k = reorder_ref(k2);
    MVClip &mv_clip = *(_mv_clip_arr[k]._clip_sptr);
    mv_clip.use_ref_frame(ref[k], _usable_flag_arr[k], _super, n, env_ptr);
  }

  if ((pixelType & VideoInfo::CS_YUY2) == VideoInfo::CS_YUY2)
  {
    for (int k2 = 0; k2 < _trad * 2; ++k2)
    {
      const int k = reorder_ref(k2);
      if (_usable_flag_arr[k])
      {
        pRef[k][0] = ref[k]->GetReadPtr();
        pRef[k][1] = pRef[k][0] + ref[k]->GetRowSize() / 2;
        pRef[k][2] = pRef[k][1] + ref[k]->GetRowSize() / 4;
        nRefPitches[k][0] = ref[k]->GetPitch();
        nRefPitches[k][1] = nRefPitches[k][0];
        nRefPitches[k][2] = nRefPitches[k][0];
      }
    }
  }
  else
  {
    for (int k2 = 0; k2 < _trad * 2; ++k2)
    {
      const int k = reorder_ref(k2);
      if (_usable_flag_arr[k])
      {
        pRef[k][0] = YRPLAN(ref[k]);
        pRef[k][1] = URPLAN(ref[k]);
        pRef[k][2] = VRPLAN(ref[k]);
        nRefPitches[k][0] = YPITCH(ref[k]);
        nRefPitches[k][1] = UPITCH(ref[k]);
        nRefPitches[k][2] = VPITCH(ref[k]);
      }
    }
  }

  memset(_planes_ptr, 0, _trad * 2 * sizeof(_planes_ptr[0]));

  for (int k2 = 0; k2 < _trad * 2; ++k2)
  {
    const int k = reorder_ref(k2);
    MVGroupOfFrames &gof = *(_mv_clip_arr[k]._gof_sptr);
    gof.Update(
      _yuvplanes,
      const_cast <BYTE *> (pRef[k][0]), nRefPitches[k][0],
      const_cast <BYTE *> (pRef[k][1]), nRefPitches[k][1],
      const_cast <BYTE *> (pRef[k][2]), nRefPitches[k][2]
    );
    if (_yuvplanes & YPLANE)
    {
      _planes_ptr[k][0] = gof.GetFrame(0)->GetPlane(YPLANE);
    }
    if (_yuvplanes & UPLANE)
    {
      _planes_ptr[k][1] = gof.GetFrame(0)->GetPlane(UPLANE);
    }
    if (_yuvplanes & VPLANE)
    {
      _planes_ptr[k][2] = gof.GetFrame(0)->GetPlane(VPLANE);
    }
  }

  // load pMVsArray into temp buf once, 2.7.46
  for (int k = 0; k < _trad * 2; ++k)
  {
    pMVsPlanesArrays[k] = _mv_clip_arr[k]._clip_sptr->GetpMVsArray(0);
  }

  //call Filter MVs here because it equal for luma and all chroma planes
//  const BYTE* pSrcCur = _src_ptr_arr[0] + td._y_beg * rowsize * _src_pitch_arr[0]; // P.F. why *rowsize? (*nBlkSizeY)

  if (bMVsAddProc)
  {
    FilterMVs();
  }

  PROFILE_START(MOTION_PROFILE_COMPENSATION);

  //-------------------------------------------------------------------------
  // LUMA plane Y

  if ((_yuvplanes & YPLANE) == 0)
  {
    if (_out16_flag) {
      // copy 8 bit source to 16bit target
      plane_copy_8_to_16_c(_dst_ptr_arr[0], _dst_pitch_arr[0],
        _src_ptr_arr[0], _src_pitch_arr[0],
        nWidth, nHeight);
    }
    else {
      BitBlt(
        _dst_ptr_arr[0], _dst_pitch_arr[0],
        _src_ptr_arr[0], _src_pitch_arr[0],
        nWidth << pixelsize_super_shift, nHeight
      );
    }
  }
  else
  {
    Slicer slicer(_mt_flag);

    if (nOverlapX == 0 && nOverlapY == 0)
    {
      {
        if (iSEWBWidth != 0)
        {
          CreateFrameWeightsArr_C();
          slicer.start(
            nBlkY,
            *this,
            &MDegrainN::process_luma_normal_slice_SEWB
          );
        }
        else
        {
          slicer.start(
            nBlkY,
            *this,
            &MDegrainN::process_luma_normal_slice
          );
        }
      }
      slicer.wait();
    }

    // Overlap
    else
    {
      uint16_t *pDstShort = (_dst_short.empty()) ? 0 : &_dst_short[0];
      int *pDstInt = (_dst_int.empty()) ? 0 : &_dst_int[0];

      if (_lsb_flag || pixelsize_output>1)
      {
        MemZoneSet(
          reinterpret_cast <unsigned char *> (pDstInt), 0,
          _covered_width * sizeof(int), _covered_height, 0, 0, _dst_int_pitch * sizeof(int)
        );
      }
      else
      {
        MemZoneSet(
          reinterpret_cast <unsigned char *> (pDstShort), 0,
          _covered_width * sizeof(short), _covered_height, 0, 0, _dst_short_pitch * sizeof(short)
        );
      }

      if (nOverlapY > 0)
      {
        memset(
          &_boundary_cnt_arr[0],
          0,
          _boundary_cnt_arr.size() * sizeof(_boundary_cnt_arr[0])
        );
      }

      slicer.start(
        nBlkY,
        *this,
        &MDegrainN::process_luma_overlap_slice,
        2
      );
      slicer.wait();
      // fixme: SSE versions from ShortToBytes family like in MDegrain3
      if (_lsb_flag)
      {
        Short2BytesLsb(
          _dst_ptr_arr[0],
          _dst_ptr_arr[0] + _lsb_offset_arr[0],
          _dst_pitch_arr[0],
          &_dst_int[0], _dst_int_pitch,
          _covered_width, _covered_height
        );
      }
      else if (_out16_flag)
      {
        Short2Bytes_Int32toWord16(
          (uint16_t *)_dst_ptr_arr[0], _dst_pitch_arr[0],
          &_dst_int[0], _dst_int_pitch,
          _covered_width, _covered_height,
          bits_per_pixel_output
        );
      }
      else if(pixelsize_super == 1)
      {
        Short2Bytes(
          _dst_ptr_arr[0], _dst_pitch_arr[0],
          &_dst_short[0], _dst_short_pitch,
          _covered_width, _covered_height
        );
      }
      else if (pixelsize_super == 2)
      {
        Short2Bytes_Int32toWord16(
          (uint16_t *)_dst_ptr_arr[0], _dst_pitch_arr[0],
          &_dst_int[0], _dst_int_pitch,
          _covered_width, _covered_height,
          bits_per_pixel_super
        );
      }
      else if (pixelsize_super == 4)
      {
        Short2Bytes_FloatInInt32ArrayToFloat(
          (float *)_dst_ptr_arr[0], _dst_pitch_arr[0],
          (float *)&_dst_int[0], _dst_int_pitch,
          _covered_width, _covered_height
        );
      }
      if (_covered_width < nWidth)
      {
        if (_out16_flag) {
          // copy 8 bit source to 16bit target
          plane_copy_8_to_16_c(_dst_ptr_arr[0] + (_covered_width << pixelsize_output_shift), _dst_pitch_arr[0],
            _src_ptr_arr[0] + _covered_width, _src_pitch_arr[0],
            nWidth - _covered_width, _covered_height
          );
        }
        else {
          BitBlt(
            _dst_ptr_arr[0] + (_covered_width << pixelsize_super_shift), _dst_pitch_arr[0],
            _src_ptr_arr[0] + (_covered_width << pixelsize_super_shift), _src_pitch_arr[0],
            (nWidth - _covered_width) << pixelsize_super_shift, _covered_height
          );
        }
      }
      if (_covered_height < nHeight) // bottom noncovered region
      {
        if (_out16_flag) {
          // copy 8 bit source to 16bit target
          plane_copy_8_to_16_c(_dst_ptr_arr[0] + _covered_height * _dst_pitch_arr[0], _dst_pitch_arr[0],
            _src_ptr_arr[0] + _covered_height * _src_pitch_arr[0], _src_pitch_arr[0],
            nWidth, nHeight - _covered_height
          );
        }
        else {
          BitBlt(
            _dst_ptr_arr[0] + _covered_height * _dst_pitch_arr[0], _dst_pitch_arr[0],
            _src_ptr_arr[0] + _covered_height * _src_pitch_arr[0], _src_pitch_arr[0],
            nWidth << pixelsize_super_shift, nHeight - _covered_height
          );
        }
      }
    }	// overlap - end

    if (_nlimit < 255)
    {
      // limit is 0-255 relative, for any bit depth
      float realLimit;
      if (pixelsize_output <= 2)
        realLimit = _nlimit * (1 << (bits_per_pixel_output - 8));
      else
        realLimit = _nlimit / 255.0f;
      LimitFunction(_dst_ptr_arr[0], _dst_pitch_arr[0],
        _src_ptr_arr[0], _src_pitch_arr[0],
        nWidth, nHeight,
        realLimit
      );
    }
  }

  //-------------------------------------------------------------------------
  // CHROMA planes

  process_chroma <1>(UPLANE & _nsupermodeyuv);
  process_chroma <2>(VPLANE & _nsupermodeyuv);

  //-------------------------------------------------------------------------

#ifndef _M_X64 
  _mm_empty(); // (we may use double-float somewhere) Fizick
#endif

  PROFILE_STOP(MOTION_PROFILE_COMPENSATION);

  if ((pixelType & VideoInfo::CS_YUY2) == VideoInfo::CS_YUY2 && !_planar_flag)
  {
    YUY2FromPlanes(
      pDstYUY2, nDstPitchYUY2, nWidth, nHeight * _height_lsb_or_out16_mul,
      _dst_ptr_arr[0], _dst_pitch_arr[0],
      _dst_ptr_arr[1], _dst_ptr_arr[2], _dst_pitch_arr[1], _cpuFlags);
  }

  return (dst);
}



// Fn...F1 B1...Bn
int MDegrainN::reorder_ref(int index) const
{
  assert(index >= 0);
  assert(index < _trad * 2);

  const int k = (index < _trad)
    ? (_trad - index) * 2 - 1
    : (index - _trad) * 2;

  return (k);
}



template <int P>
void	MDegrainN::process_chroma(int plane_mask)
{
  if ((_yuvplanes & plane_mask) == 0)
  {
    if (_out16_flag) {
      // copy 8 bit source to 16bit target
      plane_copy_8_to_16_c(_dst_ptr_arr[P], _dst_pitch_arr[P],
        _src_ptr_arr[P], _src_pitch_arr[P],
        nWidth >> nLogxRatioUV_super, nHeight >> nLogyRatioUV_super
      );
    }
    else {
      BitBlt(
        _dst_ptr_arr[P], _dst_pitch_arr[P],
        _src_ptr_arr[P], _src_pitch_arr[P],
        (nWidth >> nLogxRatioUV_super) << pixelsize_super_shift, nHeight >> nLogyRatioUV_super
      );
    }
  }

  else
  {
    Slicer slicer(_mt_flag);

    if (nOverlapX == 0 && nOverlapY == 0)
    {
      slicer.start(
        nBlkY,
        *this,
        &MDegrainN::process_chroma_normal_slice <P>
      );
      slicer.wait();
    }

    // Overlap
    else
    {
      uint16_t * pDstShort = (_dst_short.empty()) ? 0 : &_dst_short[0];
      int * pDstInt = (_dst_int.empty()) ? 0 : &_dst_int[0];

      if (_lsb_flag || pixelsize_output > 1)
      {
        MemZoneSet(
          reinterpret_cast <unsigned char *> (pDstInt), 0,
          (_covered_width * sizeof(int)) >> nLogxRatioUV_super, _covered_height >> nLogyRatioUV_super,
          0, 0, _dst_int_pitch * sizeof(int)
        );
      }
      else
      {  
        MemZoneSet(
          reinterpret_cast <unsigned char *> (pDstShort), 0,
          (_covered_width * sizeof(short)) >> nLogxRatioUV_super, _covered_height >> nLogyRatioUV_super,
          0, 0, _dst_short_pitch * sizeof(short)
        );
      }

      if (nOverlapY > 0)
      {
        memset(
          &_boundary_cnt_arr[0],
          0,
          _boundary_cnt_arr.size() * sizeof(_boundary_cnt_arr[0])
        );
      }

      slicer.start(
        nBlkY,
        *this,
        &MDegrainN::process_chroma_overlap_slice <P>,
        2
      );
      slicer.wait();
      
      if (_lsb_flag)
      {
        Short2BytesLsb(
          _dst_ptr_arr[P],
          _dst_ptr_arr[P] + _lsb_offset_arr[P], // 8 bit only
          _dst_pitch_arr[P],
          &_dst_int[0], _dst_int_pitch,
          _covered_width >> nLogxRatioUV_super, _covered_height >> nLogyRatioUV_super
        );
      }
      else if (_out16_flag)
      {
        Short2Bytes_Int32toWord16(
          (uint16_t *)_dst_ptr_arr[P], _dst_pitch_arr[P],
          &_dst_int[0], _dst_int_pitch,
          _covered_width >> nLogxRatioUV_super, _covered_height >> nLogyRatioUV_super,
          bits_per_pixel_output
        );
      }
      else if (pixelsize_super == 1)
      {
        Short2Bytes(
          _dst_ptr_arr[P], _dst_pitch_arr[P],
          &_dst_short[0], _dst_short_pitch,
          _covered_width >> nLogxRatioUV_super, _covered_height >> nLogyRatioUV_super
        );
      }
      else if (pixelsize_super == 2)
      {
        Short2Bytes_Int32toWord16(
          (uint16_t *)_dst_ptr_arr[P], _dst_pitch_arr[P],
          &_dst_int[0], _dst_int_pitch,
          _covered_width >> nLogxRatioUV_super, _covered_height >> nLogyRatioUV_super,
          bits_per_pixel_super
        );
      }
      else if (pixelsize_super == 4)
      {
        Short2Bytes_FloatInInt32ArrayToFloat(
          (float *)_dst_ptr_arr[P], _dst_pitch_arr[P],
          (float *)&_dst_int[0], _dst_int_pitch,
          _covered_width >> nLogxRatioUV_super, _covered_height >> nLogyRatioUV_super
        );
      }

      if (_covered_width < nWidth)
      {
        if (_out16_flag) {
          // copy 8 bit source to 16bit target
          plane_copy_8_to_16_c(_dst_ptr_arr[P] + ((_covered_width >> nLogxRatioUV_super) << pixelsize_output_shift), _dst_pitch_arr[P],
            _src_ptr_arr[P] + (_covered_width >> nLogxRatioUV_super), _src_pitch_arr[P],
            (nWidth - _covered_width) >> nLogxRatioUV_super, _covered_height >> nLogyRatioUV_super
          );
        }
        else {
          BitBlt(
            _dst_ptr_arr[P] + ((_covered_width >> nLogxRatioUV_super) << pixelsize_super_shift), _dst_pitch_arr[P],
            _src_ptr_arr[P] + ((_covered_width >> nLogxRatioUV_super) << pixelsize_super_shift), _src_pitch_arr[P],
            ((nWidth - _covered_width) >> nLogxRatioUV_super) << pixelsize_super_shift, _covered_height >> nLogyRatioUV_super
          );
        }
      }
      if (_covered_height < nHeight) // bottom noncovered region
      {
        if (_out16_flag) {
          // copy 8 bit source to 16bit target
          plane_copy_8_to_16_c(_dst_ptr_arr[P] + ((_dst_pitch_arr[P] * _covered_height) >> nLogyRatioUV_super), _dst_pitch_arr[P],
            _src_ptr_arr[P] + ((_src_pitch_arr[P] * _covered_height) >> nLogyRatioUV_super), _src_pitch_arr[P],
            nWidth >> nLogxRatioUV_super, ((nHeight - _covered_height) >> nLogyRatioUV_super)
          );
        }
        else {
          BitBlt(
            _dst_ptr_arr[P] + ((_dst_pitch_arr[P] * _covered_height) >> nLogyRatioUV_super), _dst_pitch_arr[P],
            _src_ptr_arr[P] + ((_src_pitch_arr[P] * _covered_height) >> nLogyRatioUV_super), _src_pitch_arr[P],
            (nWidth >> nLogxRatioUV_super) << pixelsize_super_shift, ((nHeight - _covered_height) >> nLogyRatioUV_super)
          );
        }
      }
    } // overlap - end

    if (_nlimitc < 255)
    {
      // limit is 0-255 relative, for any bit depth
      float realLimit;
      if (pixelsize_output <= 2)
        realLimit = _nlimitc * (1 << (bits_per_pixel_output - 8));
      else
        realLimit = (float)_nlimitc / 255.0f;
      LimitFunction(_dst_ptr_arr[P], _dst_pitch_arr[P],
        _src_ptr_arr[P], _src_pitch_arr[P],
        nWidth >> nLogxRatioUV_super, nHeight >> nLogyRatioUV_super,
        realLimit
      );
    }
  }
}

void	MDegrainN::process_luma_normal_slice(Slicer::TaskData &td)
{
  assert(&td != 0);

  const int rowsize = nBlkSizeY;
  BYTE *pDstCur = _dst_ptr_arr[0] + td._y_beg * rowsize * _dst_pitch_arr[0]; // P.F. why *rowsize? (*nBlkSizeY)
  const BYTE *pSrcCur = _src_ptr_arr[0] + td._y_beg * rowsize * _src_pitch_arr[0]; // P.F. why *rowsize? (*nBlkSizeY)

  for (int by = td._y_beg; by < td._y_end; ++by)
  {
    int xx = 0; // logical offset. Mul by 2 for pixelsize_super==2. Don't mul for indexing int* array

/*    int iIdxBlk_row_start = by * nBlkX;
    for (int k = 0; k < _trad * 2; ++k)
    {
      // prefetch all vectors for all ref planes all blocks of row in linear reading
      const VECTOR* pMVsArrayPref = pMVsPlanesArrays[k];
      HWprefetch_T0((char*)pMVsArrayPref, nBlkX * sizeof(VECTOR));

/*      // prefetch all rows of all ref blocks in linear lines of rows reading, use first MV in a row now as offset
      const int blx = 0 + pMVsArrayPref[iIdxBlk_row_start].x; // or zero ??
      const int bly = by * (nBlkSizeY - nOverlapY) * nPel + pMVsArrayPref[iIdxBlk_row_start].y;
      const BYTE* p = _planes_ptr[k][0]->GetPointer(blx, bly);
      int np = _planes_ptr[k][0]->GetPitch();

      for (int iH = 0; iH < nBlkSizeY; ++iH)
      {
        HWprefetch_T1((char*)p + np * iH, nBlkX * nBlkSizeX);
      } 
    }
    */
    // prefetch source full row in linear lines reading
    for (int iH = 0; iH < nBlkSizeY; ++iH)
    {
      HWprefetch_T1((char*)pSrcCur + _src_pitch_arr[0] * iH, nBlkX * nBlkSizeX);
    }

    for (int bx = 0; bx < nBlkX; ++bx)
    {
      int i = by * nBlkX + bx;
      const BYTE *ref_data_ptr_arr[MAX_TEMP_RAD * 2];
      int pitch_arr[MAX_TEMP_RAD * 2];
      int weight_arr[1 + MAX_TEMP_RAD * 2];

      PrefetchMVs(i);
      /*
      if ((i % 5) == 0) // do not prefetch each block - the 12bytes VECTOR sit about 5 times in the 64byte cache line 
      {
        if (bMVsAddProc)
        {
          for (int k = 0; k < _trad * 2; ++k)
          {
            const VECTOR* pMVsArrayPref = pMVsPlanesArrays[k];
            _mm_prefetch(const_cast<const CHAR*>(reinterpret_cast<const CHAR*>(&pMVsArrayPref[i + 5])), _MM_HINT_T0);
          }
        }
        else
        {
          for (int k = 0; k < _trad * 2; ++k)
          {
            const VECTOR* pMVsArrayPref = pFilteredMVsPlanesArrays[k];
            _mm_prefetch(const_cast<const CHAR*>(reinterpret_cast<const CHAR*>(&pMVsArrayPref[i + 5])), _MM_HINT_T0);
          }
        }
      }*/
/*
      int iCacheLine = CACHE_LINE_SIZE / nBlkSizeX;

      if ((bx % iCacheLine) == 0) // try to prefetch each next cacheline ??
      // try to prefetch set of next ref blocks
        if (bx < nBlkX - iCacheLine)
        {
          for (int k = 0; k < _trad * 2; ++k)
          {
            const VECTOR* pMVsArrayPref = pMVsPlanesArrays[k];
            const int blx = (bx + iCacheLine) * (nBlkSizeX - nOverlapX) * nPel + pMVsArrayPref[i + iCacheLine].x;
            const int bly = by * (nBlkSizeY - nOverlapY) * nPel + pMVsArrayPref[i + iCacheLine].y;
            const BYTE* p = _planes_ptr[k][0]->GetPointer(blx, bly);
            int np = _planes_ptr[k][0]->GetPitch();

            for (int iH = 0; iH < nBlkSizeY; ++iH)
            {
              _mm_prefetch(const_cast<const CHAR*>((const char*)p + np * iH), _MM_HINT_T1);
            }
          }
        }
        */
      for (int k = 0; k < _trad * 2; ++k)
      {
        if (!bMVsAddProc)
        {
          (this->*use_block_y_func)(
            ref_data_ptr_arr[k],
            pitch_arr[k],
            weight_arr[k + 1],
            _usable_flag_arr[k],
            _mv_clip_arr[k],
            i,
            _planes_ptr[k][0],
            pSrcCur,
            xx << pixelsize_super_shift,
            _src_pitch_arr[0],
            bx,
            by,
            pMVsPlanesArrays[k]
            );
        }
        else
        {
          (this->*use_block_y_func)(
            ref_data_ptr_arr[k],
            pitch_arr[k],
            weight_arr[k + 1],
            _usable_flag_arr[k],
            _mv_clip_arr[k],
            i,
            _planes_ptr[k][0],
            pSrcCur,
            xx << pixelsize_super_shift,
            _src_pitch_arr[0],
            bx,
            by,
            (const VECTOR*)pFilteredMVsPlanesArrays[k]
            );
        }
      }

      norm_weights(weight_arr, _trad);

      // luma
      _degrainluma_ptr(
        pDstCur + (xx << pixelsize_output_shift), pDstCur + _lsb_offset_arr[0] + (xx << pixelsize_super_shift), _dst_pitch_arr[0],
        pSrcCur + (xx << pixelsize_super_shift), _src_pitch_arr[0],
        ref_data_ptr_arr, pitch_arr, weight_arr, _trad
      );

      xx += (nBlkSizeX); // xx: indexing offset

      if (bx == nBlkX - 1 && _covered_width < nWidth) // right non-covered region
      {
        if (_out16_flag) {
          // copy 8 bit source to 16bit target
          plane_copy_8_to_16_c(
            pDstCur + (_covered_width << pixelsize_output_shift), _dst_pitch_arr[0],
            pSrcCur + (_covered_width << pixelsize_super_shift), _src_pitch_arr[0],
            nWidth - _covered_width, nBlkSizeY
          );
        }
        else {
          // luma
          BitBlt(
            pDstCur + (_covered_width << pixelsize_super_shift), _dst_pitch_arr[0],
            pSrcCur + (_covered_width << pixelsize_super_shift), _src_pitch_arr[0],
            (nWidth - _covered_width) << pixelsize_super_shift, nBlkSizeY);
        }
      }
    }	// for bx

    pDstCur += rowsize * _dst_pitch_arr[0];
    pSrcCur += rowsize * _src_pitch_arr[0];

    if (by == nBlkY - 1 && _covered_height < nHeight) // bottom uncovered region
    {
      // luma
      if (_out16_flag) {
        // copy 8 bit source to 16bit target
        plane_copy_8_to_16_c(
          pDstCur, _dst_pitch_arr[0],
          pSrcCur, _src_pitch_arr[0],
          nWidth, nHeight - _covered_height
        );
      }
      else {
        BitBlt(
          pDstCur, _dst_pitch_arr[0],
          pSrcCur, _src_pitch_arr[0],
          nWidth << pixelsize_super_shift, nHeight - _covered_height
        );
      }
    }
  }	// for by

}


void	MDegrainN::process_luma_normal_slice_SEWB(Slicer::TaskData& td)
{
  assert(&td != 0);

  const int rowsize = nBlkSizeY;
  BYTE* pDstCur = _dst_ptr_arr[0] + td._y_beg * rowsize * _dst_pitch_arr[0]; // P.F. why *rowsize? (*nBlkSizeY)
  const BYTE* pSrcCur = _src_ptr_arr[0] + td._y_beg * rowsize * _src_pitch_arr[0]; // P.F. why *rowsize? (*nBlkSizeY)

  for (int by = td._y_beg; by < td._y_end; ++by)
  {
    int xx = 0; // logical offset. Mul by 2 for pixelsize_super==2. Don't mul for indexing int* array

    // prefetch source full row in linear lines reading
    for (int iH = 0; iH < nBlkSizeY; ++iH)
    {
      HWprefetch_T1((char*)pSrcCur + _src_pitch_arr[0] * iH, nBlkX * nBlkSizeX);
    }

    for (int bx = 0; bx < nBlkX; ++bx)
    {
      int i = by * nBlkX + bx;
      const BYTE* ref_data_ptr_arr[MAX_TEMP_RAD * 2];
      int pitch_arr[MAX_TEMP_RAD * 2];
//      int weight_arr[1 + MAX_TEMP_RAD * 2];

      PrefetchMVs(i);

      for (int k = 0; k < _trad * 2; ++k)
      {
        VECTOR* pMVsArray;
        
        if (_usable_flag_arr[k])
        {
          if (!bMVsAddProc)
            pMVsArray = (VECTOR*)pMVsPlanesArrays[k];
          else
            pMVsArray = pFilteredMVsPlanesArrays[k];
          const int blx = bx * (nBlkSizeX - nOverlapX) * nPel + pMVsArray[i].x;
          const int bly = by * (nBlkSizeY - nOverlapY) * nPel + pMVsArray[i].y;
          ref_data_ptr_arr[k] = _planes_ptr[k][0]->GetPointer(blx, bly);
          pitch_arr[k] = _planes_ptr[k][0]->GetPitch();
        }
        else
        {
          ref_data_ptr_arr[k] = pSrcCur + (xx << pixelsize_super_shift); 
          pitch_arr[k] = _src_pitch_arr[0];
        } 
      }

      /*
      // temp copy weights from global weights arr to test
      const int nbr_frames = _trad * 2 + 1;
      uint16_t* pDst = pui16WeightsFrameArr + (bx + by * nBlkX) * nbr_frames; // norm directly to global array

      for (int k = 0; k < nbr_frames; ++k)
      {

        weight_arr[k] = pDst[k];
      }
      */
      CreateBlocks2DWeightsArr(bx, by);

      /*
      // luma
      _degrainluma_ptr(
        pDstCur + (xx << pixelsize_output_shift), pDstCur + _lsb_offset_arr[0] + (xx << pixelsize_super_shift), _dst_pitch_arr[0],
        pSrcCur + (xx << pixelsize_super_shift), _src_pitch_arr[0],
        ref_data_ptr_arr, pitch_arr, weight_arr, _trad
      );
      */
      DegrainN_sse2_SEWB<8,8,0>(
        pDstCur + (xx << pixelsize_output_shift), pDstCur + _lsb_offset_arr[0] + (xx << pixelsize_super_shift), _dst_pitch_arr[0],
        pSrcCur + (xx << pixelsize_super_shift), _src_pitch_arr[0],
        ref_data_ptr_arr, pitch_arr, pui16Blocks2DWeightsArr, _trad
      );

      xx += (nBlkSizeX); // xx: indexing offset

      if (bx == nBlkX - 1 && _covered_width < nWidth) // right non-covered region
      {
        if (_out16_flag) {
          // copy 8 bit source to 16bit target
          plane_copy_8_to_16_c(
            pDstCur + (_covered_width << pixelsize_output_shift), _dst_pitch_arr[0],
            pSrcCur + (_covered_width << pixelsize_super_shift), _src_pitch_arr[0],
            nWidth - _covered_width, nBlkSizeY
          );
        }
        else {
          // luma
          BitBlt(
            pDstCur + (_covered_width << pixelsize_super_shift), _dst_pitch_arr[0],
            pSrcCur + (_covered_width << pixelsize_super_shift), _src_pitch_arr[0],
            (nWidth - _covered_width) << pixelsize_super_shift, nBlkSizeY);
        }
      }
    }	// for bx

    pDstCur += rowsize * _dst_pitch_arr[0];
    pSrcCur += rowsize * _src_pitch_arr[0];

    if (by == nBlkY - 1 && _covered_height < nHeight) // bottom uncovered region
    {
      // luma
      if (_out16_flag) {
        // copy 8 bit source to 16bit target
        plane_copy_8_to_16_c(
          pDstCur, _dst_pitch_arr[0],
          pSrcCur, _src_pitch_arr[0],
          nWidth, nHeight - _covered_height
        );
      }
      else {
        BitBlt(
          pDstCur, _dst_pitch_arr[0],
          pSrcCur, _src_pitch_arr[0],
          nWidth << pixelsize_super_shift, nHeight - _covered_height
        );
      }
    }
  }	// for by

}


void	MDegrainN::process_luma_overlap_slice(Slicer::TaskData &td)
{
  assert(&td != 0);

  if (nOverlapY == 0
    || (td._y_beg == 0 && td._y_end == nBlkY))
  {
    process_luma_overlap_slice(td._y_beg, td._y_end);
  }

  else
  {
    assert(td._y_end - td._y_beg >= 2);

    process_luma_overlap_slice(td._y_beg, td._y_end - 1);

    const conc::AioAdd <int>	inc_ftor(+1);

    const int cnt_top = conc::AtomicIntOp::exec_new(
      _boundary_cnt_arr[td._y_beg],
      inc_ftor
    );
    if (td._y_beg > 0 && cnt_top == 2)
    {
      process_luma_overlap_slice(td._y_beg - 1, td._y_beg);
    }

    int cnt_bot = 2;
    if (td._y_end < nBlkY)
    {
      cnt_bot = conc::AtomicIntOp::exec_new(
        _boundary_cnt_arr[td._y_end],
        inc_ftor
      );
    }
    if (cnt_bot == 2)
    {
      process_luma_overlap_slice(td._y_end - 1, td._y_end);
    }
  }
}



void	MDegrainN::process_luma_overlap_slice(int y_beg, int y_end)
{
  TmpBlock       tmp_block;

  int iDivCL = CACHE_LINE_SIZE / nBlkSizeX;

  const int      rowsize = nBlkSizeY - nOverlapY;
  const BYTE *   pSrcCur = _src_ptr_arr[0] + y_beg * rowsize * _src_pitch_arr[0];

  uint16_t * pDstShort = (_dst_short.empty()) ? 0 : &_dst_short[0] + y_beg * rowsize * _dst_short_pitch;
  int *pDstInt = (_dst_int.empty()) ? 0 : &_dst_int[0] + y_beg * rowsize * _dst_int_pitch;
  const int tmpPitch = nBlkSizeX;
  assert(tmpPitch <= TmpBlock::MAX_SIZE);

  for (int by = y_beg; by < y_end; ++by)
  {
    // indexing overlap windows weighting table: top=0 middle=3 bottom=6
    /*
    0 = Top Left    1 = Top Middle    2 = Top Right
    3 = Middle Left 4 = Middle Middle 5 = Middle Right
    6 = Bottom Left 7 = Bottom Middle 8 = Bottom Right
    */

    int wby = (by == 0) ? 0 * 3 : (by == nBlkY - 1) ? 2 * 3 : 1 * 3; // 0 for very first, 2*3 for very last, 1*3 for all others in the middle
    int xx = 0; // logical offset. Mul by 2 for pixelsize_super==2. Don't mul for indexing int* array

    // prefetch source full row in linear lines reading
    for (int iH = 0; iH < nBlkSizeY; ++iH)
    {
      HWprefetch_T1((char*)pSrcCur + _src_pitch_arr[0] * iH, nBlkX * nBlkSizeX);
    }

    for (int bx = 0; bx < nBlkX; ++bx)
    {
      // select window
      // indexing overlap windows weighting table: left=+0 middle=+1 rightmost=+2
      int wbx = (bx == 0) ? 0 : (bx == nBlkX - 1) ? 2 : 1; // 0 for very first, 2 for very last, 1 for all others in the middle
      short *winOver = _overwins->GetWindow(wby + wbx);

      int i = by * nBlkX + bx;
      const BYTE *ref_data_ptr_arr[MAX_TEMP_RAD * 2];
      int pitch_arr[MAX_TEMP_RAD * 2];
      int weight_arr[1 + MAX_TEMP_RAD * 2];

      PrefetchMVs(i);
      /*
      if ((i % 5) == 0) // do not prefetch each block - the 12bytes VECTOR sit about 5 times in the 64byte cache line 
      {
        for (int k = 0; k < _trad * 2; ++k)
        {
          const VECTOR* pMVsArrayPref = pMVsPlanesArrays[k];
          _mm_prefetch(const_cast<const CHAR*>(reinterpret_cast<const CHAR*>(&pMVsArrayPref[i + 5])), _MM_HINT_T0);
        } 
      }
      */
/*      // pref next ref blocks (try each 64byte cacheline)
      if ((i % iDivCL) == 0)
      {
        for (int k = 0; k < _trad * 2; ++k)
        {
          if (_usable_flag_arr[k])
          {
            const VECTOR* pMVsArrayPref = pMVsPlanesArrays[k];
            // prefetch all rows of all ref blocks in linear lines of rows reading, use first MV in a row now as offset
            const int blx = bx * (nBlkSizeX - nOverlapX) * nPel + pMVsArrayPref[i + 1].x; // or zero ??
            const int bly = by * (nBlkSizeY - nOverlapY) * nPel + pMVsArrayPref[i + 1].y;
            const BYTE* p = _planes_ptr[k][0]->GetPointer(blx, bly);
            int np = _planes_ptr[k][0]->GetPitch();

            for (int iH = 0; iH < nBlkSizeY; ++iH)
            {
              SWprefetch((char*)p + np * iH, nBlkSizeX);
            }
          }
        }
      }
      */
      for (int k = 0; k < _trad * 2; ++k)
      {
        if (!bMVsAddProc)
        {
          (this->*use_block_y_func)(
            ref_data_ptr_arr[k],
            pitch_arr[k],
            weight_arr[k + 1],
            _usable_flag_arr[k],
            _mv_clip_arr[k],
            i,
            _planes_ptr[k][0],
            pSrcCur,
            xx << pixelsize_super_shift,
            _src_pitch_arr[0],
            bx,
            by,
            pMVsPlanesArrays[k]
            );
        }
        else
        {
          (this->*use_block_y_func)(
            ref_data_ptr_arr[k],
            pitch_arr[k],
            weight_arr[k + 1],
            _usable_flag_arr[k],
            _mv_clip_arr[k],
            i,
            _planes_ptr[k][0],
            pSrcCur,
            xx << pixelsize_super_shift,
            _src_pitch_arr[0],
            bx,
            by,
            (const VECTOR*)pFilteredMVsPlanesArrays[k]
            );
        }
      }

      norm_weights(weight_arr, _trad);

      // luma
      _degrainluma_ptr(
        &tmp_block._d[0], tmp_block._lsb_ptr, tmpPitch << pixelsize_output_shift,
        pSrcCur + (xx << pixelsize_super_shift), _src_pitch_arr[0],
        ref_data_ptr_arr, pitch_arr, weight_arr, _trad
      );
      if (_lsb_flag)
      {
        _oversluma_lsb_ptr(
          pDstInt + xx, _dst_int_pitch,
          &tmp_block._d[0], tmp_block._lsb_ptr, tmpPitch,
          winOver, nBlkSizeX
        );
      }
      else if (_out16_flag)
      {
        // cast to match the prototype
        _oversluma16_ptr((uint16_t *)(pDstInt + xx), _dst_int_pitch, &tmp_block._d[0], tmpPitch << pixelsize_output_shift, winOver, nBlkSizeX);
      }
      else if (pixelsize_super == 1)
      {
        _oversluma_ptr(
          pDstShort + xx, _dst_short_pitch,
          &tmp_block._d[0], tmpPitch,
          winOver, nBlkSizeX
        );
      }
      else if (pixelsize_super == 2) {
        _oversluma16_ptr((uint16_t *)(pDstInt + xx), _dst_int_pitch, &tmp_block._d[0], tmpPitch << pixelsize_super_shift, winOver, nBlkSizeX);
      }
      else { // pixelsize_super == 4
        _oversluma32_ptr((uint16_t *)(pDstInt + xx), _dst_int_pitch, &tmp_block._d[0], tmpPitch << pixelsize_super_shift, winOver, nBlkSizeX);
      }

      xx += nBlkSizeX - nOverlapX;
    } // for bx

    pSrcCur += rowsize * _src_pitch_arr[0]; // byte pointer
    pDstShort += rowsize * _dst_short_pitch; // short pointer
    pDstInt += rowsize * _dst_int_pitch; // int pointer
  } // for by

}

template <int P>
void	MDegrainN::process_chroma_normal_slice(Slicer::TaskData &td)
{
  assert(&td != 0);
  const int rowsize = nBlkSizeY >> nLogyRatioUV_super; // bad name. it's height really
  BYTE *pDstCur = _dst_ptr_arr[P] + td._y_beg * rowsize * _dst_pitch_arr[P];
  const BYTE *pSrcCur = _src_ptr_arr[P] + td._y_beg * rowsize * _src_pitch_arr[P];

  int effective_nSrcPitch = (nBlkSizeY >> nLogyRatioUV_super) * _src_pitch_arr[P]; // pitch is byte granularity
  int effective_nDstPitch = (nBlkSizeY >> nLogyRatioUV_super) * _dst_pitch_arr[P]; // pitch is short granularity

  for (int by = td._y_beg; by < td._y_end; ++by)
  {
    int xx = 0; // index

    int iIdxBlk_row_start = by * nBlkX;
/*    for (int k = 0; k < _trad * 2; ++k)
    {
      // prefetch all vectors for all ref planes all blocks of row in linear reading
      const VECTOR* pMVsArrayPref = pMVsPlanesArrays[k];
      HWprefetch_T0((char*)pMVsArrayPref, nBlkX * sizeof(VECTOR));

      // prefetch all rows of all ref blocks in linear lines of rows reading, use first MV in a row now as offset
      const int blx = 0 + pMVsArrayPref[iIdxBlk_row_start].x; // or zero ??
      const int bly = by * (nBlkSizeY - nOverlapY) * nPel + pMVsArrayPref[iIdxBlk_row_start].y;
      const BYTE* p = _planes_ptr[k][P]->GetPointer(blx, bly);
      int np = _planes_ptr[k][P]->GetPitch();

      for (int iH = 0; iH < nBlkSizeY; ++iH)
      {
        HWprefetch_T1((char*)p + np * iH, nBlkX * nBlkSizeX);
      }
    }
    */
    // prefetch source full row in linear lines reading
    for (int iH = 0; iH < nBlkSizeY; ++iH)
    {
      HWprefetch_T1((char*)pSrcCur + _src_pitch_arr[0] * iH, nBlkX * nBlkSizeX);
    }

    for (int bx = 0; bx < nBlkX; ++bx)
    {
      int i = by * nBlkX + bx;
      const BYTE *ref_data_ptr_arr[MAX_TEMP_RAD * 2]; // vs: const uint8_t *pointers[radius * 2]; // Moved by the degrain function. 
      int pitch_arr[MAX_TEMP_RAD * 2];
      int weight_arr[1 + MAX_TEMP_RAD * 2]; // 0th is special. vs:int WSrc, WRefs[radius * 2];

      for (int k = 0; k < _trad * 2; ++k)
      {
        if (!bMVsAddProc)
        {
          (this->*use_block_uv_func)(
            ref_data_ptr_arr[k],
            pitch_arr[k],
            weight_arr[k + 1],
            _usable_flag_arr[k],
            _mv_clip_arr[k],
            i,
            _planes_ptr[k][P],
            pSrcCur,
            xx << pixelsize_super_shift, // the pointer increment inside knows that xx later here is incremented with nBlkSize and not nBlkSize>>_xRatioUV
                // todo: copy from MDegrainX. Here we shift, and incement with nBlkSize>>_xRatioUV
            _src_pitch_arr[P],
            bx,
            by,
            pMVsPlanesArrays[k]
            ); // vs: extra nLogPel, plane, xSubUV, ySubUV, thSAD
        }
        else
        {
          (this->*use_block_uv_func)(
            ref_data_ptr_arr[k],
            pitch_arr[k],
            weight_arr[k + 1],
            _usable_flag_arr[k],
            _mv_clip_arr[k],
            i,
            _planes_ptr[k][P],
            pSrcCur,
            xx << pixelsize_super_shift, // the pointer increment inside knows that xx later here is incremented with nBlkSize and not nBlkSize>>_xRatioUV
                // todo: copy from MDegrainX. Here we shift, and incement with nBlkSize>>_xRatioUV
            _src_pitch_arr[P],
            bx,
            by,
            (const VECTOR*)pFilteredMVsPlanesArrays[k]
            ); // vs: extra nLogPel, plane, xSubUV, ySubUV, thSAD
        }
      }

      norm_weights(weight_arr, _trad); // normaliseWeights<radius>(WSrc, WRefs);


      // chroma
      _degrainchroma_ptr(
        pDstCur + (xx << pixelsize_output_shift),
        pDstCur + (xx << pixelsize_super_shift) + _lsb_offset_arr[P], _dst_pitch_arr[P],
        pSrcCur + (xx << pixelsize_super_shift), _src_pitch_arr[P],
        ref_data_ptr_arr, pitch_arr, weight_arr, _trad
      );

      //if (nLogxRatioUV != nLogxRatioUV_super) // orphaned if. chroma processing failed between 2.7.1-2.7.20
      //xx += nBlkSizeX; // blksize of Y plane, that's why there is xx >> xRatioUVlog above
      xx += (nBlkSizeX >> nLogxRatioUV_super); // xx: indexing offset

      if (bx == nBlkX - 1 && _covered_width < nWidth) // right non-covered region
      {
        // chroma
        if (_out16_flag) {
          // copy 8 bit source to 16bit target
          plane_copy_8_to_16_c(
            pDstCur + ((_covered_width >> nLogxRatioUV_super) << pixelsize_output_shift), _dst_pitch_arr[P],
            pSrcCur + ((_covered_width >> nLogxRatioUV_super) << pixelsize_super_shift), _src_pitch_arr[P],
            (nWidth - _covered_width) >> nLogxRatioUV_super/* real row_size */, rowsize /* bad name. it's height = nBlkSizeY >> nLogyRatioUV_super*/
          );
        }
        else {
          BitBlt(
            pDstCur + ((_covered_width >> nLogxRatioUV_super) << pixelsize_super_shift), _dst_pitch_arr[P],
            pSrcCur + ((_covered_width >> nLogxRatioUV_super) << pixelsize_super_shift), _src_pitch_arr[P],
            ((nWidth - _covered_width) >> nLogxRatioUV_super) << pixelsize_super_shift /* real row_size */, rowsize /* bad name. it's height = nBlkSizeY >> nLogyRatioUV_super*/
          );
        }
      }
    } // for bx

    pDstCur += effective_nDstPitch;
    pSrcCur += effective_nSrcPitch;

    if (by == nBlkY - 1 && _covered_height < nHeight) // bottom uncovered region
    {
      // chroma
      if (_out16_flag) {
        // copy 8 bit source to 16bit target
        plane_copy_8_to_16_c(
          pDstCur, _dst_pitch_arr[P],
          pSrcCur, _src_pitch_arr[P],
          nWidth >> nLogxRatioUV_super, (nHeight - _covered_height) >> nLogyRatioUV_super /* height */
        );
      }
      else {
        BitBlt(
          pDstCur, _dst_pitch_arr[P],
          pSrcCur, _src_pitch_arr[P],
          (nWidth >> nLogxRatioUV_super) << pixelsize_super_shift, (nHeight - _covered_height) >> nLogyRatioUV_super /* height */
        );
      }
    }
  } // for by

}



template <int P>
void	MDegrainN::process_chroma_overlap_slice(Slicer::TaskData &td)
{
  assert(&td != 0);

  if (nOverlapY == 0
    || (td._y_beg == 0 && td._y_end == nBlkY))
  {
    process_chroma_overlap_slice <P>(td._y_beg, td._y_end);
  }

  else
  {
    assert(td._y_end - td._y_beg >= 2);

    process_chroma_overlap_slice <P>(td._y_beg, td._y_end - 1);

    const conc::AioAdd <int> inc_ftor(+1);

    const int cnt_top = conc::AtomicIntOp::exec_new(
      _boundary_cnt_arr[td._y_beg],
      inc_ftor
    );
    if (td._y_beg > 0 && cnt_top == 2)
    {
      process_chroma_overlap_slice <P>(td._y_beg - 1, td._y_beg);
    }

    int				cnt_bot = 2;
    if (td._y_end < nBlkY)
    {
      cnt_bot = conc::AtomicIntOp::exec_new(
        _boundary_cnt_arr[td._y_end],
        inc_ftor
      );
    }
    if (cnt_bot == 2)
    {
      process_chroma_overlap_slice <P>(td._y_end - 1, td._y_end);
    }
  }
}



template <int P>
void	MDegrainN::process_chroma_overlap_slice(int y_beg, int y_end)
{
  TmpBlock       tmp_block;

  const int rowsize = (nBlkSizeY - nOverlapY) >> nLogyRatioUV_super; // bad name. it's height really
  const BYTE *pSrcCur = _src_ptr_arr[P] + y_beg * rowsize * _src_pitch_arr[P];

  uint16_t *pDstShort = (_dst_short.empty()) ? 0 : &_dst_short[0] + y_beg * rowsize * _dst_short_pitch;
  int *pDstInt = (_dst_int.empty()) ? 0 : &_dst_int[0] + y_beg * rowsize * _dst_int_pitch;
  const int tmpPitch = nBlkSizeX;
  assert(tmpPitch <= TmpBlock::MAX_SIZE);

  int effective_nSrcPitch = ((nBlkSizeY - nOverlapY) >> nLogyRatioUV_super) * _src_pitch_arr[P]; // pitch is byte granularity
  int effective_dstShortPitch = ((nBlkSizeY - nOverlapY) >> nLogyRatioUV_super) * _dst_short_pitch; // pitch is short granularity
  int effective_dstIntPitch = ((nBlkSizeY - nOverlapY) >> nLogyRatioUV_super) * _dst_int_pitch; // pitch is int granularity

  for (int by = y_beg; by < y_end; ++by)
  {
    // indexing overlap windows weighting table: top=0 middle=3 bottom=6
    /*
    0 = Top Left    1 = Top Middle    2 = Top Right
    3 = Middle Left 4 = Middle Middle 5 = Middle Right
    6 = Bottom Left 7 = Bottom Middle 8 = Bottom Right
    */

    int wby = (by == 0) ? 0 * 3 : (by == nBlkY - 1) ? 2 * 3 : 1 * 3; // 0 for very first, 2*3 for very last, 1*3 for all others in the middle
    int xx = 0; // logical offset. Mul by 2 for pixelsize_super==2. Don't mul for indexing int* array

    int iIdxBlk_row_start = by * nBlkX;
    for (int k = 0; k < _trad * 2; ++k)
    {
      // prefetch all vectors for all ref planes all blocks of row in linear reading
      const VECTOR* pMVsArrayPref = pMVsPlanesArrays[k];
      HWprefetch_T0((char*)pMVsArrayPref, nBlkX * sizeof(VECTOR));

      // prefetch all rows of all ref blocks in linear lines of rows reading, use first MV in a row now as offset
      const int blx = 0 + pMVsArrayPref[iIdxBlk_row_start].x; // or zero ??
      const int bly = by * (nBlkSizeY - nOverlapY) * nPel + pMVsArrayPref[iIdxBlk_row_start].y;
      const BYTE* p = _planes_ptr[k][0]->GetPointer(blx, bly);
      int np = _planes_ptr[k][0]->GetPitch();

      for (int iH = 0; iH < nBlkSizeY; ++iH)
      {
        HWprefetch_T1((char*)p + np * iH, nBlkX * nBlkSizeX);
      }
    }

    // prefetch source full row in linear lines reading
    for (int iH = 0; iH < nBlkSizeY; ++iH)
    {
      HWprefetch_T1((char*)pSrcCur + _src_pitch_arr[0] * iH, nBlkX * nBlkSizeX);
    }

    for (int bx = 0; bx < nBlkX; ++bx)
    {
      // select window
      // indexing overlap windows weighting table: left=+0 middle=+1 rightmost=+2
      int wbx = (bx == 0) ? 0 : (bx == nBlkX - 1) ? 2 : 1; // 0 for very first, 2 for very last, 1 for all others in the middle
      short *winOverUV = _overwins_uv->GetWindow(wby + wbx);

      int i = by * nBlkX + bx;
      const BYTE *ref_data_ptr_arr[MAX_TEMP_RAD * 2];
      int pitch_arr[MAX_TEMP_RAD * 2];
      int weight_arr[1 + MAX_TEMP_RAD * 2]; // 0th is special

      for (int k = 0; k < _trad * 2; ++k)
      {
/*        (this->*use_block_uv_func)(
          ref_data_ptr_arr[k],
          pitch_arr[k],
          weight_arr[k + 1], // from 1st
          _usable_flag_arr[k],
          _mv_clip_arr[k],
          i,
          _planes_ptr[k][P],
          pSrcCur,
          xx << pixelsize_super_shift, //  the pointer increment inside knows that xx later here is incremented with nBlkSize and not nBlkSize>>_xRatioUV
          _src_pitch_arr[P],
          bx,
          by,
          pMVsPlanesArrays[k]
        );*/
        if (!bMVsAddProc)
        {
          (this->*use_block_uv_func)(
            ref_data_ptr_arr[k],
            pitch_arr[k],
            weight_arr[k + 1],
            _usable_flag_arr[k],
            _mv_clip_arr[k],
            i,
            _planes_ptr[k][P],
            pSrcCur,
            xx << pixelsize_super_shift, // the pointer increment inside knows that xx later here is incremented with nBlkSize and not nBlkSize>>_xRatioUV
                // todo: copy from MDegrainX. Here we shift, and incement with nBlkSize>>_xRatioUV
            _src_pitch_arr[P],
            bx,
            by,
            pMVsPlanesArrays[k]
            ); // vs: extra nLogPel, plane, xSubUV, ySubUV, thSAD
        }
        else
        {
          (this->*use_block_uv_func)(
            ref_data_ptr_arr[k],
            pitch_arr[k],
            weight_arr[k + 1],
            _usable_flag_arr[k],
            _mv_clip_arr[k],
            i,
            _planes_ptr[k][P],
            pSrcCur,
            xx << pixelsize_super_shift, // the pointer increment inside knows that xx later here is incremented with nBlkSize and not nBlkSize>>_xRatioUV
                // todo: copy from MDegrainX. Here we shift, and incement with nBlkSize>>_xRatioUV
            _src_pitch_arr[P],
            bx,
            by,
            (const VECTOR*)pFilteredMVsPlanesArrays[k]
            ); // vs: extra nLogPel, plane, xSubUV, ySubUV, thSAD
        }
      }

      norm_weights(weight_arr, _trad); // 0th + 1..MAX_TEMP_RAD*2

      // chroma
      // here we don't pass pixelsize, because _degrainchroma_ptr points already to the uint16_t version
      // if the clip was 16 bit one
      _degrainchroma_ptr(
        &tmp_block._d[0], tmp_block._lsb_ptr, tmpPitch << pixelsize_output_shift,
        pSrcCur + (xx << pixelsize_super_shift), _src_pitch_arr[P],
        ref_data_ptr_arr, pitch_arr, weight_arr, _trad
      );
      if (_lsb_flag)
      {
        _overschroma_lsb_ptr(
          pDstInt + xx, _dst_int_pitch,
          &tmp_block._d[0], tmp_block._lsb_ptr, tmpPitch,
          winOverUV, nBlkSizeX >> nLogxRatioUV_super
        );
      }
      else if (_out16_flag)
      {
        // cast to match the prototype
        _overschroma16_ptr(
          (uint16_t*)(pDstInt + xx), _dst_int_pitch,
          &tmp_block._d[0], tmpPitch << pixelsize_output_shift,
          winOverUV, nBlkSizeX >> nLogxRatioUV_super);
      }
      else if (pixelsize_super == 1)
      {
        _overschroma_ptr(
          pDstShort + xx, _dst_short_pitch,
          &tmp_block._d[0], tmpPitch,
          winOverUV, nBlkSizeX >> nLogxRatioUV_super);
      } else if (pixelsize_super == 2)
      {
        _overschroma16_ptr(
          (uint16_t*)(pDstInt + xx), _dst_int_pitch, 
          &tmp_block._d[0], tmpPitch << pixelsize_super_shift, 
          winOverUV, nBlkSizeX >> nLogxRatioUV_super);
      }
      else // if (pixelsize_super == 4)
      {
        _overschroma32_ptr(
          (uint16_t*)(pDstInt + xx), _dst_int_pitch,
          &tmp_block._d[0], tmpPitch << pixelsize_super_shift,
          winOverUV, nBlkSizeX >> nLogxRatioUV_super);
      }

      xx += ((nBlkSizeX - nOverlapX) >> nLogxRatioUV_super); // no pixelsize here

    } // for bx

    pSrcCur += effective_nSrcPitch; // pitch is byte granularity
    pDstShort += effective_dstShortPitch; // pitch is short granularity
    pDstInt += effective_dstIntPitch; // pitch is int granularity
  } // for by

}

#define ClipBlxBly \
if (blx < iMinBlx) blx = iMinBlx; \
if (bly < iMinBly) bly = iMinBly; \
if (blx > iMaxBlx) blx = iMaxBlx; \
if (bly > iMaxBly) bly = iMaxBly; 


MV_FORCEINLINE void	MDegrainN::use_block_y(
  const BYTE * &p, int &np, int &wref, bool usable_flag, const MvClipInfo &c_info,
  int i, const MVPlane *plane_ptr, const BYTE *src_ptr, int xx, int src_pitch, int ibx, int iby, const VECTOR* pMVsArray
)
{
  if (usable_flag)
  {
     int blx = ibx * (nBlkSizeX - nOverlapX) * nPel + pMVsArray[i].x;
     int bly = iby * (nBlkSizeY - nOverlapY) * nPel + pMVsArray[i].y;

  // temp check - DX12_ME return invalid vectors sometime
     ClipBlxBly
/*     if (blx < -nBlkSizeX * nPel) blx = -nBlkSizeX * nPel;
     if (bly < -nBlkSizeY * nPel) bly = -nBlkSizeY * nPel;
     if (blx > nBlkSizeX* nBlkX* nPel) blx = nBlkSizeX * nBlkX * nPel;
     if (bly > nBlkSizeY* nBlkY* nPel) bly = nBlkSizeY * nBlkY * nPel;*/
    
     if (nPel != 1 && nUseSubShift != 0)
     {
       p = plane_ptr->GetPointerSubShift(blx, bly, nBlkSizeX, nBlkSizeY, np);
     }
     else
     {
       p = plane_ptr->GetPointer(blx, bly);
       np = plane_ptr->GetPitch();
     }
    sad_t block_sad = pMVsArray[i].sad;

    wref = DegrainWeightN(c_info._thsad, c_info._thsad_sq, block_sad, _wpow);
  }
  else
  {
    p = src_ptr + xx;
    np = src_pitch;
    wref = 0;
  }
}


MV_FORCEINLINE void	MDegrainN::use_block_y_thSADzeromv_thSADcohmv(
  const BYTE*& p, int& np, int& wref, bool usable_flag, const MvClipInfo& c_info,
  int i, const MVPlane* plane_ptr, const BYTE* src_ptr, int xx, int src_pitch, int ibx, int iby, const VECTOR* pMVsArray
)
{
  if (usable_flag)
  {
    int blx = ibx * (nBlkSizeX - nOverlapX) * nPel + pMVsArray[i].x;
    int bly = iby * (nBlkSizeY - nOverlapY) * nPel + pMVsArray[i].y;

    // temp check - DX12_ME return invalid vectors sometime
    ClipBlxBly
/*    if (blx < -nBlkSizeX * nPel) blx = -nBlkSizeX * nPel;
    if (bly < -nBlkSizeY * nPel) bly = -nBlkSizeY * nPel;
    if (blx > nBlkSizeX* nBlkX* nPel) blx = nBlkSizeX * nBlkX * nPel;
    if (bly > nBlkSizeY* nBlkY* nPel) bly = nBlkSizeY * nBlkY * nPel;*/

//    p = plane_ptr->GetPointer(blx, bly);
//    np = plane_ptr->GetPitch();
    if (nPel != 1 && nUseSubShift != 0)
    {
      p = plane_ptr->GetPointerSubShift(blx, bly, nBlkSizeX, nBlkSizeY, np);
    }
    else
    {
      p = plane_ptr->GetPointer(blx, bly);
      np = plane_ptr->GetPitch();
    }


    sad_t block_sad = pMVsArray[i].sad;

    // pull SAD at static areas 
    if ((pMVsArray[i].x == 0) && (pMVsArray[i].y == 0))
    {
      block_sad = (sad_t)((float)block_sad * fadjSADzeromv);
    }
    else
    {
      if (ithCohMV >= 0) // skip long calc if ithCohV<0
      {
        // pull SAD at common motion blocks 
        int x_cur = pMVsArray[i].x, y_cur = pMVsArray[i].y;
        // upper block
        VECTOR v_upper, v_left, v_right, v_lower;
        int i_upper = i - nBlkX;
        if (i_upper < 0) i_upper = 0;
        v_upper = pMVsArray[i_upper];

        int i_left = i - 1;
        if (i_left < 0) i_left = 0;
        v_left = pMVsArray[i_left];

        int i_right = i + 1;
        if (i_right > nBlkX* nBlkY) i_right = nBlkX * nBlkY;
        v_right = pMVsArray[i_right];

        int i_lower = i + nBlkX;
        if (i_lower > nBlkX* nBlkY) i_lower = i;
        v_lower = pMVsArray[i_lower];

        int iabs_dc_x = SADABS(v_upper.x - x_cur) + SADABS(v_left.x - x_cur) + SADABS(v_right.x - x_cur) + SADABS(v_lower.x - x_cur);
        int iabs_dc_y = SADABS(v_upper.y - y_cur) + SADABS(v_left.y - y_cur) + SADABS(v_right.y - y_cur) + SADABS(v_lower.y - y_cur);

        if ((iabs_dc_x + iabs_dc_y) <= ithCohMV)
        {
          block_sad = (sad_t)((float)block_sad * fadjSADcohmv);
        }
      }
    }

    wref = DegrainWeightN(c_info._thsad, c_info._thsad_sq, block_sad, _wpow);
  }
  else
  {
    p = src_ptr + xx;
    np = src_pitch;
    wref = 0;
  }
}

MV_FORCEINLINE void	MDegrainN::use_block_uv(
  const BYTE * &p, int &np, int &wref, bool usable_flag, const MvClipInfo &c_info,
  int i, const MVPlane *plane_ptr, const BYTE *src_ptr, int xx, int src_pitch, int ibx, int iby, const VECTOR* pMVsArray
)
{
  if (usable_flag)
  {
     int blx = ibx * (nBlkSizeX - nOverlapX) * nPel + pMVsArray[i].x;
     int bly = iby * (nBlkSizeY - nOverlapY) * nPel + pMVsArray[i].y;

     // temp check - DX12_ME return invalid vectors sometime
     ClipBlxBly
/*     if (blx < -nBlkSizeX * nPel) blx = -nBlkSizeX * nPel;
     if (bly < -nBlkSizeY * nPel) bly = -nBlkSizeY * nPel;
     if (blx > nBlkSizeX* nBlkX* nPel) blx = nBlkSizeX * nBlkX * nPel;
     if (bly > nBlkSizeY* nBlkY* nPel) bly = nBlkSizeY * nBlkY * nPel;*/
     
//    p = plane_ptr->GetPointer(blx >> nLogxRatioUV_super, bly >> nLogyRatioUV_super);
//    np = plane_ptr->GetPitch();
     if (nPel != 1 && nUseSubShift != 0)
     {
       p = plane_ptr->GetPointerSubShift(blx >> nLogxRatioUV_super, bly >> nLogyRatioUV_super, nBlkSizeX >> nLogxRatioUV_super, nBlkSizeY >> nLogyRatioUV_super, np);
     }
     else
     {
       p = plane_ptr->GetPointer(blx >> nLogxRatioUV_super, bly >> nLogyRatioUV_super);
       np = plane_ptr->GetPitch();
     }

    sad_t block_sad = pMVsArray[i].sad;

    wref = DegrainWeightN(c_info._thsad, c_info._thsad_sq, block_sad, _wpow);
  }
  else
  {
    // just to have a valid data pointer, will not count, weight is zero
    p = src_ptr + xx; // done: kill  >> nLogxRatioUV_super from here and put it in the caller like in MDegrainX
    np = src_pitch;
    wref = 0;
  }
}

MV_FORCEINLINE void	MDegrainN::use_block_uv_thSADzeromv_thSADcohmv(
  const BYTE*& p, int& np, int& wref, bool usable_flag, const MvClipInfo& c_info,
  int i, const MVPlane* plane_ptr, const BYTE* src_ptr, int xx, int src_pitch, int ibx, int iby, const VECTOR* pMVsArray
)
{
  if (usable_flag)
  {
    int blx = ibx * (nBlkSizeX - nOverlapX) * nPel + pMVsArray[i].x;
    int bly = iby * (nBlkSizeY - nOverlapY) * nPel + pMVsArray[i].y;

    // temp check - DX12_ME return invalid vectors sometime
    ClipBlxBly
/*    if (blx < -nBlkSizeX * nPel) blx = -nBlkSizeX * nPel;
    if (bly < -nBlkSizeY * nPel) bly = -nBlkSizeY * nPel;
    if (blx > nBlkSizeX* nBlkX* nPel) blx = nBlkSizeX * nBlkX * nPel;
    if (bly > nBlkSizeY* nBlkY* nPel) bly = nBlkSizeY * nBlkY * nPel;*/


//    p = plane_ptr->GetPointer(blx >> nLogxRatioUV_super, bly >> nLogyRatioUV_super);
//    np = plane_ptr->GetPitch();
    if (nPel != 1 && nUseSubShift != 0)
    {
      p = plane_ptr->GetPointerSubShift(blx >> nLogxRatioUV_super, bly >> nLogyRatioUV_super, nBlkSizeX >> nLogxRatioUV_super, nBlkSizeY >> nLogyRatioUV_super, np);
    }
    else
    {
      p = plane_ptr->GetPointer(blx >> nLogxRatioUV_super, bly >> nLogyRatioUV_super);
      np = plane_ptr->GetPitch();
    }

    sad_t block_sad = pMVsArray[i].sad;

    // pull SAD at static areas 
    if ((pMVsArray[i].x == 0) && (pMVsArray[i].y == 0))
    {
      block_sad = (sad_t)((float)block_sad * fadjSADzeromv);
    }
    else
    {
      if (ithCohMV >= 0) // skip long calc if ithCohV<0
      {
        // pull SAD at common motion blocks 
        int x_cur = pMVsArray[i].x, y_cur = pMVsArray[i].y;
        // upper block
        VECTOR v_upper, v_left, v_right, v_lower;
        int i_upper = i - nBlkX;
        if (i_upper < 0) i_upper = 0;
        v_upper = pMVsArray[i_upper];

        int i_left = i - 1;
        if (i_left < 0) i_left = 0;
        v_left = pMVsArray[i_left];

        int i_right = i + 1;
        if (i_right > nBlkX* nBlkY) i_right = nBlkX * nBlkY;
        v_right = pMVsArray[i_right];

        int i_lower = i + nBlkX;
        if (i_lower > nBlkX* nBlkY) i_lower = i;
        v_lower = pMVsArray[i_lower];

        int iabs_dc_x = SADABS(v_upper.x - x_cur) + SADABS(v_left.x - x_cur) + SADABS(v_right.x - x_cur) + SADABS(v_lower.x - x_cur);
        int iabs_dc_y = SADABS(v_upper.y - y_cur) + SADABS(v_left.y - y_cur) + SADABS(v_right.y - y_cur) + SADABS(v_lower.y - y_cur);

        if ((iabs_dc_x + iabs_dc_y) <= ithCohMV)
        {
          block_sad = (sad_t)((float)block_sad * fadjSADcohmv);
        }
      }
    }

    wref = DegrainWeightN(c_info._thsad, c_info._thsad_sq, block_sad, _wpow);
  }
  else
  {
    // just to have a valid data pointer, will not count, weight is zero
    p = src_ptr + xx; // done: kill  >> nLogxRatioUV_super from here and put it in the caller like in MDegrainX
    np = src_pitch;
    wref = 0;
  }
}



void MDegrainN::norm_weights(int wref_arr[], int trad)
{
  const int nbr_frames = trad * 2 + 1;

  const int one = 1 << DEGRAIN_WEIGHT_BITS; // 8 bit, 256

  wref_arr[0] = one;
  int wsum = 1;
  for (int k = 0; k < nbr_frames; ++k)
  {
    wsum += wref_arr[k];
  }

  // normalize weights to 256
  int wsrc = one;
  for (int k = 1; k < nbr_frames; ++k)
  {
    const int norm = wref_arr[k] * one / wsum;
    wref_arr[k] = norm;
    wsrc -= norm;
  }
  wref_arr[0] = wsrc;
}

MV_FORCEINLINE int DegrainWeightN(int thSAD, double thSAD_pow, int blockSAD, int wpow)
{
  // Returning directly prevents a divide by 0 if thSAD == blockSAD == 0.
  // keep integer comparison for speed
  if (thSAD <= blockSAD)
    return 0;

  if (wpow > 6) return (int)(1 << DEGRAIN_WEIGHT_BITS); // if 7  - equal weights version - fast return, max speed

//  double blockSAD_pow = blockSAD;
  float blockSAD_pow = blockSAD;

  for (int i = 0; i < wpow - 1; i++)
  {
    blockSAD_pow *= blockSAD;
  }
  /*
  if (CPU_SSE2) // test if single precicion and approximate reciprocal is enough
  {
    float fthSAD_pow = (float)thSAD_pow;
    __m128 xmm_divisor = _mm_cvt_si2ss(xmm_divisor, (fthSAD_pow + blockSAD_pow));
    __m128 xmm_res = _mm_cvt_si2ss(xmm_res, (fthSAD_pow - blockSAD_pow));
    xmm_divisor = _mm_rcp_ss(xmm_divisor);

    __m128 xmm_dwb = _mm_cvt_si2ss(xmm_dwb, (1 << DEGRAIN_WEIGHT_BITS));

    xmm_res = _mm_mul_ss(xmm_res, xmm_divisor);
    xmm_res = _mm_mul_ss(xmm_res, xmm_dwb);

    return _mm_cvt_ss2si(xmm_res);
  }
  */
  // float is approximately only 24 bit precise, use double
  return (int)((double)(1 << DEGRAIN_WEIGHT_BITS) * (thSAD_pow - blockSAD_pow) / (thSAD_pow + blockSAD_pow));

}

void MDegrainN::CreateBlocks2DWeightsArr(int bx, int by) // still no internal MT supported - global class pBlocks2DWeightsArr array
{
  int iX0 = (iSEWBWidth / 2) + 1;
  int iHalfSEWBWidth = iSEWBWidth / 2;

  uint16_t* pScalarWeights = pui16WeightsFrameArr + (bx + by * nBlkX) * (2 * _trad + 1); // bx, by - current block coords
  // square hard weights at start
  // todo: make fill with inner weight only inner part of weight area of size (nBlkSizeX-2*iSEWBWidth)x(nBlkSizeY-2*2*iSEWBWidth)
  for (int k = 0; k < 2 * _trad + 1; ++k) // weight block counter, zero is weight of current frame' block 
  {
    uint16_t* pDst2DWeights = pui16Blocks2DWeightsArr + nBlkSizeX * nBlkSizeY * k;
    for (int h = 0; h < nBlkSizeY; h++)
    {
      for (int x = 0; x < nBlkSizeX; x++)
      {
        pDst2DWeights[x] = pScalarWeights[k];
      }
      pDst2DWeights+=nBlkSizeX;
    }
  }
  
  // linear 2-weight interpolation as first approach
  for (int k = 0; k < 2 * _trad + 1; ++k) // weight block counter, zero is weight of current frame' block 
  {
    int iCurrBlockCenterWeight = pScalarWeights[k];

    // upper block' weight border
    if (by != 0)
    {
      uint16_t* pScalarWeightsUp = pui16WeightsFrameArr + (bx + (by - 1) * nBlkX) * (2 * _trad + 1); // bx, by - current block coords
      int iWeightUp = pScalarWeightsUp[k];

      // temp float arg
      float fMulCoeff = (float)(iCurrBlockCenterWeight - iWeightUp) / (float)(iSEWBWidth + 1);

      uint16_t* pDst2DWeights = pui16Blocks2DWeightsArr + nBlkSizeX * nBlkSizeY * k;
      for (int h = 0; h < iHalfSEWBWidth; h++)
      {
        // here we have equal soft-weight per all samples of row, so calculate only single value per row and broadcast-save
        // first float linear interpolation to test
        float fRowSW = iWeightUp + fMulCoeff * (h + iX0);

        for (int x = 0; x < nBlkSizeX; x++)
        {
          // place broadcast here
          pDst2DWeights[x] = (uint16_t)(fRowSW + 0.5f);
        }
        pDst2DWeights += nBlkSizeX;
      }
    }

    // lower block' weight border
    if (by != nBlkY)
    {
      uint16_t* pScalarWeightsLow = pui16WeightsFrameArr + (bx + (by + 1) * nBlkX) * (2 * _trad + 1); // bx, by - current block coords
      int iWeightLow = pScalarWeightsLow[k];

      // temp float arg
      float fMulCoeff = (float)(iCurrBlockCenterWeight - iWeightLow) / (float)(iSEWBWidth + 1);

      uint16_t* pDst2DWeights = pui16Blocks2DWeightsArr + nBlkSizeX * nBlkSizeY * k + nBlkSizeX * (nBlkSizeY - 1); // point to start of last line
      for (int h = 0; h < iHalfSEWBWidth; h++) // reverse step from block's bottom
      {
        float fRowSW = iWeightLow + fMulCoeff * (h + iX0);

        for (int x = 0; x < nBlkSizeX; x++)
        {
          // place broadcast here
          pDst2DWeights[x] = (uint16_t)(fRowSW + 0.5f);
        }
        pDst2DWeights -= nBlkSizeX; // reverse step from block's bottom
      }
    }

    // left block' weight border
    if (bx != 0)
    {
      uint16_t* pScalarWeightsLeft = pui16WeightsFrameArr + ((bx - 1) + by * nBlkX) * (2 * _trad + 1); // bx, by - current block coords
      int iWeightLeft = pScalarWeightsLeft[k];

      // temp float arg
      float fMulCoeff = (float)(iCurrBlockCenterWeight - iWeightLeft) / (float)(iSEWBWidth + 1);

      for (int x = 0; x < iHalfSEWBWidth; x++) // top left and lower left corners overlap proc in next version
      {
        float fColumnSW = iWeightLeft + fMulCoeff * (x + iX0);
        uint16_t* pDst2DWeights = pui16Blocks2DWeightsArr + nBlkSizeX * nBlkSizeY * k + x;

        for (int h = 0; h < nBlkSizeY; h++)
        {
          // put vertical broadcast
          *pDst2DWeights = (uint16_t)(fColumnSW + 0.5f);
          pDst2DWeights += nBlkSizeX;
        }
      }
    }
    // skip proc

    // right block' weight border
    if (bx != nBlkX)
    {
      uint16_t*  pScalarWeightsRight = pui16WeightsFrameArr + ((bx + 1) + by * nBlkX) * (2 * _trad + 1); // bx, by - current block coords
      int iWeightRight = pScalarWeightsRight[k];

      // temp float arg
      float fMulCoeff = (float)(iCurrBlockCenterWeight - iWeightRight) / (float)(iSEWBWidth + 1);

      for (int x = 0; x < iHalfSEWBWidth; x++) // top left and lower left corners overlap proc in next version
      {
        float fColumnSW = iWeightRight + fMulCoeff * (x + iX0);
        uint16_t* pDst2DWeights = pui16Blocks2DWeightsArr + nBlkSizeX * nBlkSizeY * k + (nBlkSizeX - 1 - x); // reverse columns scan

        for (int h = 0; h < nBlkSizeY; h++)
        {
          // put vertical broadcast
          *pDst2DWeights = (uint16_t)(fColumnSW + 0.5f);
          pDst2DWeights += nBlkSizeX;
        }
      }
    }

  }

  // perform new re-weighting of processed borders of the width=iSEWBWidth/2
  /*

        pDst[0] = one;
      int wsum = 1;
      for (int k = 0; k < nbr_frames; ++k)
      {
        wsum += pDst[k];
      }

      // normalize weights to 256
      int wsrc = one;
      for (int k = 1; k < nbr_frames; ++k)
      {
        const int norm = pDst[k] * one / wsum;
        pDst[k] = norm;
        wsrc -= norm;
      }
      pDst[0] = wsrc;
  */

  const int one = 1 << DEGRAIN_WEIGHT_BITS; // 8 bit, 256
  const int nbr_frames = _trad * 2 + 1;
  const int iSizeOfWeightMask = nBlkSizeX * nBlkSizeY;

  const int iLowNonProc = nBlkSizeY - iSEWBWidth / 2;
  const int iRightNonProc = nBlkSizeX - iSEWBWidth / 2;
  
  // todo: make re-weight only outer part of block
  for (int h = 0; h < nBlkSizeY; h++)
  {
    for (int x = 0; x < nBlkSizeX; x++)
    {
      // simple attempt of skip inner part of block
//      if ((h > iHalfSEWBWidth) && (h < (iLowNonProc))) continue; //- some bug here ??
//      if ((x > iHalfSEWBWidth) && (x < (iRightNonProc))) continue;

      uint16_t* pDst2DWeights = pui16Blocks2DWeightsArr + (nBlkSizeX * h) + x;
      uint16_t* pDst2DWeights_start = pDst2DWeights;
      int wsum = 1;
      for (int k = 0; k < nbr_frames; ++k) // weight block counter, zero is weight of current frame' block 
      {
        wsum += *pDst2DWeights;
        pDst2DWeights += iSizeOfWeightMask;
      }

      // normalize weights to 256
      int wsrc = one;
      pDst2DWeights = pDst2DWeights_start + iSizeOfWeightMask;// first ref block
      for (int k = 1; k < nbr_frames; ++k)
      {
        const int norm = (*pDst2DWeights) * one / wsum;
        *pDst2DWeights = norm;
        wsrc -= norm;
        pDst2DWeights += iSizeOfWeightMask;
      }
      *pDst2DWeights_start = wsrc;
    }
  }
  

}

void MDegrainN::CreateFrameWeightsArr_C(void)
{
  const int one = 1 << DEGRAIN_WEIGHT_BITS; // 8 bit, 256
  //  for (int by = td._y_beg; by < td._y_end; ++by)
  for (int by = 0; by < nBlkY; ++by) // single threaded proc
  {
    int xx = 0; // logical offset. Mul by 2 for pixelsize_super==2. Don't mul for indexing int* array

    for (int bx = 0; bx < nBlkX; ++bx) // todo: make this SIMD
    {
      int i = by * nBlkX + bx;

      const int nbr_frames = _trad * 2 + 1;
      uint16_t* pDst = pui16WeightsFrameArr + (bx + by * nBlkX) * nbr_frames; // norm directly to global array

      PrefetchMVs(i);

      for (int k = 0; k < _trad * 2; ++k)
      {

        if (_usable_flag_arr[k])
        {
          VECTOR* pMVsArray;
          if (!bMVsAddProc)
          {
            pMVsArray = (VECTOR*)pMVsPlanesArrays[k];
          }
          else
          {
            pMVsArray = pFilteredMVsPlanesArrays[k];
          }
          const sad_t block_sad = pMVsArray[i].sad;

          pDst[k + 1] = (uint16_t)DegrainWeightN(_mv_clip_arr[k]._thsad, _mv_clip_arr[k]._thsad_sq, block_sad, _wpow);
        }
        else
        {
          pDst[k + 1] = 0;
        }
      }
      
      pDst[0] = one;
      int wsum = 1;
      for (int k = 0; k < nbr_frames; ++k)
      {
        wsum += pDst[k];
      }

      // normalize weights to 256
      int wsrc = one;
      for (int k = 1; k < nbr_frames; ++k)
      {
        const int norm = pDst[k] * one / wsum;
        pDst[k] = norm;
        wsrc -= norm;
      }
      pDst[0] = wsrc; 

    }	// for bx
  }	// for by
}

float MDegrainN::fSinc(float x)
{
  x = fabsf(x);

  if (x > 0.000001f)
  {
    return sinf(x) / x;
  }
  else return 1.0f;
}

void MDegrainN::FilterMVs(void)
{
  const int  rowsize = nBlkSizeY - nOverlapY; // num of lines in row of blocks = block height - overlap ?
  const BYTE* pSrcCur = _src_ptr_arr[0];
  const BYTE* pSrcCurU = _src_ptr_arr[1];
  const BYTE* pSrcCurV = _src_ptr_arr[2];
  int effective_nSrcPitch = ((nBlkSizeY - nOverlapY) >> nLogyRatioUV_super)* _src_pitch_arr[1]; // pitch is byte granularity, from 1st chroma plane

  bool bChroma = (_nsupermodeyuv & UPLANE) && (_nsupermodeyuv & VPLANE); // chroma present in super clip ?
  // scaleCSAD in the MVclip props
  int chromaSADscale = _mv_clip_arr[0]._clip_sptr->chromaSADScale; // from 1st ?

  // todo: add chroma check in SAD if it present in super clip

  VECTOR filteredp2fvectors[(MAX_TEMP_RAD * 2) + 1];

  VECTOR p2fvectors[(MAX_TEMP_RAD * 2) + 1];

  for (int by = 0; by < nBlkY; by++)
  {
    int xx = 0; // logical offset. Mul by 2 for pixelsize_super==2. Don't mul for indexing int* array
    int xx_uv = 0; // logical offset. Mul by 2 for pixelsize_super==2. Don't mul for indexing int* array

    for (int bx = 0; bx < nBlkX; bx++)
    {
      int i = by * nBlkX + bx;

      // convert +1, -1, +2, -2, +3, -3 ... to
      // -3, -2, -1, 0, +1, +2, +3 timed sequence
      for (int k = 0; k < _trad; ++k)
      {
        p2fvectors[k] = pMVsPlanesArrays[(_trad - k - 1) * 2 + 1][i];
      }

      p2fvectors[_trad].x = 0; // zero trad - source block itself
      p2fvectors[_trad].y = 0;
      p2fvectors[_trad].sad = 0;

      for (int k = 1; k < _trad + 1; ++k)
      {
        p2fvectors[k + _trad] = pMVsPlanesArrays[(k - 1) * 2][i];
      }

      // perform lpf of all good vectors in tr-scope
      for (int pos = 0; pos < (_trad * 2 + 1); pos++)
      {
        float fSumX = 0.0f;
        float fSumY = 0.0f;
        for (int kpos = 0; kpos < MVLPFKERNELSIZE; kpos++)
        {
          int src_pos = pos + kpos - MVLPFKERNELSIZE / 2;
          if (src_pos < 0) src_pos = 0;
          if (src_pos > _trad * 2) src_pos = (_trad * 2); // total valid samples in vector of VECTORs is _trad*2+1
          fSumX += p2fvectors[src_pos].x * fMVLPFKernel[kpos];
          fSumY += p2fvectors[src_pos].y * fMVLPFKernel[kpos];
        }

        filteredp2fvectors[pos].x = (int)(fSumX);
        filteredp2fvectors[pos].y = (int)(fSumY);
        filteredp2fvectors[pos].sad = p2fvectors[pos].sad;
      }

      // final copy output
      VECTOR vLPFed, vOrig;

      for (int k = 0; k < _trad; ++k)
      {
        // recheck SAD:
        vLPFed = filteredp2fvectors[k];
        int idx_mvto = (_trad - k - 1) * 2 + 1;

        int blx = bx * (nBlkSizeX - nOverlapX) * nPel + vLPFed.x;
        int bly = by * (nBlkSizeY - nOverlapY) * nPel + vLPFed.y;

        // temp check - DX12_ME return invalid vectors sometime 
        if (blx < -nBlkSizeX * nPel) blx = -nBlkSizeX * nPel;
        if (bly < -nBlkSizeY * nPel) bly = -nBlkSizeY * nPel;
        if (blx > nBlkSizeX* nBlkX* nPel) blx = nBlkSizeX * nBlkX * nPel;
        if (bly > nBlkSizeY* nBlkY* nPel) bly = nBlkSizeY * nBlkY * nPel;

        if (_usable_flag_arr[idx_mvto])
        {
          const uint8_t* pRef = _planes_ptr[idx_mvto][0]->GetPointer(blx, bly);
          int npitchRef = _planes_ptr[idx_mvto][0]->GetPitch();

          sad_t sad_chroma = 0;

          if (bChroma)
          {
            const uint8_t* pRefU = _planes_ptr[idx_mvto][1]->GetPointer(blx >> nLogxRatioUV_super, bly >> nLogyRatioUV_super);
            int npitchRefU = _planes_ptr[idx_mvto][1]->GetPitch();
            const uint8_t* pRefV = _planes_ptr[idx_mvto][2]->GetPointer(blx >> nLogxRatioUV_super, bly >> nLogyRatioUV_super);
            int npitchRefV = _planes_ptr[idx_mvto][2]->GetPitch();

            sad_chroma = ScaleSadChroma(SADCHROMA(pSrcCurU + (xx_uv << pixelsize_super_shift), _src_pitch_arr[1], pRefU, npitchRefU)
              + SADCHROMA(pSrcCurV + (xx_uv << pixelsize_super_shift), _src_pitch_arr[2], pRefV, npitchRefV), chromaSADscale);

            sad_t luma_sad = SAD(pSrcCur + (xx << pixelsize_super_shift), _src_pitch_arr[0], pRef, npitchRef);

            vLPFed.sad = luma_sad + sad_chroma;

          }
          else
          {
            vLPFed.sad = SAD(pSrcCur + (xx << pixelsize_super_shift), _src_pitch_arr[0], pRef, npitchRef);
          }
        }
        vOrig = pMVsPlanesArrays[(_trad - k - 1) * 2 + 1][i];
        if ( (abs(vLPFed.x - vOrig.x) <= ithMVLPFCorr) && (abs(vLPFed.y - vOrig.y) <= ithMVLPFCorr) && (vLPFed.sad < _mv_clip_arr[idx_mvto]._thsad))
          pFilteredMVsPlanesArrays[(_trad - k - 1) * 2 + 1][i] = vLPFed;
        else // place original vector
          pFilteredMVsPlanesArrays[(_trad - k - 1) * 2 + 1][i] = vOrig;
      }

      for (int k = 1; k < _trad + 1; ++k)
      {
        // recheck SAD
        vLPFed = filteredp2fvectors[k + _trad];
        int idx_mvto = (k - 1) * 2;

        int blx = bx * (nBlkSizeX - nOverlapX) * nPel + vLPFed.x;
        int bly = by * (nBlkSizeY - nOverlapY) * nPel + vLPFed.y;

        // temp check - DX12_ME return invalid vectors sometime 
        if (blx < -nBlkSizeX * nPel) blx = -nBlkSizeX * nPel;
        if (bly < -nBlkSizeY * nPel) bly = -nBlkSizeY * nPel;
        if (blx > nBlkSizeX* nBlkX* nPel) blx = nBlkSizeX * nBlkX * nPel;
        if (bly > nBlkSizeY* nBlkY* nPel) bly = nBlkSizeY * nBlkY * nPel;

        if (_usable_flag_arr[idx_mvto])
        {
          const uint8_t* pRef = _planes_ptr[idx_mvto][0]->GetPointer(blx, bly);
          int npitchRef = _planes_ptr[idx_mvto][0]->GetPitch();

          sad_t sad_chroma = 0;

          //  looks still somwhere bug with chroma sad
          if (bChroma)
          {
            const uint8_t* pRefU = _planes_ptr[idx_mvto][1]->GetPointer(blx >> nLogxRatioUV_super, bly >> nLogyRatioUV_super);
            int npitchRefU = _planes_ptr[idx_mvto][1]->GetPitch();
            const uint8_t* pRefV = _planes_ptr[idx_mvto][2]->GetPointer(blx >> nLogxRatioUV_super, bly >> nLogyRatioUV_super);
            int npitchRefV = _planes_ptr[idx_mvto][2]->GetPitch();

            sad_chroma = ScaleSadChroma(SADCHROMA(pSrcCurU + (xx_uv << pixelsize_super_shift), _src_pitch_arr[1], pRefU, npitchRefU)
              + SADCHROMA(pSrcCurV + (xx_uv << pixelsize_super_shift), _src_pitch_arr[2], pRefV, npitchRefV), chromaSADscale);

            sad_t luma_sad = SAD(pSrcCur + (xx << pixelsize_super_shift), _src_pitch_arr[0], pRef, npitchRef);

            vLPFed.sad = luma_sad + sad_chroma;
          }
          else 
          {
            vLPFed.sad = SAD(pSrcCur + (xx << pixelsize_super_shift), _src_pitch_arr[0], pRef, npitchRef);
          }
        }
        vOrig = pMVsPlanesArrays[(k - 1) * 2][i];
        if ((abs(vLPFed.x - vOrig.x) <= ithMVLPFCorr) && (abs(vLPFed.y - vOrig.y) <= ithMVLPFCorr) && (vLPFed.sad < _mv_clip_arr[idx_mvto]._thsad))
          pFilteredMVsPlanesArrays[(k - 1) * 2][i] = vLPFed;
        else
          pFilteredMVsPlanesArrays[(k - 1) * 2][i] = vOrig;
      }

      xx += (nBlkSizeX - nOverlapX); // xx: indexing offset, - overlap ?
      xx_uv += ((nBlkSizeX - nOverlapX) >> nLogxRatioUV_super); // xx_uv: indexing offset

    } // bx

    pSrcCur += rowsize * _src_pitch_arr[0];
    
    pSrcCurU += effective_nSrcPitch;
    pSrcCurV += effective_nSrcPitch;

  } // by

}

MV_FORCEINLINE void MDegrainN::PrefetchMVs(int i)
{
  if ((i % 5) == 0) // do not prefetch each block - the 12bytes VECTOR sit about 5 times in the 64byte cache line 
  {
    if (!bMVsAddProc)
    {
      for (int k = 0; k < _trad * 2; ++k)
      {
        const VECTOR* pMVsArrayPref = pMVsPlanesArrays[k];
        _mm_prefetch(const_cast<const CHAR*>(reinterpret_cast<const CHAR*>(&pMVsArrayPref[i + 5])), _MM_HINT_T0);
      }
    }
    else
    {
      for (int k = 0; k < _trad * 2; ++k)
      {
        const VECTOR* pMVsArrayPref = pFilteredMVsPlanesArrays[k];
        _mm_prefetch(const_cast<const CHAR*>(reinterpret_cast<const CHAR*>(&pMVsArrayPref[i + 5])), _MM_HINT_T0);
      }
    }
  }
}

