#include "SADFunctions_avx.h"
//#include "overlap.h"
#include <map>
#include <tuple>

#include <emmintrin.h>
#include "def.h"

   // SAD routined are same for SADFunctions.h and SADFunctions_avx.cpp
   // todo put it into a common hpp

template<int nBlkHeight>
unsigned int Sad16_sse2_4xN_avx(const uint8_t *pSrc, int nSrcPitch, const uint8_t *pRef, int nRefPitch)
{
#ifdef __AVX__
  _mm256_zeroupper(); // diff from main sse2
#endif

  __m128i zero = _mm_setzero_si128();
  __m128i sum = _mm_setzero_si128();

  const int vert_inc = (nBlkHeight % 2) == 1 ? 1 : 2;

  for (int y = 0; y < nBlkHeight; y += vert_inc)
  {
    // 4 pixels: 8 bytes
    auto src1 = _mm_loadl_epi64((__m128i *) (pSrc));
    auto src2 = _mm_loadl_epi64((__m128i *) (pRef)); // lower 8 bytes
    if (vert_inc == 2) {
      auto src1_h = _mm_loadl_epi64((__m128i *) (pSrc + nSrcPitch));
      auto src2_h = _mm_loadl_epi64((__m128i *) (pRef + nRefPitch));
      src1 = _mm_castps_si128(_mm_movelh_ps(_mm_castsi128_ps(src1), _mm_castsi128_ps(src1_h))); // lower 64 bits from src2 low 64 bits, high 64 bits from src4b low 64 bits
      src2 = _mm_castps_si128(_mm_movelh_ps(_mm_castsi128_ps(src2), _mm_castsi128_ps(src2_h))); // lower 64 bits from src2 low 64 bits, high 64 bits from src4b low 64 bits
    }

    __m128i greater_t = _mm_subs_epu16(src1, src2); // unsigned sub with saturation
    __m128i smaller_t = _mm_subs_epu16(src2, src1);
    __m128i absdiff = _mm_or_si128(greater_t, smaller_t); //abs(s1-s2)  == (satsub(s1,s2) | satsub(s2,s1))
    sum = _mm_add_epi32(sum, _mm_unpacklo_epi16(absdiff, zero));
    if (vert_inc == 2) {
      sum = _mm_add_epi32(sum, _mm_unpackhi_epi16(absdiff, zero));
    }

    pSrc += nSrcPitch * vert_inc;
    pRef += nRefPitch * vert_inc;
  }
  // we have 4 integers for sum: a0 a1 a2 a3
#ifdef __AVX__
  __m128i sum1 = _mm_hadd_epi32(sum, sum); // a0+a1, a2+a3, (a0+a1, a2+a3)
  sum = _mm_hadd_epi32(sum1, sum1); // a0+a1+a2+a3, (a0+a1+a2+a3,a0+a1+a2+a3,a0+a1+a2+a3)
#else
  __m128i a0_a1 = _mm_unpacklo_epi32(sum, zero); // a0 0 a1 0
  __m128i a2_a3 = _mm_unpackhi_epi32(sum, zero); // a2 0 a3 0
  sum = _mm_add_epi32(a0_a1, a2_a3); // a0+a2, 0, a1+a3, 0
                                      // sum here: two 32 bit partial result: sum1 0 sum2 0
  __m128i sum_hi = _mm_castps_si128(_mm_movehl_ps(_mm_castsi128_ps(sum), _mm_castsi128_ps(sum)));
  // __m128i sum_hi  _mm_unpackhi_epi64(sum, zero); // a1 + a3. 2 dwords right 
  sum = _mm_add_epi32(sum, sum_hi);  // a0 + a2 + a1 + a3
#endif
  unsigned int result = _mm_cvtsi128_si32(sum);

#ifdef __AVX__
  _mm256_zeroupper(); // diff from main sse2, paranoia, bacause here we have no ymm regs
#endif

  return result;
}

