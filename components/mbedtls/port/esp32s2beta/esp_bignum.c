/**
 * \brief  Multi-precision integer library, ESP32C hardware accelerated parts
 *
 *  based on mbedTLS implementation
 *
 *  Copyright (C) 2006-2015, ARM Limited, All Rights Reserved
 *  Additions Copyright (C) 2016, Espressif Systems (Shanghai) PTE Ltd
 *  SPDX-License-Identifier: Apache-2.0
 *
 *  Licensed under the Apache License, Version 2.0 (the "License"); you may
 *  not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 *  WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 */
#include <stdio.h>
#include <string.h>
#include <malloc.h>
#include <limits.h>
#include <assert.h>
#include <stdlib.h>
#include <sys/param.h>
#include "mbedtls/bignum.h"
#include "esp32s2beta/rom/bigint.h"
#include "soc/hwcrypto_reg.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_intr.h"
#include "esp_intr_alloc.h"
#include "esp_attr.h"

#include "soc/dport_reg.h"
#include "soc/periph_defs.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const __attribute__((unused)) char *TAG = "bignum";

#define ciL    (sizeof(mbedtls_mpi_uint))         /* chars in limb  */
#define biL    (ciL << 3)                         /* bits  in limb  */


static _lock_t mpi_lock;

void esp_mpi_acquire_hardware( void )
{
    /* newlib locks lazy initialize on ESP-IDF */
    _lock_acquire(&mpi_lock);

    DPORT_REG_SET_BIT(DPORT_PERIP_CLK_EN1_REG, DPORT_CRYPTO_RSA_CLK_EN);
    /* also clear reset on digital signature, otherwise RSA is held in reset */
    DPORT_REG_CLR_BIT(DPORT_PERIP_RST_EN1_REG, DPORT_CRYPTO_RSA_RST
                      | DPORT_CRYPTO_DS_RST);

    DPORT_REG_CLR_BIT(DPORT_RSA_PD_CTRL_REG, DPORT_RSA_MEM_PD);

    while (DPORT_REG_READ(RSA_QUERY_CLEAN_REG) != 1) {
    }
    // Note: from enabling RSA clock to here takes about 1.3us
}

void esp_mpi_release_hardware( void )
{
    DPORT_REG_SET_BIT(DPORT_RSA_PD_CTRL_REG, DPORT_RSA_PD);

    /* don't reset digital signature unit, as this resets AES also */
    DPORT_REG_SET_BIT(DPORT_PERIP_RST_EN1_REG, DPORT_CRYPTO_RSA_RST);
    DPORT_REG_CLR_BIT(DPORT_PERIP_CLK_EN1_REG, DPORT_CRYPTO_RSA_CLK_EN);

    _lock_release(&mpi_lock);
}

/* Convert bit count to word count
 */
static inline size_t bits_to_words(size_t bits)
{
    return (bits + 31) / 32;
}


/* Return the number of words actually used to represent an mpi
   number.
*/
static size_t mpi_words(const mbedtls_mpi *mpi)
{
    for (size_t i = mpi->n; i > 0; i--) {
        if (mpi->p[i - 1] != 0) {
            return i;
        }
    }
    return 0;
}

/* Copy mbedTLS MPI bignum 'mpi' to hardware memory block at 'mem_base'.

   If num_words is higher than the number of words in the bignum then
   these additional words will be zeroed in the memory buffer.
*/
static inline void mpi_to_mem_block(uint32_t mem_base, const mbedtls_mpi *mpi, size_t num_words)
{
    uint32_t *pbase = (uint32_t *)mem_base;
    uint32_t copy_words = num_words < mpi->n ? num_words : mpi->n;

    /* Copy MPI data to memory block registers */
    for (int i = 0; i < copy_words; i++) {
        pbase[i] = mpi->p[i];
    }

    /* Zero any remaining memory block data */
    for (int i = copy_words; i < num_words; i++) {
        pbase[i] = 0;
    }

    /* Note: not executing memw here, can do it before we start a bignum operation */
}

/* Read mbedTLS MPI bignum back from hardware memory block.

   Reads num_words words from block.

   Can return a failure result if fails to grow the MPI result.

*/
static inline int mem_block_to_mpi(mbedtls_mpi *x, uint32_t mem_base, int num_words)
{
    int ret = 0;

    MBEDTLS_MPI_CHK( mbedtls_mpi_grow(x, num_words) );

    /* Copy data from memory block registers */
    esp_dport_access_read_buffer(x->p, mem_base, num_words);
    /* Zero any remaining limbs in the bignum, if the buffer is bigger
       than num_words */
    for (size_t i = num_words; i < x->n; i++) {
        x->p[i] = 0;
    }

    asm volatile ("memw");
cleanup:
    return ret;
}