template<int nBlkHeight>
unsigned int Sad16_sse2_6xN_avx(const uint8_t *pSrc, int nSrcPitch, const uint8_t *pRef, int nRefPitch)
{
#ifdef __AVX__
  _mm256_zeroupper(); // diff from main sse2
#endif

  __m128i zero = _mm_setzero_si128();
  __m128i sum = _mm_setzero_si128();
  __m128i mask6_16bit = _mm_set_epi16(0, 0, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF);

  for (int y = 0; y < nBlkHeight; y++)
  {
    // 6 pixels: 12 bytes 8+4, source is aligned
    auto src1 = _mm_load_si128((__m128i *) (pSrc));
    src1 = _mm_and_si128(src1, mask6_16bit);

    auto src2 =   _mm_loadl_epi64((__m128i *) (pRef)); // lower 8 bytes
    auto srcRest32 = _mm_load_ss(reinterpret_cast<const float *>(pRef + 8)); // upper 4 bytes
    src2 = _mm_castps_si128(_mm_movelh_ps(_mm_castsi128_ps(src2), srcRest32)); // lower 64 bits from src2 low 64 bits, high 64 bits from src4b low 64 bits

    __m128i greater_t = _mm_subs_epu16(src1, src2); // unsigned sub with saturation
    __m128i smaller_t = _mm_subs_epu16(src2, src1);
    __m128i absdiff = _mm_or_si128(greater_t, smaller_t); //abs(s1-s2)  == (satsub(s1,s2) | satsub(s2,s1))
    sum = _mm_add_epi32(sum, _mm_unpacklo_epi16(absdiff, zero));
    sum = _mm_add_epi32(sum, _mm_unpackhi_epi16(absdiff, zero));

    pSrc += nSrcPitch;
    pRef += nRefPitch;
  }
#ifdef __AVX__
  __m128i sum1 = _mm_hadd_epi32(sum, sum);
  sum = _mm_hadd_epi32(sum1, sum1);
#else
  // at 16 bits: we have 4 integers for sum: a0 a1 a2 a3
  __m128i a0_a1 = _mm_unpacklo_epi32(sum, zero); // a0 0 a1 0
  __m128i a2_a3 = _mm_unpackhi_epi32(sum, zero); // a2 0 a3 0
  sum = _mm_add_epi32(a0_a1, a2_a3); // a0+a2, 0, a1+a3, 0
                                     // sum here: two 32 bit partial result: sum1 0 sum2 0
  __m128i sum_hi = _mm_castps_si128(_mm_movehl_ps(_mm_castsi128_ps(sum), _mm_castsi128_ps(sum)));
  // __m128i sum_hi  _mm_unpackhi_epi64(sum, zero); // a1 + a3. 2 dwords right 
  sum = _mm_add_epi32(sum, sum_hi);  // a0 + a2 + a1 + a3
#endif
  unsigned int result = _mm_cvtsi128_si32(sum);

#ifdef __AVX__
  _mm256_zeroupper(); // diff from main sse2, paranoia, bacause here we have no ymm regs
#endif

  return result;
}

template<int nBlkHeight>
unsigned int Sad16_sse2_8xN_avx(const uint8_t *pSrc, int nSrcPitch, const uint8_t *pRef, int nRefPitch)
{
#ifdef __AVX__
  _mm256_zeroupper(); // diff from main sse2
#endif

  __m128i zero = _mm_setzero_si128();
  __m128i sum = _mm_setzero_si128();

  const int vert_inc = (nBlkHeight %2) == 1 ? 1 : 2;

  for (int y = 0; y < nBlkHeight; y += vert_inc)
  {
    // 1st row 8 pixels 16 bytes
    auto src1 = _mm_load_si128((__m128i *) (pSrc));
    auto src2 = _mm_loadu_si128((__m128i *) (pRef));
    __m128i greater_t = _mm_subs_epu16(src1, src2); // unsigned sub with saturation
    __m128i smaller_t = _mm_subs_epu16(src2, src1);
    __m128i absdiff = _mm_or_si128(greater_t, smaller_t); //abs(s1-s2)  == (satsub(s1,s2) | satsub(s2,s1))
    // 8 x uint16 absolute differences
    sum = _mm_add_epi32(sum, _mm_unpacklo_epi16(absdiff, zero));
    sum = _mm_add_epi32(sum, _mm_unpackhi_epi16(absdiff, zero));
    
    if (vert_inc == 2) {
    // 2nd row 8 pixels 16 bytes
      src1 = _mm_load_si128((__m128i *) (pSrc + nSrcPitch * 1));
      src2 = _mm_loadu_si128((__m128i *) (pRef + nRefPitch * 1));
      greater_t = _mm_subs_epu16(src1, src2); // unsigned sub with saturation
      smaller_t = _mm_subs_epu16(src2, src1);
      absdiff = _mm_or_si128(greater_t, smaller_t); //abs(s1-s2)  == (satsub(s1,s2) | satsub(s2,s1))
      sum = _mm_add_epi32(sum, _mm_unpacklo_epi16(absdiff, zero));
      sum = _mm_add_epi32(sum, _mm_unpackhi_epi16(absdiff, zero));
    }
    pSrc += nSrcPitch * vert_inc;
    pRef += nRefPitch * vert_inc;
  }
#ifdef __AVX__
  __m128i sum1 = _mm_hadd_epi32(sum, sum);
  sum = _mm_hadd_epi32(sum1, sum1);
#else
  // at 16 bits: we have 4 integers for sum: a0 a1 a2 a3
  __m128i a0_a1 = _mm_unpacklo_epi32(sum, zero); // a0 0 a1 0
  __m128i a2_a3 = _mm_unpackhi_epi32(sum, zero); // a2 0 a3 0
  sum = _mm_add_epi32(a0_a1, a2_a3); // a0+a2, 0, a1+a3, 0
  // sum here: two 32 bit partial result: sum1 0 sum2 0
  __m128i sum_hi = _mm_castps_si128(_mm_movehl_ps(_mm_castsi128_ps(sum), _mm_castsi128_ps(sum)));
  // __m128i sum_hi  _mm_unpackhi_epi64(sum, zero); // a1 + a3. 2 dwords right 
  sum = _mm_add_epi32(sum, sum_hi);  // a0 + a2 + a1 + a3
#endif
  unsigned int result = _mm_cvtsi128_si32(sum);

#ifdef __AVX__
  _mm256_zeroupper(); // diff from main sse2, paranoia, bacause here we have no ymm regs
#endif

  return result;
}