/**
 *
 * There is a need for the value of integer N' such that B^-1(B-1)-N^-1N'=1,
 * where B^-1(B-1) mod N=1. Actually, only the least significant part of
 * N' is needed, hence the definition N0'=N' mod b. We reproduce below the
 * simple algorithm from an article by Dusse and Kaliski to efficiently
 * find N0' from N0 and b
 */
static mbedtls_mpi_uint modular_inverse(const mbedtls_mpi *M)
{
    int i;
    uint64_t t = 1;
    uint64_t two_2_i_minus_1 = 2;   /* 2^(i-1) */
    uint64_t two_2_i = 4;           /* 2^i */
    uint64_t N = M->p[0];

    for (i = 2; i <= 32; i++) {
        if ((mbedtls_mpi_uint) N * t % two_2_i >= two_2_i_minus_1) {
            t += two_2_i_minus_1;
        }

        two_2_i_minus_1 <<= 1;
        two_2_i <<= 1;
    }

    return (mbedtls_mpi_uint)(UINT32_MAX - t + 1);
}

/* Calculate Rinv = RR^2 mod M, where:
 *
 *  R = b^n where b = 2^32, n=num_words,
 *  R = 2^N (where N=num_bits)
 *  RR = R^2 = 2^(2*N) (where N=num_bits=num_words*32)
 *
 * This calculation is computationally expensive (mbedtls_mpi_mod_mpi)
 * so caller should cache the result where possible.
 *
 * DO NOT call this function while holding esp_mpi_acquire_hardware().
 *
 */
static int calculate_rinv(mbedtls_mpi *Rinv, const mbedtls_mpi *M, int num_words)
{
    int ret;
    size_t num_bits = num_words * 32;
    mbedtls_mpi RR;
    mbedtls_mpi_init(&RR);
    MBEDTLS_MPI_CHK(mbedtls_mpi_set_bit(&RR, num_bits * 2, 1));
    MBEDTLS_MPI_CHK(mbedtls_mpi_mod_mpi(Rinv, &RR, M));

cleanup:
    mbedtls_mpi_free(&RR);
    return ret;
}


/* Begin an RSA operation. op_reg specifies which 'START' register
   to write to.
*/
static inline void start_op(uint32_t op_reg)
{
    /* Clear interrupt status */
    DPORT_REG_WRITE(RSA_CLEAR_INTERRUPT_REG, 1);
    DPORT_REG_WRITE(RSA_INTERRUPT_REG, 1);

    /* Note: above REG_WRITE includes a memw, so we know any writes
       to the memory blocks are also complete. */

    DPORT_REG_WRITE(op_reg, 1);
}

/* Wait for an RSA operation to complete.
*/
static inline void wait_op_complete(uint32_t op_reg)
{
    while(DPORT_REG_READ(RSA_QUERY_INTERRUPT_REG) != 1)
       { }

    /* clear the interrupt */
    DPORT_REG_WRITE(RSA_CLEAR_INTERRUPT_REG, 1);
}

/* Z = (X * Y) mod M

   Not an mbedTLS function
*/
int esp_mpi_mul_mpi_mod(mbedtls_mpi *Z, const mbedtls_mpi *X, const mbedtls_mpi *Y, const mbedtls_mpi *M)
{

    int ret;
    size_t y_bits = mbedtls_mpi_bitlen(Y);
    size_t x_words = mpi_words(X);
    size_t y_words = mpi_words(Y);
    size_t m_words = mpi_words(M);
    mbedtls_mpi Rinv;
    mbedtls_mpi_uint Mprime;

    size_t num_words = MAX(MAX(m_words, x_words), y_words);

    if (num_words * 32 > 4096) {
        return MBEDTLS_ERR_MPI_NOT_ACCEPTABLE;
    }

    /* Calculate and load the first stage montgomery multiplication */
    mbedtls_mpi_init(&Rinv);
    MBEDTLS_MPI_CHK(calculate_rinv(&Rinv, M, num_words));
    Mprime = modular_inverse(M);

    esp_mpi_acquire_hardware();

    DPORT_REG_WRITE(RSA_LENGTH_REG, (num_words-1));
    DPORT_REG_WRITE(RSA_M_DASH_REG, (uint32_t)Mprime);

    /* Load M, X, Rinv, Mprime (Mprime is mod 2^32) */
    mpi_to_mem_block(RSA_MEM_M_BLOCK_BASE, M, num_words);
    mpi_to_mem_block(RSA_MEM_RB_BLOCK_BASE, &Rinv, num_words);
    mpi_to_mem_block(RSA_MEM_X_BLOCK_BASE, X, num_words);
    mpi_to_mem_block(RSA_MEM_Y_BLOCK_BASE, Y, num_words);

    /* Enable acceleration options */
    DPORT_REG_WRITE(RSA_CONSTANT_TIME_REG, 0);
    DPORT_REG_WRITE(RSA_SEARCH_OPEN_REG, 1);
    DPORT_REG_WRITE(RSA_SEARCH_POS_REG, y_bits - 1);

    /* Execute first stage montgomery multiplication */
    start_op(RSA_MOD_MULT_START_REG);
    wait_op_complete(RSA_MOD_MULT_START_REG);

    DPORT_REG_WRITE(RSA_SEARCH_OPEN_REG, 1);

    mem_block_to_mpi(Z, RSA_MEM_Z_BLOCK_BASE, m_words);

    esp_mpi_release_hardware();

cleanup:
    mbedtls_mpi_free(&Rinv);
    return ret;
}