template<int nBlkHeight>
unsigned int Sad16_sse2_12xN_avx(const uint8_t *pSrc, int nSrcPitch, const uint8_t *pRef, int nRefPitch)
{
#ifdef __AVX__
  _mm256_zeroupper(); // diff from main sse2
#endif

  __m128i zero = _mm_setzero_si128();
  __m128i sum = _mm_setzero_si128(); // 2x or 4x int is probably enough for 32x32
  
  for (int y = 0; y < nBlkHeight; y += 1)
  {
    // BlkSizeX==12: 8+4 pixels, 16+8 bytes
    auto src1 = _mm_load_si128((__m128i *) (pSrc));
    auto src2 = _mm_loadu_si128((__m128i *) (pRef));
    __m128i greater_t = _mm_subs_epu16(src1, src2); // unsigned sub with saturation
    __m128i smaller_t = _mm_subs_epu16(src2, src1);
    __m128i absdiff = _mm_or_si128(greater_t, smaller_t); //abs(s1-s2)  == (satsub(s1,s2) | satsub(s2,s1))
    // 8 x uint16 absolute differences
    sum = _mm_add_epi32(sum, _mm_unpacklo_epi16(absdiff, zero));
    sum = _mm_add_epi32(sum, _mm_unpackhi_epi16(absdiff, zero));
    // 2nd 4 pixels
    src1 = _mm_loadl_epi64((__m128i *) (pSrc + 16)); // zeros upper 64 bits
    src2 = _mm_loadl_epi64((__m128i *) (pRef + 16));
    greater_t = _mm_subs_epu16(src1, src2); // unsigned sub with saturation
    smaller_t = _mm_subs_epu16(src2, src1);
    absdiff = _mm_or_si128(greater_t, smaller_t); //abs(s1-s2)  == (satsub(s1,s2) | satsub(s2,s1))
    sum = _mm_add_epi32(sum, _mm_unpacklo_epi16(absdiff, zero));
    sum = _mm_add_epi32(sum, _mm_unpackhi_epi16(absdiff, zero));
    // sum1_32, sum2_32, sum3_32, sum4_32
    pSrc += nSrcPitch;
    pRef += nRefPitch;
  }
#ifdef __AVX__ // avx or avx2
  __m128i sum1 = _mm_hadd_epi32(sum, sum);
  sum = _mm_hadd_epi32(sum1, sum1);
#else
  // at 16 bits: we have 4 integers for sum: a0 a1 a2 a3
  __m128i a0_a1 = _mm_unpacklo_epi32(sum, zero); // a0 0 a1 0
  __m128i a2_a3 = _mm_unpackhi_epi32(sum, zero); // a2 0 a3 0
  sum = _mm_add_epi32(a0_a1, a2_a3); // a0+a2, 0, a1+a3, 0
                                     // sum here: two 32 bit partial result: sum1 0 sum2 0
  __m128i sum_hi = _mm_castps_si128(_mm_movehl_ps(_mm_castsi128_ps(sum), _mm_castsi128_ps(sum)));
  // __m128i sum_hi  _mm_unpackhi_epi64(sum, zero); // a1 + a3. 2 dwords right 
  sum = _mm_add_epi32(sum, sum_hi);  // a0 + a2 + a1 + a3
#endif

  unsigned int result = _mm_cvtsi128_si32(sum);

#ifdef __AVX__
  _mm256_zeroupper(); // diff from main sse2, paranoia, bacause here we have no ymm regs
#endif

  return result;
}


template<int nBlkHeight>
unsigned int Sad16_sse2_16xN_avx(const uint8_t *pSrc, int nSrcPitch, const uint8_t *pRef, int nRefPitch)
{
  __m128i zero = _mm_setzero_si128();
  __m128i sum = _mm_setzero_si128();

  for (int y = 0; y < nBlkHeight; y++)
  {
    // 16 pixels: 2x8 pixels = 2x16 bytes
    // 2nd 8 pixels
    auto src1 = _mm_load_si128((__m128i *) (pSrc));
    auto src2 = _mm_loadu_si128((__m128i *) (pRef));
    __m128i greater_t = _mm_subs_epu16(src1, src2); // unsigned sub with saturation
    __m128i smaller_t = _mm_subs_epu16(src2, src1);
    __m128i absdiff = _mm_or_si128(greater_t, smaller_t); //abs(s1-s2)  == (satsub(s1,s2) | satsub(s2,s1))
                                                          // 8 x uint16 absolute differences
    sum = _mm_add_epi32(sum, _mm_unpacklo_epi16(absdiff, zero));
    sum = _mm_add_epi32(sum, _mm_unpackhi_epi16(absdiff, zero));
    // 2nd 8 pixels
    src1 = _mm_load_si128((__m128i *) (pSrc + 16));
    src2 = _mm_loadu_si128((__m128i *) (pRef + 16));
    greater_t = _mm_subs_epu16(src1, src2); // unsigned sub with saturation
    smaller_t = _mm_subs_epu16(src2, src1);
    absdiff = _mm_or_si128(greater_t, smaller_t); //abs(s1-s2)  == (satsub(s1,s2) | satsub(s2,s1))
    sum = _mm_add_epi32(sum, _mm_unpacklo_epi16(absdiff, zero));
    sum = _mm_add_epi32(sum, _mm_unpackhi_epi16(absdiff, zero));
    // sum1_32, sum2_32, sum3_32, sum4_32
    pSrc += nSrcPitch;
    pRef += nRefPitch;
  }
#ifdef __AVX__ // avx or avx2
  __m128i sum1 = _mm_hadd_epi32(sum, sum);
  sum = _mm_hadd_epi32(sum1, sum1);
#else
  // at 16 bits: we have 4 integers for sum: a0 a1 a2 a3
    __m128i a0_a1 = _mm_unpacklo_epi32(sum, zero); // a0 0 a1 0
    __m128i a2_a3 = _mm_unpackhi_epi32(sum, zero); // a2 0 a3 0
    sum = _mm_add_epi32(a0_a1, a2_a3); // a0+a2, 0, a1+a3, 0
                                       // sum here: two 32 bit partial result: sum1 0 sum2 0
    __m128i sum_hi = _mm_castps_si128(_mm_movehl_ps(_mm_castsi128_ps(sum), _mm_castsi128_ps(sum)));
    // __m128i sum_hi  _mm_unpackhi_epi64(sum, zero); // a1 + a3. 2 dwords right 
    sum = _mm_add_epi32(sum, sum_hi);  // a0 + a2 + a1 + a3
#endif

  unsigned int result = _mm_cvtsi128_si32(sum);

#ifdef __AVX__
  _mm256_zeroupper(); // diff from main sse2, paranoia, bacause here we have no ymm regs
#endif

  return result;
}


template<int nBlkHeight>
unsigned int Sad16_sse2_24xN_avx(const uint8_t *pSrc, int nSrcPitch, const uint8_t *pRef, int nRefPitch)
{
  __m128i zero = _mm_setzero_si128();
  __m128i sum = _mm_setzero_si128();

  for (int y = 0; y < nBlkHeight; y++)
  {
    auto src1 = _mm_load_si128((__m128i *) (pSrc));
    auto src2 = _mm_loadu_si128((__m128i *) (pRef));
    __m128i greater_t = _mm_subs_epu16(src1, src2); // unsigned sub with saturation
    __m128i smaller_t = _mm_subs_epu16(src2, src1);
    __m128i absdiff = _mm_or_si128(greater_t, smaller_t); //abs(s1-s2)  == (satsub(s1,s2) | satsub(s2,s1))
    // 8 x uint16 absolute differences
    sum = _mm_add_epi32(sum, _mm_unpacklo_epi16(absdiff, zero));
    sum = _mm_add_epi32(sum, _mm_unpackhi_epi16(absdiff, zero));
    // 2nd 8 pixels
    src1 = _mm_load_si128((__m128i *) (pSrc + 16));
    src2 = _mm_loadu_si128((__m128i *) (pRef + + 16));
    greater_t = _mm_subs_epu16(src1, src2); // unsigned sub with saturation
    smaller_t = _mm_subs_epu16(src2, src1);
    absdiff = _mm_or_si128(greater_t, smaller_t); //abs(s1-s2)  == (satsub(s1,s2) | satsub(s2,s1))
    sum = _mm_add_epi32(sum, _mm_unpacklo_epi16(absdiff, zero));
    sum = _mm_add_epi32(sum, _mm_unpackhi_epi16(absdiff, zero));
    // 3rd 8 pixels
    src1 = _mm_load_si128((__m128i *) (pSrc + 32));
    src2 = _mm_loadu_si128((__m128i *) (pRef + 32));
    greater_t = _mm_subs_epu16(src1, src2); // unsigned sub with saturation
    smaller_t = _mm_subs_epu16(src2, src1);
    absdiff = _mm_or_si128(greater_t, smaller_t); //abs(s1-s2)  == (satsub(s1,s2) | satsub(s2,s1))
    sum = _mm_add_epi32(sum, _mm_unpacklo_epi16(absdiff, zero));
    sum = _mm_add_epi32(sum, _mm_unpackhi_epi16(absdiff, zero));
    // sum1_32, sum2_32, sum3_32, sum4_32
    pSrc += nSrcPitch;
    pRef += nRefPitch;
  }
#ifdef __AVX__ // avx or avx2
  __m128i sum1 = _mm_hadd_epi32(sum, sum);
  sum = _mm_hadd_epi32(sum1, sum1);
#else
  // at 16 bits: we have 4 integers for sum: a0 a1 a2 a3
  __m128i a0_a1 = _mm_unpacklo_epi32(sum, zero); // a0 0 a1 0
  __m128i a2_a3 = _mm_unpackhi_epi32(sum, zero); // a2 0 a3 0
  sum = _mm_add_epi32(a0_a1, a2_a3); // a0+a2, 0, a1+a3, 0
                                     // sum here: two 32 bit partial result: sum1 0 sum2 0
  __m128i sum_hi = _mm_castps_si128(_mm_movehl_ps(_mm_castsi128_ps(sum), _mm_castsi128_ps(sum)));
  // __m128i sum_hi  _mm_unpackhi_epi64(sum, zero); // a1 + a3. 2 dwords right 
  sum = _mm_add_epi32(sum, sum_hi);  // a0 + a2 + a1 + a3
#endif

  unsigned int result = _mm_cvtsi128_si32(sum);

#ifdef __AVX__
  _mm256_zeroupper(); // diff from main sse2, paranoia, bacause here we have no ymm regs
#endif

  return result;
}