#if defined(MBEDTLS_MPI_EXP_MOD_ALT)

/*
 * Sliding-window exponentiation: Z = X^Y mod M  (HAC 14.85)
 *
 * _Rinv is optional pre-calculated version of Rinv (via calculate_rinv()).
 *
 * (See RSA Accelerator section in Technical Reference for more about Mprime, Rinv)
 *
 */
int mbedtls_mpi_exp_mod( mbedtls_mpi *Z, const mbedtls_mpi *X, const mbedtls_mpi *Y, const mbedtls_mpi *M, mbedtls_mpi *_Rinv )
{
    int ret = 0;
    size_t y_bits = mbedtls_mpi_bitlen(Y);
    size_t x_words = mpi_words(X);
    size_t y_words = mpi_words(Y);
    size_t m_words = mpi_words(M);
    size_t num_words;

    mbedtls_mpi Rinv_new; /* used if _Rinv == NULL */
    mbedtls_mpi *Rinv;    /* points to _Rinv (if not NULL) othwerwise &RR_new */
    mbedtls_mpi_uint Mprime;

    /* "all numbers must be the same length", so choose longest number
       as cardinal length of operation...
    */
    num_words = MAX(m_words, MAX(x_words, y_words));

    if (mbedtls_mpi_cmp_int(M, 0) <= 0 || (M->p[0] & 1) == 0) {
        return MBEDTLS_ERR_MPI_BAD_INPUT_DATA;
    }

    if (mbedtls_mpi_cmp_int(Y, 0) < 0) {
        return MBEDTLS_ERR_MPI_BAD_INPUT_DATA;
    }

    if (mbedtls_mpi_cmp_int(Y, 0) == 0) {
        return mbedtls_mpi_lset(Z, 1);
    }

    if (num_words * 32 > 4096) {
        return MBEDTLS_ERR_MPI_NOT_ACCEPTABLE;
    }

    /* Determine RR pointer, either _RR for cached value
       or local RR_new */
    if (_Rinv == NULL) {
        mbedtls_mpi_init(&Rinv_new);
        Rinv = &Rinv_new;
    } else {
        Rinv = _Rinv;
    }
    if (Rinv->p == NULL) {
        MBEDTLS_MPI_CHK(calculate_rinv(Rinv, M, num_words));
    }

    Mprime = modular_inverse(M);

    esp_mpi_acquire_hardware();

    DPORT_REG_WRITE(RSA_LENGTH_REG, num_words - 1);

    /* Load M, X, Rinv, M-prime (M-prime is mod 2^32) */
    mpi_to_mem_block(RSA_MEM_X_BLOCK_BASE, X, num_words);
    mpi_to_mem_block(RSA_MEM_Y_BLOCK_BASE, Y, num_words);
    mpi_to_mem_block(RSA_MEM_M_BLOCK_BASE, M, num_words);
    mpi_to_mem_block(RSA_MEM_RB_BLOCK_BASE, Rinv, num_words);
    DPORT_REG_WRITE(RSA_M_DASH_REG, Mprime);

    /* Enable acceleration options */
    DPORT_REG_WRITE(RSA_CONSTANT_TIME_REG, 0);
    DPORT_REG_WRITE(RSA_SEARCH_OPEN_REG, 1);
    DPORT_REG_WRITE(RSA_SEARCH_POS_REG, y_bits - 1);

    start_op(RSA_MODEXP_START_REG);
    wait_op_complete(RSA_MODEXP_START_REG);

    DPORT_REG_WRITE(RSA_SEARCH_OPEN_REG, 0);

    ret = mem_block_to_mpi(Z, RSA_MEM_Z_BLOCK_BASE, m_words);

    esp_mpi_release_hardware();

    // Compensate for negative X
    if (X->s == -1 && (Y->p[0] & 1) != 0) {
        Z->s = -1;
        MBEDTLS_MPI_CHK(mbedtls_mpi_add_mpi(Z, M, Z));
    } else {
        Z->s = 1;
    }

cleanup:
    if (_Rinv == NULL) {
        mbedtls_mpi_free(&Rinv_new);
    }

    return ret;
}