template<int nBlkHeight>
unsigned int Sad16_sse2_32xN_avx(const uint8_t *pSrc, int nSrcPitch, const uint8_t *pRef, int nRefPitch)
{
  __m128i zero = _mm_setzero_si128();
  __m128i sum = _mm_setzero_si128();

  for (int y = 0; y < nBlkHeight; y++)
  {
    auto src1 = _mm_load_si128((__m128i *) (pSrc));
    auto src2 = _mm_loadu_si128((__m128i *) (pRef));
    __m128i greater_t = _mm_subs_epu16(src1, src2); // unsigned sub with saturation
    __m128i smaller_t = _mm_subs_epu16(src2, src1);
    __m128i absdiff = _mm_or_si128(greater_t, smaller_t); //abs(s1-s2)  == (satsub(s1,s2) | satsub(s2,s1))
    sum = _mm_add_epi32(sum, _mm_unpacklo_epi16(absdiff, zero));
    sum = _mm_add_epi32(sum, _mm_unpackhi_epi16(absdiff, zero));
    // 2nd 8
    src1 = _mm_load_si128((__m128i *) (pSrc + 16));
    src2 = _mm_loadu_si128((__m128i *) (pRef + 16));
    greater_t = _mm_subs_epu16(src1, src2); // unsigned sub with saturation
    smaller_t = _mm_subs_epu16(src2, src1);
    absdiff = _mm_or_si128(greater_t, smaller_t); //abs(s1-s2)  == (satsub(s1,s2) | satsub(s2,s1))
    sum = _mm_add_epi32(sum, _mm_unpacklo_epi16(absdiff, zero));
    sum = _mm_add_epi32(sum, _mm_unpackhi_epi16(absdiff, zero));
    // 3rd 8
    src1 = _mm_load_si128((__m128i *) (pSrc + 32));
    src2 = _mm_loadu_si128((__m128i *) (pRef + 32));
    greater_t = _mm_subs_epu16(src1, src2); // unsigned sub with saturation
    smaller_t = _mm_subs_epu16(src2, src1);
    absdiff = _mm_or_si128(greater_t, smaller_t); //abs(s1-s2)  == (satsub(s1,s2) | satsub(s2,s1))
    sum = _mm_add_epi32(sum, _mm_unpacklo_epi16(absdiff, zero));
    sum = _mm_add_epi32(sum, _mm_unpackhi_epi16(absdiff, zero));
    // 4th 8
    src1 = _mm_load_si128((__m128i *) (pSrc + 32 + 16));
    src2 = _mm_loadu_si128((__m128i *) (pRef + 32 + 16));
    greater_t = _mm_subs_epu16(src1, src2); // unsigned sub with saturation
    smaller_t = _mm_subs_epu16(src2, src1);
    absdiff = _mm_or_si128(greater_t, smaller_t); //abs(s1-s2)  == (satsub(s1,s2) | satsub(s2,s1))
    sum = _mm_add_epi32(sum, _mm_unpacklo_epi16(absdiff, zero));
    sum = _mm_add_epi32(sum, _mm_unpackhi_epi16(absdiff, zero));
  // unroll#2: not any faster. Too many XMM registers are used and prolog/epilog (saving and restoring them) takes a lot of time.
  // sum1_32, sum2_32, sum3_32, sum4_32
    pSrc += nSrcPitch;
    pRef += nRefPitch;
  }
#ifdef __AVX__ // avx or avx2
  __m128i sum1 = _mm_hadd_epi32(sum, sum);
  sum = _mm_hadd_epi32(sum1, sum1);
#else
  // at 16 bits: we have 4 integers for sum: a0 a1 a2 a3
    __m128i a0_a1 = _mm_unpacklo_epi32(sum, zero); // a0 0 a1 0
    __m128i a2_a3 = _mm_unpackhi_epi32(sum, zero); // a2 0 a3 0
    sum = _mm_add_epi32(a0_a1, a2_a3); // a0+a2, 0, a1+a3, 0
  // sum here: two 32 bit partial result: sum1 0 sum2 0
  __m128i sum_hi = _mm_unpackhi_epi64(sum, zero); // a1 + a3. 2 dwords right 
  sum = _mm_add_epi32(sum, sum_hi);  // a0 + a2 + a1 + a3
#endif

  unsigned int result = _mm_cvtsi128_si32(sum);

#ifdef __AVX__
  _mm256_zeroupper(); // diff from main sse2, paranoia, bacause here we have no ymm regs
#endif

  return result;
}

template<int nBlkHeight>
unsigned int Sad16_sse2_48xN_avx(const uint8_t *pSrc, int nSrcPitch, const uint8_t *pRef, int nRefPitch)
{
  __m128i zero = _mm_setzero_si128();
  __m128i sum = _mm_setzero_si128();

  for (int y = 0; y < nBlkHeight; y++)
  {
    // BlockSizeX==48: 3x(8+8) pixels cycles, 3x32 bytes
    for (int x = 0; x < 48 * 2; x += 32)
    {
      // 1st 8 pixels
      auto src1 = _mm_load_si128((__m128i *) (pSrc + x));
      auto src2 = _mm_loadu_si128((__m128i *) (pRef + x));
      __m128i greater_t = _mm_subs_epu16(src1, src2); // unsigned sub with saturation
      __m128i smaller_t = _mm_subs_epu16(src2, src1);
      __m128i absdiff = _mm_or_si128(greater_t, smaller_t); //abs(s1-s2)  == (satsub(s1,s2) | satsub(s2,s1))
      sum = _mm_add_epi32(sum, _mm_unpacklo_epi16(absdiff, zero));
      sum = _mm_add_epi32(sum, _mm_unpackhi_epi16(absdiff, zero));
      // 2nd 8 pixels
      src1 = _mm_load_si128((__m128i *) (pSrc + x + 16));
      src2 = _mm_loadu_si128((__m128i *) (pRef + x + 16));
      greater_t = _mm_subs_epu16(src1, src2); // unsigned sub with saturation
      smaller_t = _mm_subs_epu16(src2, src1);
      absdiff = _mm_or_si128(greater_t, smaller_t); //abs(s1-s2)  == (satsub(s1,s2) | satsub(s2,s1))
      sum = _mm_add_epi32(sum, _mm_unpacklo_epi16(absdiff, zero));
      sum = _mm_add_epi32(sum, _mm_unpackhi_epi16(absdiff, zero));
      // sum1_32, sum2_32, sum3_32, sum4_32
    }
    pSrc += nSrcPitch;
    pRef += nRefPitch;
  }
#ifdef __AVX__ // avx or avx2
  __m128i sum1 = _mm_hadd_epi32(sum, sum);
  sum = _mm_hadd_epi32(sum1, sum1);
#else
  // at 16 bits: we have 4 integers for sum: a0 a1 a2 a3
  __m128i a0_a1 = _mm_unpacklo_epi32(sum, zero); // a0 0 a1 0
  __m128i a2_a3 = _mm_unpackhi_epi32(sum, zero); // a2 0 a3 0
  sum = _mm_add_epi32(a0_a1, a2_a3); // a0+a2, 0, a1+a3, 0
                                     // sum here: two 32 bit partial result: sum1 0 sum2 0
  __m128i sum_hi = _mm_unpackhi_epi64(sum, zero); // a1 + a3. 2 dwords right 
  sum = _mm_add_epi32(sum, sum_hi);  // a0 + a2 + a1 + a3
#endif

  unsigned int result = _mm_cvtsi128_si32(sum);

#ifdef __AVX__
  _mm256_zeroupper(); // diff from main sse2, paranoia, bacause here we have no ymm regs
#endif

  return result;
}