#endif /* MBEDTLS_MPI_EXP_MOD_ALT */

#if defined(MBEDTLS_MPI_MUL_MPI_ALT) /* MBEDTLS_MPI_MUL_MPI_ALT */

static int mpi_mult_mpi_failover_mod_mult(mbedtls_mpi *Z, const mbedtls_mpi *X, const mbedtls_mpi *Y, size_t z_words);
static int mpi_mult_mpi_overlong(mbedtls_mpi *Z, const mbedtls_mpi *X, const mbedtls_mpi *Y, size_t y_words, size_t z_words);

/* Z = X * Y */
int mbedtls_mpi_mul_mpi( mbedtls_mpi *Z, const mbedtls_mpi *X, const mbedtls_mpi *Y )
{
    int ret = 0;
    size_t x_bits = mbedtls_mpi_bitlen(X);
    size_t y_bits = mbedtls_mpi_bitlen(Y);
    size_t x_words = bits_to_words(x_bits);
    size_t y_words =  bits_to_words(y_bits);
    size_t num_words = MAX(x_words, y_words);
    size_t z_words  = x_words + y_words;

     /* Short-circuit eval if either argument is 0 or 1.

       This is needed as the mpi modular division
       argument will sometimes call in here when one
       argument is too large for the hardware unit, but the other
       argument is zero or one.

       This leaks some timing information, although overall there is a
       lot less timing variation than a software MPI approach.
    */
    if (x_bits == 0 || y_bits== 0) {
        mbedtls_mpi_lset(Z, 0);
        return 0;
    }
    if (x_bits == 1) {
        ret = mbedtls_mpi_copy(Z, Y);
        Z->s *= X->s;
        return ret;
    }
    if (y_bits == 1) {
        ret = mbedtls_mpi_copy(Z, X);
        Z->s *= Y->s;
        return ret;
    }

    /* If either factor is over 2048 bits, we can't use the standard hardware multiplier
       (it assumes result is double longest factor, and result is max 4096 bits.)

       However, we can fail over to mod_mult for up to 4096 bits of result (modulo
       multiplication doesn't have the same restriction, so result is simply the
       number of bits in X plus number of bits in in Y.)
    */



    if (num_words * 32 > 2048) {
        if (z_words * 32 <= 4096) {
            /* Note: it's possible to use mpi_mult_mpi_overlong
               for this case as well, but it's very slightly
               slower and requires a memory allocation.
            */
            return mpi_mult_mpi_failover_mod_mult(Z, X, Y, z_words);
        } else {
            /* Still too long for the hardware unit... */
            mbedtls_mpi_grow(Z, z_words);
            if(y_words > x_words) {
                return mpi_mult_mpi_overlong(Z, X, Y, y_words, z_words);
            } else {
                return mpi_mult_mpi_overlong(Z, Y, X, x_words, z_words);
            }
        }
    }

    /* Otherwise, we can use the (faster) multiply hardware unit */
    esp_mpi_acquire_hardware();

    /* Copy X (right-extended) & Y (left-extended) to memory block */
    mpi_to_mem_block(RSA_MEM_X_BLOCK_BASE, X, num_words);
    mpi_to_mem_block(RSA_MEM_Z_BLOCK_BASE + num_words * 4, Y, num_words);
    /* NB: as Y is left-extended, we don't zero the bottom words_mult words of Y block.
       This is OK for now because zeroing is done by hardware when we do esp_mpi_acquire_hardware().
    */

    DPORT_REG_WRITE(RSA_M_DASH_REG, 0);
    DPORT_REG_WRITE(RSA_LENGTH_REG, (num_words*2 - 1));
    start_op(RSA_MULT_START_REG);

    wait_op_complete(RSA_MULT_START_REG);

    /* Read back the result */
    ret = mem_block_to_mpi(Z, RSA_MEM_Z_BLOCK_BASE, z_words);

    Z->s = X->s * Y->s;

    esp_mpi_release_hardware();

    return ret;
}