template<int nBlkHeight>
unsigned int Sad16_sse2_64xN_avx(const uint8_t *pSrc, int nSrcPitch, const uint8_t *pRef, int nRefPitch)
{
  __m128i zero = _mm_setzero_si128();
  __m128i sum = _mm_setzero_si128();

  for (int y = 0; y < nBlkHeight; y++)
  {
    // BlockSizeX==64: 2x16 pixels cycles, 2x64 bytes
    for (int x = 0; x < 64*2; x += 32)
    {
      // 1st 8
      auto src1 = _mm_load_si128((__m128i *) (pSrc + x));
      auto src2 = _mm_loadu_si128((__m128i *) (pRef + x));
      __m128i greater_t = _mm_subs_epu16(src1, src2); // unsigned sub with saturation
      __m128i smaller_t = _mm_subs_epu16(src2, src1);
      __m128i absdiff = _mm_or_si128(greater_t, smaller_t); //abs(s1-s2)  == (satsub(s1,s2) | satsub(s2,s1))
      sum = _mm_add_epi32(sum, _mm_unpacklo_epi16(absdiff, zero));
      sum = _mm_add_epi32(sum, _mm_unpackhi_epi16(absdiff, zero));
      // 2nd 8
      src1 = _mm_load_si128((__m128i *) (pSrc + x + 16));
      src2 = _mm_loadu_si128((__m128i *) (pRef + x + 16));
      greater_t = _mm_subs_epu16(src1, src2); // unsigned sub with saturation
      smaller_t = _mm_subs_epu16(src2, src1);
      absdiff = _mm_or_si128(greater_t, smaller_t); //abs(s1-s2)  == (satsub(s1,s2) | satsub(s2,s1))
      sum = _mm_add_epi32(sum, _mm_unpacklo_epi16(absdiff, zero));
      sum = _mm_add_epi32(sum, _mm_unpackhi_epi16(absdiff, zero));
    }
    pSrc += nSrcPitch;
    pRef += nRefPitch;
  }
#ifdef __AVX__ // avx or avx2
  __m128i sum1 = _mm_hadd_epi32(sum, sum);
  sum = _mm_hadd_epi32(sum1, sum1);
#else
  // at 16 bits: we have 4 integers for sum: a0 a1 a2 a3
  __m128i a0_a1 = _mm_unpacklo_epi32(sum, zero); // a0 0 a1 0
  __m128i a2_a3 = _mm_unpackhi_epi32(sum, zero); // a2 0 a3 0
  sum = _mm_add_epi32(a0_a1, a2_a3); // a0+a2, 0, a1+a3, 0
                                     // sum here: two 32 bit partial result: sum1 0 sum2 0
  __m128i sum_hi = _mm_unpackhi_epi64(sum, zero); // a1 + a3. 2 dwords right 
  sum = _mm_add_epi32(sum, sum_hi);  // a0 + a2 + a1 + a3
#endif

  unsigned int result = _mm_cvtsi128_si32(sum);

#ifdef __AVX__
  _mm256_zeroupper(); // diff from main sse2, paranoia, bacause here we have no ymm regs
#endif

  return result;
}