/* Special-case of mbedtls_mpi_mult_mpi(), where we use hardware montgomery mod
   multiplication to calculate an mbedtls_mpi_mult_mpi result where either
   A or B are >2048 bits so can't use the standard multiplication method.

   Result (number of words, based on A bits + B bits) must still be less than 4096 bits.

   This case is simpler than the general case modulo multiply of
   esp_mpi_mul_mpi_mod() because we can control the other arguments:

   * Modulus is chosen with M=(2^num_bits - 1) (ie M=R-1), so output
   isn't actually modulo anything.
   * Mprime and Rinv are therefore predictable as follows:
   Mprime = 1
   Rinv = 1

   (See RSA Accelerator section in Technical Reference for more about Mprime, Rinv)
*/
static int mpi_mult_mpi_failover_mod_mult(mbedtls_mpi *Z, const mbedtls_mpi *X, const mbedtls_mpi *Y, size_t num_words)
{
    int ret = 0;

    /* Load coefficients to hardware */
    esp_mpi_acquire_hardware();

    /* M = 2^num_words - 1, so block is entirely FF */
    for (int i = 0; i < num_words; i++) {
        DPORT_REG_WRITE(RSA_MEM_M_BLOCK_BASE + i * 4, UINT32_MAX);
    }

    /* Mprime = 1 */
    DPORT_REG_WRITE(RSA_M_DASH_REG, 1);
    DPORT_REG_WRITE(RSA_LENGTH_REG, num_words - 1);

    /* Load X & Y */
    mpi_to_mem_block(RSA_MEM_X_BLOCK_BASE, X, num_words);
    mpi_to_mem_block(RSA_MEM_Y_BLOCK_BASE, Y, num_words);

    /* Rinv = 1 */
    DPORT_REG_WRITE(RSA_MEM_RB_BLOCK_BASE, 1);
    for (int i = 1; i < num_words; i++) {
        DPORT_REG_WRITE(RSA_MEM_RB_BLOCK_BASE + i * 4, 0);
    }

    start_op(RSA_MOD_MULT_START_REG);
    wait_op_complete(RSA_MOD_MULT_START_REG);

    mem_block_to_mpi(Z, RSA_MEM_Z_BLOCK_BASE, num_words);

    esp_mpi_release_hardware();

    return ret;
}

/* Deal with the case when X & Y are too long for the hardware unit, by splitting one operand
   into two halves.

   Y must be the longer operand

   Slice Y into Yp, Ypp such that:
   Yp = lower 'b' bits of Y
   Ypp = upper 'b' bits of Y (right shifted)

   Such that
   Z = X * Y
   Z = X * (Yp + Ypp<<b)
   Z = (X * Yp) + (X * Ypp<<b)

   Note that this function may recurse multiple times, if both X & Y
   are too long for the hardware multiplication unit.
*/
static int mpi_mult_mpi_overlong(mbedtls_mpi *Z, const mbedtls_mpi *X, const mbedtls_mpi *Y, size_t y_words, size_t z_words)
{
    int ret = 0;
    mbedtls_mpi Ztemp;
    /* Rather than slicing in two on bits we slice on limbs (32 bit words) */
    const size_t words_slice = y_words / 2;
    /* Yp holds lower bits of Y (declared to reuse Y's array contents to save on copying) */
    const mbedtls_mpi Yp = {
        .p = Y->p,
        .n = words_slice,
        .s = Y->s
    };
    /* Ypp holds upper bits of Y, right shifted (also reuses Y's array contents) */
    const mbedtls_mpi Ypp = {
        .p = Y->p + words_slice,
        .n = y_words - words_slice,
        .s = Y->s
    };
    mbedtls_mpi_init(&Ztemp);

    /* Get result Ztemp = Yp * X (need temporary variable Ztemp) */
    MBEDTLS_MPI_CHK( mbedtls_mpi_mul_mpi(&Ztemp, X, &Yp) );

    /* Z = Ypp * Y */
    MBEDTLS_MPI_CHK( mbedtls_mpi_mul_mpi(Z, X, &Ypp) );

    /* Z = Z << b */
    MBEDTLS_MPI_CHK( mbedtls_mpi_shift_l(Z, words_slice * 32) );

    /* Z += Ztemp */
    MBEDTLS_MPI_CHK( mbedtls_mpi_add_mpi(Z, Z, &Ztemp) );

cleanup:
    mbedtls_mpi_free(&Ztemp);

    return ret;
}

#endif /* MBEDTLS_MPI_MUL_MPI_ALT */

