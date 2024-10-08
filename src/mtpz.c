/**
 * \file mtpz.c
 *
 * Copyright (C) 2011-2012 Sajid Anwar <sajidanwar94@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 * This file provides mtp zune cryptographic setup interfaces.
 * It is also used with Windows Phone 7, but Microsoft/Nokiad seem
 * to have discontinued MTPZ on Windows Phone 8.
 *
 * DISCLAIMER:
 *
 * The intention of this implementation is for users to be able
 * to interoperate with their devices, i.e. copy music to them in
 * operating systems other than Microsoft Windows, so it can be
 * played back on the device. We do not provide encryption keys
 * and constants in libmtp, we never will. You have to have these
 * on file in your home directory in $HOME/.mtpz-data, and we suggest
 * that you talk to Microsoft about providing the proper numbers if
 * you want to use this facility.
 */
#include "config.h"
#include "libmtp.h"
#include "unicode.h"
#include "ptp.h"
#include "libusb-glue.h"
#include "device-flags.h"
#include "playlist-spl.h"
#include "util.h"
#include "mtpz.h"

#include <gcrypt.h>

#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <time.h>
#include <errno.h>


/* Microsoft MTPZ extensions */

/*
 * The ~/.mtpz-data file contains all four necessary pieces of data:
 *
 *   encryption key
 *   public exponent
 *   modulus
 *   private key
 *   certificate data
 *
 * These four pieces of data are each stored in hex representation,
 * separated by newline characters.
 *
 * If you know of a published, public reference for one of these
 * arrays of data, please inform us, so we can include it here and
 * drop it from the external file. Even better is if you convince
 * Microsoft to officially provide keys to this project.
 */

static unsigned char *MTPZ_ENCRYPTION_KEY;
static unsigned char *MTPZ_PUBLIC_EXPONENT;
static unsigned char *MTPZ_MODULUS;
static unsigned char *MTPZ_PRIVATE_KEY;
static char *MTPZ_CERTIFICATES;

// Strip the trailing newline from fgets().
static char *fgets_strip(char * str, int num, FILE * stream)
{
	char *result = str;

	if ((result = fgets(str, num, stream)))
	{
		size_t newlen = strlen(result);

		if (result[newlen - 1] == '\n')
			result[newlen - 1] = '\0';
	}

	return result;
}

static char *hex_to_bytes(char *hex, size_t len)
{
	if (len % 2)
		return NULL;

	char *bytes = malloc(len / 2);
	unsigned int u;
	unsigned int i = 0;

	while (i < len && sscanf(hex + i, "%2x", &u) == 1)
	{
		bytes[i / 2] = u;
		i += 2;
	}

	return bytes;
}

int mtpz_loaddata()
{
	char *home = getenv("HOME");
	int ret = -1;
	if (!home)
	{
		LIBMTP_ERROR("Unable to determine user's home directory, MTPZ disabled.\n");
		return -1;
	}

	int plen = strlen(home) + strlen("/.mtpz-data") + 1;
	char path[plen];
	sprintf(path, "%s/.mtpz-data", home);

	FILE *fdata = fopen(path, "r");
	if (!fdata)
		/*文件不存在*/
		return ret;

	// Should only be six characters in length, but fgets will encounter a newline and stop.
	MTPZ_PUBLIC_EXPONENT = (unsigned char *)fgets_strip((char *)malloc(8), 8, fdata);
	if (!MTPZ_PUBLIC_EXPONENT)
	{
		LIBMTP_ERROR("Unable to read MTPZ public exponent from ~/.mtpz-data, MTPZ disabled.\n");
		goto cleanup;
	}

	// Should only be 33 characters in length, but fgets will encounter a newline and stop.
	char *hexenckey = fgets_strip((char *)malloc(35), 35, fdata);
	if (!hexenckey)
	{
		LIBMTP_ERROR("Unable to read MTPZ encryption key from ~/.mtpz-data, MTPZ disabled.\n");
		goto cleanup;
	}

	MTPZ_ENCRYPTION_KEY = hex_to_bytes(hexenckey, strlen(hexenckey));
	if (!MTPZ_ENCRYPTION_KEY)
	{
		LIBMTP_ERROR("Unable to read MTPZ encryption key from ~/.mtpz-data, MTPZ disabled.\n");
		goto cleanup;
	}

	// Should only be 256 characters in length, but fgets will encounter a newline and stop.
	MTPZ_MODULUS = (unsigned char *)fgets_strip((char *)malloc(260), 260, fdata);
	if (!MTPZ_MODULUS)
	{
		LIBMTP_ERROR("Unable to read MTPZ modulus from ~/.mtpz-data, MTPZ disabled.\n");
		goto cleanup;
	}

	// Should only be 256 characters in length, but fgets will encounter a newline and stop.
	MTPZ_PRIVATE_KEY = (unsigned char *)fgets_strip((char *)malloc(260), 260, fdata);
	if (!MTPZ_PRIVATE_KEY)
	{
		LIBMTP_ERROR("Unable to read MTPZ private key from ~/.mtpz-data, MTPZ disabled.\n");
		goto cleanup;
	}

	// Should only be 1258 characters in length, but fgets will encounter the end of the file and stop.
	char *hexcerts = fgets_strip((char *)malloc(1260), 1260, fdata);
	if (!hexcerts)
	{
		LIBMTP_ERROR("Unable to read MTPZ certificates from ~/.mtpz-data, MTPZ disabled.\n");
		goto cleanup;
	}

	MTPZ_CERTIFICATES = hex_to_bytes(hexcerts, strlen(hexcerts));
	if (!MTPZ_CERTIFICATES)
	{
		LIBMTP_ERROR("Unable to parse MTPZ certificates from ~/.mtpz-data, MTPZ disabled.\n");
		goto cleanup;
	}
	// If all done without errors, drop the fail
	ret = 0;
cleanup:
	fclose(fdata);
	return ret;
}
/* MTPZ RSA */

typedef struct mtpz_rsa_struct
{
	gcry_sexp_t privkey;
	gcry_sexp_t pubkey;
} mtpz_rsa_t;

mtpz_rsa_t *mtpz_rsa_init(const unsigned char *modulus, const unsigned char *priv_key, const unsigned char *pub_exp);
void mtpz_rsa_free(mtpz_rsa_t *);
int mtpz_rsa_decrypt(int flen, unsigned char *from, int tlen, unsigned char *to, mtpz_rsa_t *rsa);
int mtpz_rsa_sign(int flen, unsigned char *from, int tlen, unsigned char *to, mtpz_rsa_t *rsa);

/* MTPZ hashing */

#define MTPZ_HASHSTATE_84 5
#define MTPZ_HASHSTATE_88 6

static char *mtpz_hash_init_state();
static void mtpz_hash_reset_state(char *);
static void mtpz_hash_transform_hash(char *, char *, int);
static void mtpz_hash_finalize_hash(char *, char *);
static char *mtpz_hash_custom6A5DC(char *, char *, int, int);

static void mtpz_hash_compute_hash(char *, char *, int);
static unsigned int mtpz_hash_f(int s, unsigned int x, unsigned int y, unsigned int z);
static unsigned int mtpz_hash_rotate_left(unsigned int x, int n);

/* MTPZ encryption */

unsigned char mtpz_aes_rcon[];
unsigned char mtpz_aes_sbox[];
unsigned char mtpz_aes_invsbox[];
unsigned int mtpz_aes_ft1[];
unsigned int mtpz_aes_ft2[];
unsigned int mtpz_aes_ft3[];
unsigned int mtpz_aes_ft4[];
unsigned int mtpz_aes_rt1[];
unsigned int mtpz_aes_rt2[];
unsigned int mtpz_aes_rt3[];
unsigned int mtpz_aes_rt4[];
unsigned int mtpz_aes_gb11[];
unsigned int mtpz_aes_gb14[];
unsigned int mtpz_aes_gb13[];
unsigned int mtpz_aes_gb9[];

#define MTPZ_ENCRYPTIONLOBYTE(val) (((val) >> 24) & 0xFF)
#define MTPZ_ENCRYPTIONBYTE1(val) (((val) >> 16) & 0xFF)
#define MTPZ_ENCRYPTIONBYTE2(val) (((val) >>  8) & 0xFF)
#define MTPZ_ENCRYPTIONBYTE3(val) (((val) >>  0) & 0xFF)

#define MTPZ_SWAP(x) mtpz_bswap32(x)

void mtpz_encryption_cipher(unsigned char *data, unsigned int len, char encrypt);
void mtpz_encryption_cipher_advanced(unsigned char *key, unsigned int key_len, unsigned char *data, unsigned int data_len, char encrypt);
unsigned char *mtpz_encryption_expand_key(unsigned char *constant, int key_len, int count, int *out_len);
void mtpz_encryption_expand_key_inner(unsigned char *constant, int key_len, unsigned char **out, int *out_len);
void mtpz_encryption_inv_mix_columns(unsigned char *expanded, int offset, int rounds);
void mtpz_encryption_decrypt_custom(unsigned char *data, unsigned char *seed, unsigned char *expanded);
void mtpz_encryption_encrypt_custom(unsigned char *data, unsigned char *seed, unsigned char *expanded);
void mtpz_encryption_encrypt_mac(unsigned char *hash, unsigned int hash_length, unsigned char *seed, unsigned int seed_len, unsigned char *out);


static inline uint32_t mtpz_bswap32(uint32_t x)
{
#if defined __GNUC__ && (__GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 3)) || defined(__clang__)
	return __builtin_bswap32(x);
#else
	return (x >> 24) |
	       ((x >> 8) & 0x0000ff00) |
	       ((x << 8) & 0x00ff0000) |
	       (x << 24);
#endif
}


/* MTPZ RSA implementation */
mtpz_rsa_t *mtpz_rsa_init(const unsigned char *str_modulus, const unsigned char *str_privkey, const unsigned char *str_pubexp)
{
	mtpz_rsa_t *rsa = calloc(1, sizeof(mtpz_rsa_t));
	if (rsa == NULL)
		return NULL;

	gcry_mpi_t mpi_modulus, mpi_privkey, mpi_pubexp;

	gcry_mpi_scan(&mpi_modulus, GCRYMPI_FMT_HEX, str_modulus, 0, NULL);
	gcry_mpi_scan(&mpi_privkey, GCRYMPI_FMT_HEX, str_privkey, 0, NULL);
	gcry_mpi_scan(&mpi_pubexp, GCRYMPI_FMT_HEX, str_pubexp, 0, NULL);

	gcry_sexp_build(&rsa->privkey, NULL, "(private-key (rsa (n %m) (e %m) (d %m)))", mpi_modulus, mpi_pubexp, mpi_privkey);
	gcry_sexp_build(&rsa->pubkey, NULL, "(public-key (rsa (n %m) (e %m)))", mpi_modulus, mpi_pubexp);

	gcry_mpi_release(mpi_modulus);
	gcry_mpi_release(mpi_privkey);
	gcry_mpi_release(mpi_pubexp);

	return rsa;
}

void mtpz_rsa_free(mtpz_rsa_t *rsa)
{
	gcry_sexp_release(rsa->privkey);
	gcry_sexp_release(rsa->pubkey);
}

int mtpz_rsa_decrypt(int flen, unsigned char *from, int tlen, unsigned char *to, mtpz_rsa_t *rsa)
{
	gcry_mpi_t mpi_from;
	gcry_mpi_scan(&mpi_from, GCRYMPI_FMT_USG, from, flen, NULL);

	gcry_sexp_t sexp_data;
	gcry_sexp_build(&sexp_data, NULL, "(enc-val (flags raw) (rsa (a %m)))", mpi_from);

	gcry_sexp_t sexp_plain;
	gcry_pk_decrypt(&sexp_plain, sexp_data, rsa->privkey);

	gcry_mpi_t mpi_value = gcry_sexp_nth_mpi(sexp_plain, 1, GCRYMPI_FMT_USG);

	// Lame workaround. GCRYMPI_FMT_USG gets rid of any leading zeroes which we do need,
	// so we'll count how many bits are being used, and subtract that from how many bits actually
	// should be there, and then write into our output array shifted over however many bits/8.
	int bitshift = (tlen * 8) - gcry_mpi_get_nbits(mpi_value);
	size_t written;

	if (bitshift / 8)
	{
		memset(to, 0, bitshift / 8);
		to += bitshift / 8;
		tlen -= bitshift / 8;
	}

	gcry_mpi_print(GCRYMPI_FMT_USG, to, tlen, &written, mpi_value);

	gcry_mpi_release(mpi_from);
	gcry_mpi_release(mpi_value);
	gcry_sexp_release(sexp_data);
	gcry_sexp_release(sexp_plain);

	return (int)written;
}

int mtpz_rsa_sign(int flen, unsigned char *from, int tlen, unsigned char *to, mtpz_rsa_t *rsa)
{
	return mtpz_rsa_decrypt(flen, from, tlen, to, rsa);
}

/* MTPZ hashing implementation */

static char *mtpz_hash_init_state()
{
	char *s = (char *)malloc(92);

	if (s != NULL)
		memset(s, 0, 92);

	return s;
}

void mtpz_hash_reset_state(char *state)
{
	int *state_box = (int *)(state + 64);

	/*
	 * Constants from
	 * http://csrc.nist.gov/publications/fips/fips180-2/fips180-2withchangenotice.pdf
	 * Page 13, section 5.3.1
	 */
	state_box[0] = 0x67452301;
	state_box[1] = 0xefcdab89;
	state_box[2] = 0x98badcfe;
	state_box[3] = 0x10325476;
	state_box[4] = 0xc3d2e1f0;
	state_box[MTPZ_HASHSTATE_84] = 0;
	state_box[MTPZ_HASHSTATE_88] = 0;
}

void mtpz_hash_transform_hash(char *state, char *msg, int len)
{
	int *state_box = (int *)(state + 64);

	int x = state_box[MTPZ_HASHSTATE_88] & 0x3F;
	int v5 = len + state_box[MTPZ_HASHSTATE_88];
	state_box[MTPZ_HASHSTATE_88] = v5;

	int i = len, j = 0;
	int a1 = 0;
	int c = 0;

	if (len > v5)
		state_box[MTPZ_HASHSTATE_84] += 1;

	if (x)
	{
		if (len + x > 0x3F)
		{
			for (a1 = 0; a1 < 64 - x; a1++)
			{
				state[x + a1] = msg[a1];
			}

			i = len + x - 64;
			j = 64 - x;

			mtpz_hash_compute_hash(state, state, 64);
		}
	}

	while (i > 63)
	{
		mtpz_hash_compute_hash(state, msg + j, 64);
		j += 64;
		i -= 64;
	}

	if (i != 0)
	{
		for (c = 0; c < i; c++)
		{
			state[x + c] = msg[j + c];
		}
	}
}

// out has at least 20 bytes of space
void mtpz_hash_finalize_hash(char *state, char *out)
{
	int *state_box = (int *)(state + 64);

	int v2 = 64 - (state_box[MTPZ_HASHSTATE_88] & 0x3F);
	int v6, v7;

	if (v2 <= 8)
		v2 += 64;

	char *v5 = (char *)malloc(72);
	memset(v5, 0, 72);

	v5[0] = '\x80';
	v6 = 8 * state_box[MTPZ_HASHSTATE_84] | (state_box[MTPZ_HASHSTATE_88] >> 29);
	v7 = 8 * state_box[MTPZ_HASHSTATE_88];

	v6 = MTPZ_SWAP(v6);
	v7 = MTPZ_SWAP(v7);

	*(int *)(v5 + v2 - 8) = v6;
	*(int *)(v5 + v2 - 4) = v7;

	mtpz_hash_transform_hash(state, v5, v2);

	int *out_int = (int *)out;
	out_int[0] = MTPZ_SWAP(state_box[0]);
	out_int[1] = MTPZ_SWAP(state_box[1]);
	out_int[2] = MTPZ_SWAP(state_box[2]);
	out_int[3] = MTPZ_SWAP(state_box[3]);
	out_int[4] = MTPZ_SWAP(state_box[4]);

	memset(state, 0, 64);
	mtpz_hash_reset_state(state);
}

char *mtpz_hash_custom6A5DC(char *state, char *msg, int len, int a4)
{
	int v11 = (a4 / 20) + 1;
	char *v13 = (char *)malloc(v11 * 20);
	char *v5 = (char *)malloc(len + 4);
	int i;
	int k;

	memset(v13, 0, v11 * 20);
	memset(v5, 0, len + 4);
	memcpy(v5, msg, len);

	for (i = 0; i < v11; i++)
	{
		k = MTPZ_SWAP(i);
		*(int *)(v5 + len) = k;

		mtpz_hash_reset_state(state);
		mtpz_hash_transform_hash(state, v5, len + 4);
		mtpz_hash_finalize_hash(state, v13 + i * 20);
	}

	free(v5); v5 = NULL;

	return v13;
}

void mtpz_hash_compute_hash(char *state, char *msg, int len)
{
	int *state_box = (int *)(state + 64);

	const unsigned int K[] = { 0x5a827999, 0x6ed9eba1, 0x8f1bbcdc, 0xca62c1d6 };

	if (len != 64)
		return;

	int *M = (int *)msg;

	// HASH COMPUTATION
	unsigned int W[80];
	unsigned int a, b, c, d, e;
	int i, s;
	unsigned int T;

	// 1 - prepare message schedule 'W'.
	for (i = 0; i < 16; i++) W[i] = MTPZ_SWAP(M[i]);
	for (i = 16; i < 80; i++) W[i] = mtpz_hash_rotate_left(W[i - 3] ^ W[i - 8] ^ W[i - 14] ^ W[i - 16], 1);

	// 2 - initialize five working variables a, b, c, d, e with previous hash value
	a = state_box[0];
	b = state_box[1];
	c = state_box[2];
	d = state_box[3];
	e = state_box[4];

	// 3 - main loop
	for (i = 0; i < 80; i++)
	{
		s = i / 20;
		T = (mtpz_hash_rotate_left(a, 5) + mtpz_hash_f(s, b, c, d) + e + K[s] + W[i]) & 0xFFFFFFFF;
		e = d;
		d = c;
		c = mtpz_hash_rotate_left(b, 30);
		b = a;
		a = T;
	}

	state_box[0] = (state_box[0] + a) & 0xFFFFFFFF;
	state_box[1] = (state_box[1] + b) & 0xFFFFFFFF;
	state_box[2] = (state_box[2] + c) & 0xFFFFFFFF;
	state_box[3] = (state_box[3] + d) & 0xFFFFFFFF;
	state_box[4] = (state_box[4] + e) & 0xFFFFFFFF;
}

unsigned int mtpz_hash_f(int s, unsigned int x, unsigned int y, unsigned int z)
{
	switch (s)
	{
	case 0:
		return (x & y) ^ (~x & z); // Ch()
	case 1:
		return x ^ y ^ z; // Parity()
	case 2:
		return (x & y) ^ (x & z) ^ (y & z); // Maj()
	case 3:
		return x ^ y ^ z; // Parity()
	}

	return 0;
}

unsigned int mtpz_hash_rotate_left(unsigned int x, int n)
{
	return (x << n) | (x >> (32 - n));
}

/* MTPZ encryption implementation */

void mtpz_encryption_cipher(unsigned char *data, unsigned int len, char encrypt)
{
	unsigned char *expanded = NULL;

	int offset = 0, count = len;

	if ((count & 0x0F) == 0)
	{
		int exp_len = 0;
		expanded = mtpz_encryption_expand_key((unsigned char *)MTPZ_ENCRYPTION_KEY, 16, 10, &exp_len);

		if (count != 0)
		{
			do
			{
				if (encrypt)
					mtpz_encryption_encrypt_custom(data + offset, NULL, expanded);
				else
					mtpz_encryption_decrypt_custom(data + offset, NULL, expanded);

				count -= 16;
				offset += 16;
			}
			while (count != 0);
		}
	}
}

void mtpz_encryption_cipher_advanced(unsigned char *key, unsigned int key_len, unsigned char *data, unsigned int data_len, char encrypt)
{
	int len = (key_len == 16) ? 10 :
			  (key_len == 24) ? 12 : 32;
	int exp_len;
	unsigned char *expanded = mtpz_encryption_expand_key(key, key_len, len, &exp_len);

	int offset = 0, count = data_len;
	unsigned char *out = (unsigned char *)malloc(16);
	unsigned int *out_int = (unsigned int *)out;
	unsigned int *data_int = (unsigned int *)data;
	unsigned int *dtf = (unsigned int *)malloc(16);
	memset((unsigned char *)dtf, 0, 16);

	while (count != 0)
	{
		int chunk = 16;

		if (count < 16)
		{
			memset(out, 0, 16);
			chunk = count;
		}

		memcpy(out, data + offset, chunk);

		if (encrypt)
		{
			out_int[0] ^= MTPZ_SWAP(dtf[0]);
			out_int[1] ^= MTPZ_SWAP(dtf[1]);
			out_int[2] ^= MTPZ_SWAP(dtf[2]);
			out_int[3] ^= MTPZ_SWAP(dtf[3]);

			mtpz_encryption_encrypt_custom(data + offset, out, expanded);

			dtf[0] = MTPZ_SWAP(data_int[(offset / 4) + 0]);
            dtf[1] = MTPZ_SWAP(data_int[(offset / 4) + 1]);
            dtf[2] = MTPZ_SWAP(data_int[(offset / 4) + 2]);
            dtf[3] = MTPZ_SWAP(data_int[(offset / 4) + 3]);
		}
		else
		{
			mtpz_encryption_decrypt_custom(data + offset, out, expanded);

			data_int[(offset / 4) + 0] ^= MTPZ_SWAP(dtf[0]);
			data_int[(offset / 4) + 1] ^= MTPZ_SWAP(dtf[1]);
			data_int[(offset / 4) + 2] ^= MTPZ_SWAP(dtf[2]);
			data_int[(offset / 4) + 3] ^= MTPZ_SWAP(dtf[3]);

			dtf[0] = MTPZ_SWAP(out_int[0]);
			dtf[1] = MTPZ_SWAP(out_int[1]);
			dtf[2] = MTPZ_SWAP(out_int[2]);
			dtf[3] = MTPZ_SWAP(out_int[3]);
		}

		offset += chunk;
		count -= chunk;
	}

	free(out);
	free(dtf);
	free(expanded);
}

unsigned char *mtpz_encryption_expand_key(unsigned char *constant, int key_len, int count, int *out_len)
{
	int i = 0;
	int seek = 0;
	unsigned char *back = (unsigned char *)malloc(484);
	memset(back, 0, 484);
	*out_len = 484;

	unsigned char *inner;
	int inner_len;
	mtpz_encryption_expand_key_inner(constant, key_len, &inner, &inner_len);

	back[i] = (unsigned char)(count % 0xFF);
	i += 4;

	memcpy(back + i, inner, inner_len);
	i += inner_len;
	memcpy(back + i, inner, inner_len);
	i += inner_len;

	switch (count)
	{
	case 10:
		seek = 0xB4;
		break;

	case 12:
		seek = 0xD4;
		break;

	case 14:
	default:
		seek = 0xF4;
		break;
	}

	mtpz_encryption_inv_mix_columns(back, seek, count);

	return back;
}

void mtpz_encryption_expand_key_inner(unsigned char *constant, int key_len, unsigned char **out, int *out_len)
{
	int ks = -1;
	int rcon_i = 0;
	int i = 0, j = 0;

	switch (key_len)
	{
	case 16:
		ks = 16 * (10 + 1);
		break;

	case 24:
		ks = 16 * (12 + 1);
		break;

	case 32:
		ks = 16 * (14 + 1);
		break;

	default:
		*out = NULL;
		*out_len = 0;
		return;
	}

	unsigned char *key = (unsigned char *)malloc(ks);
	unsigned char *temp = (unsigned char *)malloc(4);
	memcpy(key, constant, key_len);
	unsigned char t0, t1, t2, t3;

	for (i = key_len; i < ks; i += 4)
	{
		temp[0] = t0 = key[i - 4];
		temp[1] = t1 = key[i - 3];
		temp[2] = t2 = key[i - 2];
		temp[3] = t3 = key[i - 1];

		if (i % key_len == 0)
		{
			temp[0] = (mtpz_aes_sbox[t1] ^ mtpz_aes_rcon[rcon_i]) & 0xFF;
			temp[1] = mtpz_aes_sbox[t2];
			temp[2] = mtpz_aes_sbox[t3];
			temp[3] = mtpz_aes_sbox[t0];
			rcon_i++;
		}
		else if ((key_len > 24) && (i % key_len == 16))
		{
			temp[0] = mtpz_aes_sbox[t0];
			temp[1] = mtpz_aes_sbox[t1];
			temp[2] = mtpz_aes_sbox[t2];
			temp[3] = mtpz_aes_sbox[t3];
		}

		for (j = 0; j < 4; j++)
		{
			key[i + j] = (unsigned char)((key[i + j - key_len] ^ temp[j]) & 0xFF);
		}
	}

	free(temp);

	*out = key;
	*out_len = ks;
}

void mtpz_encryption_inv_mix_columns(unsigned char *expanded, int offset, int rounds)
{
	int v8 = 1, o = offset;
	unsigned int *exp_int = NULL;

	for (v8 = 1; v8 < rounds; v8++)
	{
		exp_int = (unsigned int *)(expanded + o + 16);

		exp_int[0] = MTPZ_SWAP(mtpz_aes_gb9[expanded[o + 19]] ^ mtpz_aes_gb13[expanded[o + 18]] ^ mtpz_aes_gb11[expanded[o + 17]] ^ mtpz_aes_gb14[expanded[o + 16]]);
		exp_int[1] = MTPZ_SWAP(mtpz_aes_gb9[expanded[o + 23]] ^ mtpz_aes_gb13[expanded[o + 22]] ^ mtpz_aes_gb11[expanded[o + 21]] ^ mtpz_aes_gb14[expanded[o + 20]]);
		exp_int[2] = MTPZ_SWAP(mtpz_aes_gb9[expanded[o + 27]] ^ mtpz_aes_gb13[expanded[o + 26]] ^ mtpz_aes_gb11[expanded[o + 25]] ^ mtpz_aes_gb14[expanded[o + 24]]);
		exp_int[3] = MTPZ_SWAP(mtpz_aes_gb9[expanded[o + 31]] ^ mtpz_aes_gb13[expanded[o + 30]] ^ mtpz_aes_gb11[expanded[o + 29]] ^ mtpz_aes_gb14[expanded[o + 28]]);
		o += 16;
	}
}

void mtpz_encryption_decrypt_custom(unsigned char *data, unsigned char *seed, unsigned char *expanded)
{
	unsigned int *u_data = (unsigned int *)data;
	unsigned int *u_expanded = (unsigned int *)expanded;
	int keyOffset = 0xB4 + 0xA0;

	unsigned int *u_seed;

	if (seed == NULL)
		u_seed = u_data;
	else
		u_seed = (unsigned int *)seed;

	unsigned int v14 = MTPZ_SWAP(u_seed[0]) ^ MTPZ_SWAP(u_expanded[(keyOffset     ) / 4]);
	unsigned int v15 = MTPZ_SWAP(u_seed[1]) ^ MTPZ_SWAP(u_expanded[(keyOffset +  4) / 4]);
	unsigned int v16 = MTPZ_SWAP(u_seed[2]) ^ MTPZ_SWAP(u_expanded[(keyOffset +  8) / 4]);
	unsigned int v17 = MTPZ_SWAP(u_seed[3]) ^ MTPZ_SWAP(u_expanded[(keyOffset + 12) / 4]);

	unsigned int v18 = mtpz_aes_rt1[MTPZ_ENCRYPTIONBYTE3(v15)] ^ mtpz_aes_rt2[MTPZ_ENCRYPTIONBYTE2(v16)] ^ mtpz_aes_rt3[MTPZ_ENCRYPTIONLOBYTE(v14)] ^ mtpz_aes_rt4[MTPZ_ENCRYPTIONBYTE1(v17)];
	unsigned int v19 = mtpz_aes_rt1[MTPZ_ENCRYPTIONBYTE3(v16)] ^ mtpz_aes_rt2[MTPZ_ENCRYPTIONBYTE2(v17)] ^ mtpz_aes_rt3[MTPZ_ENCRYPTIONLOBYTE(v15)] ^ mtpz_aes_rt4[MTPZ_ENCRYPTIONBYTE1(v14)];
	unsigned int v20 = mtpz_aes_rt1[MTPZ_ENCRYPTIONBYTE3(v17)] ^ mtpz_aes_rt2[MTPZ_ENCRYPTIONBYTE2(v14)] ^ mtpz_aes_rt3[MTPZ_ENCRYPTIONLOBYTE(v16)] ^ mtpz_aes_rt4[MTPZ_ENCRYPTIONBYTE1(v15)];
	unsigned int v21 = mtpz_aes_rt1[MTPZ_ENCRYPTIONBYTE3(v14)] ^ mtpz_aes_rt2[MTPZ_ENCRYPTIONBYTE2(v15)] ^ mtpz_aes_rt3[MTPZ_ENCRYPTIONLOBYTE(v17)] ^ mtpz_aes_rt4[MTPZ_ENCRYPTIONBYTE1(v16)];

	keyOffset -= 16;
	int rounds = 9;

	do
	{
		v14 = v18 ^ MTPZ_SWAP(u_expanded[(keyOffset     ) / 4]);
		v15 = v19 ^ MTPZ_SWAP(u_expanded[(keyOffset +  4) / 4]);
		v16 = v20 ^ MTPZ_SWAP(u_expanded[(keyOffset +  8) / 4]);
		v17 = v21 ^ MTPZ_SWAP(u_expanded[(keyOffset + 12) / 4]);

		v18 = mtpz_aes_rt1[MTPZ_ENCRYPTIONBYTE3(v15)] ^ mtpz_aes_rt2[MTPZ_ENCRYPTIONBYTE2(v16)] ^ mtpz_aes_rt3[MTPZ_ENCRYPTIONLOBYTE(v14)] ^ mtpz_aes_rt4[MTPZ_ENCRYPTIONBYTE1(v17)];
		v19 = mtpz_aes_rt1[MTPZ_ENCRYPTIONBYTE3(v16)] ^ mtpz_aes_rt2[MTPZ_ENCRYPTIONBYTE2(v17)] ^ mtpz_aes_rt3[MTPZ_ENCRYPTIONLOBYTE(v15)] ^ mtpz_aes_rt4[MTPZ_ENCRYPTIONBYTE1(v14)];
		v20 = mtpz_aes_rt1[MTPZ_ENCRYPTIONBYTE3(v17)] ^ mtpz_aes_rt2[MTPZ_ENCRYPTIONBYTE2(v14)] ^ mtpz_aes_rt3[MTPZ_ENCRYPTIONLOBYTE(v16)] ^ mtpz_aes_rt4[MTPZ_ENCRYPTIONBYTE1(v15)];
		v21 = mtpz_aes_rt1[MTPZ_ENCRYPTIONBYTE3(v14)] ^ mtpz_aes_rt2[MTPZ_ENCRYPTIONBYTE2(v15)] ^ mtpz_aes_rt3[MTPZ_ENCRYPTIONLOBYTE(v17)] ^ mtpz_aes_rt4[MTPZ_ENCRYPTIONBYTE1(v16)];

		rounds--;
		keyOffset -= 16;
	}
	while (rounds != 1);

	v14 = v18 ^ MTPZ_SWAP(u_expanded[(keyOffset     ) / 4]);
	v15 = v19 ^ MTPZ_SWAP(u_expanded[(keyOffset +  4) / 4]);
	v16 = v20 ^ MTPZ_SWAP(u_expanded[(keyOffset +  8) / 4]);
	v17 = v21 ^ MTPZ_SWAP(u_expanded[(keyOffset + 12) / 4]);
	keyOffset -= 16;

	v18 = ((mtpz_aes_invsbox[MTPZ_ENCRYPTIONLOBYTE(v14)]) << 24) |
		  ((mtpz_aes_invsbox[MTPZ_ENCRYPTIONBYTE1 (v17)]) << 16) |
		  ((mtpz_aes_invsbox[MTPZ_ENCRYPTIONBYTE2 (v16)]) <<  8) |
		  ((mtpz_aes_invsbox[MTPZ_ENCRYPTIONBYTE3 (v15)]) <<  0);

	v19 = ((mtpz_aes_invsbox[MTPZ_ENCRYPTIONLOBYTE(v15)]) << 24) |
		  ((mtpz_aes_invsbox[MTPZ_ENCRYPTIONBYTE1 (v14)]) << 16) |
		  ((mtpz_aes_invsbox[MTPZ_ENCRYPTIONBYTE2 (v17)]) <<  8) |
		  ((mtpz_aes_invsbox[MTPZ_ENCRYPTIONBYTE3 (v16)]) <<  0);

	v20 = ((mtpz_aes_invsbox[MTPZ_ENCRYPTIONLOBYTE(v16)]) << 24) |
		  ((mtpz_aes_invsbox[MTPZ_ENCRYPTIONBYTE1 (v15)]) << 16) |
		  ((mtpz_aes_invsbox[MTPZ_ENCRYPTIONBYTE2 (v14)]) <<  8) |
		  ((mtpz_aes_invsbox[MTPZ_ENCRYPTIONBYTE3 (v17)]) <<  0);

	v21 = ((mtpz_aes_invsbox[MTPZ_ENCRYPTIONLOBYTE(v17)]) << 24) |
		  ((mtpz_aes_invsbox[MTPZ_ENCRYPTIONBYTE1 (v16)]) << 16) |
		  ((mtpz_aes_invsbox[MTPZ_ENCRYPTIONBYTE2 (v15)]) <<  8) |
		  ((mtpz_aes_invsbox[MTPZ_ENCRYPTIONBYTE3 (v14)]) <<  0);

	u_data[0] = MTPZ_SWAP(v18 ^ MTPZ_SWAP(u_expanded[(keyOffset     ) / 4]));
	u_data[1] = MTPZ_SWAP(v19 ^ MTPZ_SWAP(u_expanded[(keyOffset +  4) / 4]));
	u_data[2] = MTPZ_SWAP(v20 ^ MTPZ_SWAP(u_expanded[(keyOffset +  8) / 4]));
	u_data[3] = MTPZ_SWAP(v21 ^ MTPZ_SWAP(u_expanded[(keyOffset + 12) / 4]));
};

void mtpz_encryption_encrypt_custom(unsigned char *data, unsigned char *seed, unsigned char *expanded)
{
	unsigned int *u_data = (unsigned int *)data;
	unsigned int *u_expanded = (unsigned int *)expanded;
	int keyOffset = 0x04;

	unsigned int *u_seed;

	if (seed == NULL)
		u_seed = u_data;
	else
		u_seed = (unsigned int *)seed;

	unsigned int v14 = MTPZ_SWAP(u_seed[0]) ^ MTPZ_SWAP(u_expanded[(keyOffset     ) / 4]);
	unsigned int v15 = MTPZ_SWAP(u_seed[1]) ^ MTPZ_SWAP(u_expanded[(keyOffset +  4) / 4]);
	unsigned int v16 = MTPZ_SWAP(u_seed[2]) ^ MTPZ_SWAP(u_expanded[(keyOffset +  8) / 4]);
	unsigned int v17 = MTPZ_SWAP(u_seed[3]) ^ MTPZ_SWAP(u_expanded[(keyOffset + 12) / 4]);

	unsigned int v18 = mtpz_aes_ft1[MTPZ_ENCRYPTIONBYTE3(v17)] ^ mtpz_aes_ft2[MTPZ_ENCRYPTIONBYTE2(v16)] ^ mtpz_aes_ft3[MTPZ_ENCRYPTIONLOBYTE(v14)] ^ mtpz_aes_ft4[MTPZ_ENCRYPTIONBYTE1(v15)];
	unsigned int v19 = mtpz_aes_ft1[MTPZ_ENCRYPTIONBYTE3(v14)] ^ mtpz_aes_ft2[MTPZ_ENCRYPTIONBYTE2(v17)] ^ mtpz_aes_ft3[MTPZ_ENCRYPTIONLOBYTE(v15)] ^ mtpz_aes_ft4[MTPZ_ENCRYPTIONBYTE1(v16)];
	unsigned int v20 = mtpz_aes_ft1[MTPZ_ENCRYPTIONBYTE3(v15)] ^ mtpz_aes_ft2[MTPZ_ENCRYPTIONBYTE2(v14)] ^ mtpz_aes_ft3[MTPZ_ENCRYPTIONLOBYTE(v16)] ^ mtpz_aes_ft4[MTPZ_ENCRYPTIONBYTE1(v17)];
	unsigned int v21 = mtpz_aes_ft1[MTPZ_ENCRYPTIONBYTE3(v16)] ^ mtpz_aes_ft2[MTPZ_ENCRYPTIONBYTE2(v15)] ^ mtpz_aes_ft3[MTPZ_ENCRYPTIONLOBYTE(v17)] ^ mtpz_aes_ft4[MTPZ_ENCRYPTIONBYTE1(v14)];

	keyOffset += 16;
	int rounds = 1;

	do
	{

		v14 = v18 ^ MTPZ_SWAP(u_expanded[(keyOffset     ) / 4]);
		v15 = v19 ^ MTPZ_SWAP(u_expanded[(keyOffset +  4) / 4]);
		v16 = v20 ^ MTPZ_SWAP(u_expanded[(keyOffset +  8) / 4]);
		v17 = v21 ^ MTPZ_SWAP(u_expanded[(keyOffset + 12) / 4]);

		v18 = mtpz_aes_ft1[MTPZ_ENCRYPTIONBYTE3(v17)] ^ mtpz_aes_ft2[MTPZ_ENCRYPTIONBYTE2(v16)] ^ mtpz_aes_ft3[MTPZ_ENCRYPTIONLOBYTE(v14)] ^ mtpz_aes_ft4[MTPZ_ENCRYPTIONBYTE1(v15)];
		v19 = mtpz_aes_ft1[MTPZ_ENCRYPTIONBYTE3(v14)] ^ mtpz_aes_ft2[MTPZ_ENCRYPTIONBYTE2(v17)] ^ mtpz_aes_ft3[MTPZ_ENCRYPTIONLOBYTE(v15)] ^ mtpz_aes_ft4[MTPZ_ENCRYPTIONBYTE1(v16)];
		v20 = mtpz_aes_ft1[MTPZ_ENCRYPTIONBYTE3(v15)] ^ mtpz_aes_ft2[MTPZ_ENCRYPTIONBYTE2(v14)] ^ mtpz_aes_ft3[MTPZ_ENCRYPTIONLOBYTE(v16)] ^ mtpz_aes_ft4[MTPZ_ENCRYPTIONBYTE1(v17)];
		v21 = mtpz_aes_ft1[MTPZ_ENCRYPTIONBYTE3(v16)] ^ mtpz_aes_ft2[MTPZ_ENCRYPTIONBYTE2(v15)] ^ mtpz_aes_ft3[MTPZ_ENCRYPTIONLOBYTE(v17)] ^ mtpz_aes_ft4[MTPZ_ENCRYPTIONBYTE1(v14)];

		rounds++;
		keyOffset += 16;
	}
	while (rounds != 9);

	v14 = v18 ^ MTPZ_SWAP(u_expanded[(keyOffset     ) / 4]);
	v15 = v19 ^ MTPZ_SWAP(u_expanded[(keyOffset +  4) / 4]);
	v16 = v20 ^ MTPZ_SWAP(u_expanded[(keyOffset +  8) / 4]);
	v17 = v21 ^ MTPZ_SWAP(u_expanded[(keyOffset + 12) / 4]);
	keyOffset += 16;

	unsigned char *FT3_Bytes = (unsigned char *)mtpz_aes_ft3;

	v18 = ((FT3_Bytes[1 + 4 * MTPZ_ENCRYPTIONLOBYTE(v14)]) << 24) |
		  ((FT3_Bytes[1 + 4 * MTPZ_ENCRYPTIONBYTE1 (v15)]) << 16) |
		  ((FT3_Bytes[1 + 4 * MTPZ_ENCRYPTIONBYTE2 (v16)]) <<  8) |
		  ((FT3_Bytes[1 + 4 * MTPZ_ENCRYPTIONBYTE3 (v17)]) <<  0);

	v19 = ((FT3_Bytes[1 + 4 * MTPZ_ENCRYPTIONLOBYTE(v15)]) << 24) |
		  ((FT3_Bytes[1 + 4 * MTPZ_ENCRYPTIONBYTE1 (v16)]) << 16) |
		  ((FT3_Bytes[1 + 4 * MTPZ_ENCRYPTIONBYTE2 (v17)]) <<  8) |
		  ((FT3_Bytes[1 + 4 * MTPZ_ENCRYPTIONBYTE3 (v14)]) <<  0);

	v20 = ((FT3_Bytes[1 + 4 * MTPZ_ENCRYPTIONLOBYTE(v16)]) << 24) |
		  ((FT3_Bytes[1 + 4 * MTPZ_ENCRYPTIONBYTE1 (v17)]) << 16) |
		  ((FT3_Bytes[1 + 4 * MTPZ_ENCRYPTIONBYTE2 (v14)]) <<  8) |
		  ((FT3_Bytes[1 + 4 * MTPZ_ENCRYPTIONBYTE3 (v15)]) <<  0);

	v21 = ((FT3_Bytes[1 + 4 * MTPZ_ENCRYPTIONLOBYTE(v17)]) << 24) |
		  ((FT3_Bytes[1 + 4 * MTPZ_ENCRYPTIONBYTE1 (v14)]) << 16) |
		  ((FT3_Bytes[1 + 4 * MTPZ_ENCRYPTIONBYTE2 (v15)]) <<  8) |
		  ((FT3_Bytes[1 + 4 * MTPZ_ENCRYPTIONBYTE3 (v16)]) <<  0);

	u_data[0] = MTPZ_SWAP(v18 ^ MTPZ_SWAP(u_expanded[(keyOffset     ) / 4]));
	u_data[1] = MTPZ_SWAP(v19 ^ MTPZ_SWAP(u_expanded[(keyOffset +  4) / 4]));
	u_data[2] = MTPZ_SWAP(v20 ^ MTPZ_SWAP(u_expanded[(keyOffset +  8) / 4]));
	u_data[3] = MTPZ_SWAP(v21 ^ MTPZ_SWAP(u_expanded[(keyOffset + 12) / 4]));
}

void mtpz_encryption_encrypt_mac(unsigned char *hash, unsigned int hash_length, unsigned char *seed, unsigned int seed_len, unsigned char *out)
{
	if (hash == NULL || hash_length != 16)
		return;

	unsigned char *loop1 = (unsigned char *)malloc(17);
	memset(loop1, 0, 17);
	unsigned char *loop2 = (unsigned char *)malloc(17);
	memset(loop2, 0, 17);
	int i = 0;

	{
		unsigned char *enc_hash = (unsigned char *)malloc(17);
		memset(enc_hash, 0, 17);
		mtpz_encryption_cipher_advanced(hash, hash_length, enc_hash, 16, 1);

		for (i = 0; i < 16; i++)
			loop1[i] = (unsigned char)((2 * enc_hash[i]) | (enc_hash[i + 1] >> 7));

		if (enc_hash[0] >= (unsigned char)128)
			loop1[15] ^= (unsigned char)0x87;

		for (i = 0; i < 16; i++)
			loop2[i] = (unsigned char)((2 * loop1[i]) | (loop1[i + 1] >> 7));

		if (loop1[0] >= (unsigned char)128)
			loop2[15] ^= (unsigned char)0x87;

		free(enc_hash);
	}

	{
		int len = 	(hash_length == 16) ? 10 :
					(hash_length == 24) ? 12 : 32;
		int exp_len;
		unsigned char *expanded = mtpz_encryption_expand_key(hash, hash_length, len, &exp_len);

		unsigned char *actual_seed = (unsigned char *)malloc(16);
		memset(actual_seed, 0, 16);

		unsigned int i = 0;

		if (seed_len == 16)
		{
			for (i = 0; i < 16; i++)
				actual_seed[i] ^= seed[i];

			for (i = 0; i < 16; i++)
				actual_seed[i] ^= loop1[i];
		}
		else
		{
			for (i = 0; i < seed_len; i++)
				actual_seed[i] ^= seed[i];

			actual_seed[seed_len] = (unsigned char)128;

			for (i = 0; i < 16; i++)
				actual_seed[i] ^= loop2[i];
		}

		mtpz_encryption_encrypt_custom(out, actual_seed, expanded);

		free(expanded);
		free(actual_seed);
	}

	free(loop1);
	free(loop2);
}


/* ENCRYPTION CONSTANTS */
/*
 * These tables can also be found in Mozilla's Network Security Services:
 *     http://www.mozilla.org/projects/security/pki/nss/
 *
 * <rijndael32.tab>:
 *     https://hg.mozilla.org/mozilla-central/raw-file/90828ac18dcf/security/nss/lib/freebl/rijndael32.tab
 *
 * Each of the following constant tables will also identify the corresponding
 * table in the <rijndael32.tab> link.
 */

/* Corresponds to Rcon[30] (seems to be truncated to include only the used constants) */
unsigned char mtpz_aes_rcon[] =
{
	0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1b, 0x36, 0x6c, 0xd8, 0xab, 0x4d, 0x9a
};

/* Corresponds to _S[256] (in hex) */
unsigned char mtpz_aes_sbox[] =
{
	0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01,
	0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76, 0xca, 0x82, 0xc9, 0x7d,
	0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4,
	0x72, 0xc0, 0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc,
	0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15, 0x04, 0xc7,
	0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2,
	0xeb, 0x27, 0xb2, 0x75, 0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e,
	0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
	0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb,
	0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf, 0xd0, 0xef, 0xaa, 0xfb,
	0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c,
	0x9f, 0xa8, 0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5,
	0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2, 0xcd, 0x0c,
	0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d,
	0x64, 0x5d, 0x19, 0x73, 0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a,
	0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
	0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3,
	0xac, 0x62, 0x91, 0x95, 0xe4, 0x79, 0xe7, 0xc8, 0x37, 0x6d,
	0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a,
	0xae, 0x08, 0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6,
	0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a, 0x70, 0x3e,
	0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9,
	0x86, 0xc1, 0x1d, 0x9e, 0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9,
	0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
	0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99,
	0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16
};

/* Corresponds to _SInv[256] (in hex) */
unsigned char mtpz_aes_invsbox[] =
{
	0x52, 0x09, 0x6A, 0xD5, 0x30, 0x36, 0xA5, 0x38,  0xBF, 0x40, 0xA3, 0x9E, 0x81, 0xF3, 0xD7, 0xFB,
	0x7C, 0xE3, 0x39, 0x82, 0x9B, 0x2F, 0xFF, 0x87,  0x34, 0x8E, 0x43, 0x44, 0xC4, 0xDE, 0xE9, 0xCB,
	0x54, 0x7B, 0x94, 0x32, 0xA6, 0xC2, 0x23, 0x3D,  0xEE, 0x4C, 0x95, 0x0B, 0x42, 0xFA, 0xC3, 0x4E,
	0x08, 0x2E, 0xA1, 0x66, 0x28, 0xD9, 0x24, 0xB2,  0x76, 0x5B, 0xA2, 0x49, 0x6D, 0x8B, 0xD1, 0x25,
	0x72, 0xF8, 0xF6, 0x64, 0x86, 0x68, 0x98, 0x16,  0xD4, 0xA4, 0x5C, 0xCC, 0x5D, 0x65, 0xB6, 0x92,
	0x6C, 0x70, 0x48, 0x50, 0xFD, 0xED, 0xB9, 0xDA,  0x5E, 0x15, 0x46, 0x57, 0xA7, 0x8D, 0x9D, 0x84,
	0x90, 0xD8, 0xAB, 0x00, 0x8C, 0xBC, 0xD3, 0x0A,  0xF7, 0xE4, 0x58, 0x05, 0xB8, 0xB3, 0x45, 0x06,
	0xD0, 0x2C, 0x1E, 0x8F, 0xCA, 0x3F, 0x0F, 0x02,  0xC1, 0xAF, 0xBD, 0x03, 0x01, 0x13, 0x8A, 0x6B,
	0x3A, 0x91, 0x11, 0x41, 0x4F, 0x67, 0xDC, 0xEA,  0x97, 0xF2, 0xCF, 0xCE, 0xF0, 0xB4, 0xE6, 0x73,
	0x96, 0xAC, 0x74, 0x22, 0xE7, 0xAD, 0x35, 0x85,  0xE2, 0xF9, 0x37, 0xE8, 0x1C, 0x75, 0xDF, 0x6E,
	0x47, 0xF1, 0x1A, 0x71, 0x1D, 0x29, 0xC5, 0x89,  0x6F, 0xB7, 0x62, 0x0E, 0xAA, 0x18, 0xBE, 0x1B,
	0xFC, 0x56, 0x3E, 0x4B, 0xC6, 0xD2, 0x79, 0x20,  0x9A, 0xDB, 0xC0, 0xFE, 0x78, 0xCD, 0x5A, 0xF4,
	0x1F, 0xDD, 0xA8, 0x33, 0x88, 0x07, 0xC7, 0x31,  0xB1, 0x12, 0x10, 0x59, 0x27, 0x80, 0xEC, 0x5F,
	0x60, 0x51, 0x7F, 0xA9, 0x19, 0xB5, 0x4A, 0x0D,  0x2D, 0xE5, 0x7A, 0x9F, 0x93, 0xC9, 0x9C, 0xEF,
	0xA0, 0xE0, 0x3B, 0x4D, 0xAE, 0x2A, 0xF5, 0xB0,  0xC8, 0xEB, 0xBB, 0x3C, 0x83, 0x53, 0x99, 0x61,
	0x17, 0x2B, 0x04, 0x7E, 0xBA, 0x77, 0xD6, 0x26,  0xE1, 0x69, 0x14, 0x63, 0x55, 0x21, 0x0C, 0x7D
};

/* Corresponds to _T3[256] */
unsigned int mtpz_aes_ft1[] =
{
	0x6363A5C6, 0x7C7C84F8,  0x777799EE, 0x7B7B8DF6, 0xF2F20DFF, 0x6B6BBDD6,  0x6F6FB1DE, 0xC5C55491,
	0x30305060, 0x01010302,  0x6767A9CE, 0x2B2B7D56, 0xFEFE19E7, 0xD7D762B5,  0xABABE64D, 0x76769AEC,
	0xCACA458F, 0x82829D1F,  0xC9C94089, 0x7D7D87FA, 0xFAFA15EF, 0x5959EBB2,  0x4747C98E, 0xF0F00BFB,
	0xADADEC41, 0xD4D467B3,  0xA2A2FD5F, 0xAFAFEA45, 0x9C9CBF23, 0xA4A4F753,  0x727296E4, 0xC0C05B9B,
	0xB7B7C275, 0xFDFD1CE1,  0x9393AE3D, 0x26266A4C, 0x36365A6C, 0x3F3F417E,  0xF7F702F5, 0xCCCC4F83,
	0x34345C68, 0xA5A5F451,  0xE5E534D1, 0xF1F108F9, 0x717193E2, 0xD8D873AB,  0x31315362, 0x15153F2A,
	0x04040C08, 0xC7C75295,  0x23236546, 0xC3C35E9D, 0x18182830, 0x9696A137,  0x05050F0A, 0x9A9AB52F,
	0x0707090E, 0x12123624,  0x80809B1B, 0xE2E23DDF, 0xEBEB26CD, 0x2727694E,  0xB2B2CD7F, 0x75759FEA,
	0x09091B12, 0x83839E1D,  0x2C2C7458, 0x1A1A2E34, 0x1B1B2D36, 0x6E6EB2DC,  0x5A5AEEB4, 0xA0A0FB5B,
	0x5252F6A4, 0x3B3B4D76,  0xD6D661B7, 0xB3B3CE7D, 0x29297B52, 0xE3E33EDD,  0x2F2F715E, 0x84849713,
	0x5353F5A6, 0xD1D168B9,  0x00000000, 0xEDED2CC1, 0x20206040, 0xFCFC1FE3,  0xB1B1C879, 0x5B5BEDB6,
	0x6A6ABED4, 0xCBCB468D,  0xBEBED967, 0x39394B72, 0x4A4ADE94, 0x4C4CD498,  0x5858E8B0, 0xCFCF4A85,
	0xD0D06BBB, 0xEFEF2AC5,  0xAAAAE54F, 0xFBFB16ED, 0x4343C586, 0x4D4DD79A,  0x33335566, 0x85859411,
	0x4545CF8A, 0xF9F910E9,  0x02020604, 0x7F7F81FE, 0x5050F0A0, 0x3C3C4478,  0x9F9FBA25, 0xA8A8E34B,
	0x5151F3A2, 0xA3A3FE5D,  0x4040C080, 0x8F8F8A05, 0x9292AD3F, 0x9D9DBC21,  0x38384870, 0xF5F504F1,
	0xBCBCDF63, 0xB6B6C177,  0xDADA75AF, 0x21216342, 0x10103020, 0xFFFF1AE5,  0xF3F30EFD, 0xD2D26DBF,
	0xCDCD4C81, 0x0C0C1418,  0x13133526, 0xECEC2FC3, 0x5F5FE1BE, 0x9797A235,  0x4444CC88, 0x1717392E,
	0xC4C45793, 0xA7A7F255,  0x7E7E82FC, 0x3D3D477A, 0x6464ACC8, 0x5D5DE7BA,  0x19192B32, 0x737395E6,
	0x6060A0C0, 0x81819819,  0x4F4FD19E, 0xDCDC7FA3, 0x22226644, 0x2A2A7E54,  0x9090AB3B, 0x8888830B,
	0x4646CA8C, 0xEEEE29C7,  0xB8B8D36B, 0x14143C28, 0xDEDE79A7, 0x5E5EE2BC,  0x0B0B1D16, 0xDBDB76AD,
	0xE0E03BDB, 0x32325664,  0x3A3A4E74, 0x0A0A1E14, 0x4949DB92, 0x06060A0C,  0x24246C48, 0x5C5CE4B8,
	0xC2C25D9F, 0xD3D36EBD,  0xACACEF43, 0x6262A6C4, 0x9191A839, 0x9595A431,  0xE4E437D3, 0x79798BF2,
	0xE7E732D5, 0xC8C8438B,  0x3737596E, 0x6D6DB7DA, 0x8D8D8C01, 0xD5D564B1,  0x4E4ED29C, 0xA9A9E049,
	0x6C6CB4D8, 0x5656FAAC,  0xF4F407F3, 0xEAEA25CF, 0x6565AFCA, 0x7A7A8EF4,  0xAEAEE947, 0x08081810,
	0xBABAD56F, 0x787888F0,  0x25256F4A, 0x2E2E725C, 0x1C1C2438, 0xA6A6F157,  0xB4B4C773, 0xC6C65197,
	0xE8E823CB, 0xDDDD7CA1,  0x74749CE8, 0x1F1F213E, 0x4B4BDD96, 0xBDBDDC61,  0x8B8B860D, 0x8A8A850F,
	0x707090E0, 0x3E3E427C,  0xB5B5C471, 0x6666AACC, 0x4848D890, 0x03030506,  0xF6F601F7, 0x0E0E121C,
	0x6161A3C2, 0x35355F6A,  0x5757F9AE, 0xB9B9D069, 0x86869117, 0xC1C15899,  0x1D1D273A, 0x9E9EB927,
	0xE1E138D9, 0xF8F813EB,  0x9898B32B, 0x11113322, 0x6969BBD2, 0xD9D970A9,  0x8E8E8907, 0x9494A733,
	0x9B9BB62D, 0x1E1E223C,  0x87879215, 0xE9E920C9, 0xCECE4987, 0x5555FFAA,  0x28287850, 0xDFDF7AA5,
	0x8C8C8F03, 0xA1A1F859,  0x89898009, 0x0D0D171A, 0xBFBFDA65, 0xE6E631D7,  0x4242C684, 0x6868B8D0,
	0x4141C382, 0x9999B029,  0x2D2D775A, 0x0F0F111E, 0xB0B0CB7B, 0x5454FCA8,  0xBBBBD66D, 0x16163A2C,
};

/* Corresponds to _T2[256] */
unsigned int mtpz_aes_ft2[] =
{
	0x63A5C663, 0x7C84F87C,  0x7799EE77, 0x7B8DF67B, 0xF20DFFF2, 0x6BBDD66B,  0x6FB1DE6F, 0xC55491C5,
	0x30506030, 0x01030201,  0x67A9CE67, 0x2B7D562B, 0xFE19E7FE, 0xD762B5D7,  0xABE64DAB, 0x769AEC76,
	0xCA458FCA, 0x829D1F82,  0xC94089C9, 0x7D87FA7D, 0xFA15EFFA, 0x59EBB259,  0x47C98E47, 0xF00BFBF0,
	0xADEC41AD, 0xD467B3D4,  0xA2FD5FA2, 0xAFEA45AF, 0x9CBF239C, 0xA4F753A4,  0x7296E472, 0xC05B9BC0,
	0xB7C275B7, 0xFD1CE1FD,  0x93AE3D93, 0x266A4C26, 0x365A6C36, 0x3F417E3F,  0xF702F5F7, 0xCC4F83CC,
	0x345C6834, 0xA5F451A5,  0xE534D1E5, 0xF108F9F1, 0x7193E271, 0xD873ABD8,  0x31536231, 0x153F2A15,
	0x040C0804, 0xC75295C7,  0x23654623, 0xC35E9DC3, 0x18283018, 0x96A13796,  0x050F0A05, 0x9AB52F9A,
	0x07090E07, 0x12362412,  0x809B1B80, 0xE23DDFE2, 0xEB26CDEB, 0x27694E27,  0xB2CD7FB2, 0x759FEA75,
	0x091B1209, 0x839E1D83,  0x2C74582C, 0x1A2E341A, 0x1B2D361B, 0x6EB2DC6E,  0x5AEEB45A, 0xA0FB5BA0,
	0x52F6A452, 0x3B4D763B,  0xD661B7D6, 0xB3CE7DB3, 0x297B5229, 0xE33EDDE3,  0x2F715E2F, 0x84971384,
	0x53F5A653, 0xD168B9D1,  0x00000000, 0xED2CC1ED, 0x20604020, 0xFC1FE3FC,  0xB1C879B1, 0x5BEDB65B,
	0x6ABED46A, 0xCB468DCB,  0xBED967BE, 0x394B7239, 0x4ADE944A, 0x4CD4984C,  0x58E8B058, 0xCF4A85CF,
	0xD06BBBD0, 0xEF2AC5EF,  0xAAE54FAA, 0xFB16EDFB, 0x43C58643, 0x4DD79A4D,  0x33556633, 0x85941185,
	0x45CF8A45, 0xF910E9F9,  0x02060402, 0x7F81FE7F, 0x50F0A050, 0x3C44783C,  0x9FBA259F, 0xA8E34BA8,
	0x51F3A251, 0xA3FE5DA3,  0x40C08040, 0x8F8A058F, 0x92AD3F92, 0x9DBC219D,  0x38487038, 0xF504F1F5,
	0xBCDF63BC, 0xB6C177B6,  0xDA75AFDA, 0x21634221, 0x10302010, 0xFF1AE5FF,  0xF30EFDF3, 0xD26DBFD2,
	0xCD4C81CD, 0x0C14180C,  0x13352613, 0xEC2FC3EC, 0x5FE1BE5F, 0x97A23597,  0x44CC8844, 0x17392E17,
	0xC45793C4, 0xA7F255A7,  0x7E82FC7E, 0x3D477A3D, 0x64ACC864, 0x5DE7BA5D,  0x192B3219, 0x7395E673,
	0x60A0C060, 0x81981981,  0x4FD19E4F, 0xDC7FA3DC, 0x22664422, 0x2A7E542A,  0x90AB3B90, 0x88830B88,
	0x46CA8C46, 0xEE29C7EE,  0xB8D36BB8, 0x143C2814, 0xDE79A7DE, 0x5EE2BC5E,  0x0B1D160B, 0xDB76ADDB,
	0xE03BDBE0, 0x32566432,  0x3A4E743A, 0x0A1E140A, 0x49DB9249, 0x060A0C06,  0x246C4824, 0x5CE4B85C,
	0xC25D9FC2, 0xD36EBDD3,  0xACEF43AC, 0x62A6C462, 0x91A83991, 0x95A43195,  0xE437D3E4, 0x798BF279,
	0xE732D5E7, 0xC8438BC8,  0x37596E37, 0x6DB7DA6D, 0x8D8C018D, 0xD564B1D5,  0x4ED29C4E, 0xA9E049A9,
	0x6CB4D86C, 0x56FAAC56,  0xF407F3F4, 0xEA25CFEA, 0x65AFCA65, 0x7A8EF47A,  0xAEE947AE, 0x08181008,
	0xBAD56FBA, 0x7888F078,  0x256F4A25, 0x2E725C2E, 0x1C24381C, 0xA6F157A6,  0xB4C773B4, 0xC65197C6,
	0xE823CBE8, 0xDD7CA1DD,  0x749CE874, 0x1F213E1F, 0x4BDD964B, 0xBDDC61BD,  0x8B860D8B, 0x8A850F8A,
	0x7090E070, 0x3E427C3E,  0xB5C471B5, 0x66AACC66, 0x48D89048, 0x03050603,  0xF601F7F6, 0x0E121C0E,
	0x61A3C261, 0x355F6A35,  0x57F9AE57, 0xB9D069B9, 0x86911786, 0xC15899C1,  0x1D273A1D, 0x9EB9279E,
	0xE138D9E1, 0xF813EBF8,  0x98B32B98, 0x11332211, 0x69BBD269, 0xD970A9D9,  0x8E89078E, 0x94A73394,
	0x9BB62D9B, 0x1E223C1E,  0x87921587, 0xE920C9E9, 0xCE4987CE, 0x55FFAA55,  0x28785028, 0xDF7AA5DF,
	0x8C8F038C, 0xA1F859A1,  0x89800989, 0x0D171A0D, 0xBFDA65BF, 0xE631D7E6,  0x42C68442, 0x68B8D068,
	0x41C38241, 0x99B02999,  0x2D775A2D, 0x0F111E0F, 0xB0CB7BB0, 0x54FCA854,  0xBBD66DBB, 0x163A2C16,
};

/* Corresponds to _T0[256] */
unsigned int mtpz_aes_ft3[] =
{
	0xC66363A5, 0xF87C7C84,  0xEE777799, 0xF67B7B8D, 0xFFF2F20D, 0xD66B6BBD,  0xDE6F6FB1, 0x91C5C554,
	0x60303050, 0x02010103,  0xCE6767A9, 0x562B2B7D, 0xE7FEFE19, 0xB5D7D762,  0x4DABABE6, 0xEC76769A,
	0x8FCACA45, 0x1F82829D,  0x89C9C940, 0xFA7D7D87, 0xEFFAFA15, 0xB25959EB,  0x8E4747C9, 0xFBF0F00B,
	0x41ADADEC, 0xB3D4D467,  0x5FA2A2FD, 0x45AFAFEA, 0x239C9CBF, 0x53A4A4F7,  0xE4727296, 0x9BC0C05B,
	0x75B7B7C2, 0xE1FDFD1C,  0x3D9393AE, 0x4C26266A, 0x6C36365A, 0x7E3F3F41,  0xF5F7F702, 0x83CCCC4F,
	0x6834345C, 0x51A5A5F4,  0xD1E5E534, 0xF9F1F108, 0xE2717193, 0xABD8D873,  0x62313153, 0x2A15153F,
	0x0804040C, 0x95C7C752,  0x46232365, 0x9DC3C35E, 0x30181828, 0x379696A1,  0x0A05050F, 0x2F9A9AB5,
	0x0E070709, 0x24121236,  0x1B80809B, 0xDFE2E23D, 0xCDEBEB26, 0x4E272769,  0x7FB2B2CD, 0xEA75759F,
	0x1209091B, 0x1D83839E,  0x582C2C74, 0x341A1A2E, 0x361B1B2D, 0xDC6E6EB2,  0xB45A5AEE, 0x5BA0A0FB,
	0xA45252F6, 0x763B3B4D,  0xB7D6D661, 0x7DB3B3CE, 0x5229297B, 0xDDE3E33E,  0x5E2F2F71, 0x13848497,
	0xA65353F5, 0xB9D1D168,  0x00000000, 0xC1EDED2C, 0x40202060, 0xE3FCFC1F,  0x79B1B1C8, 0xB65B5BED,
	0xD46A6ABE, 0x8DCBCB46,  0x67BEBED9, 0x7239394B, 0x944A4ADE, 0x984C4CD4,  0xB05858E8, 0x85CFCF4A,
	0xBBD0D06B, 0xC5EFEF2A,  0x4FAAAAE5, 0xEDFBFB16, 0x864343C5, 0x9A4D4DD7,  0x66333355, 0x11858594,
	0x8A4545CF, 0xE9F9F910,  0x04020206, 0xFE7F7F81, 0xA05050F0, 0x783C3C44,  0x259F9FBA, 0x4BA8A8E3,
	0xA25151F3, 0x5DA3A3FE,  0x804040C0, 0x058F8F8A, 0x3F9292AD, 0x219D9DBC,  0x70383848, 0xF1F5F504,
	0x63BCBCDF, 0x77B6B6C1,  0xAFDADA75, 0x42212163, 0x20101030, 0xE5FFFF1A,  0xFDF3F30E, 0xBFD2D26D,
	0x81CDCD4C, 0x180C0C14,  0x26131335, 0xC3ECEC2F, 0xBE5F5FE1, 0x359797A2,  0x884444CC, 0x2E171739,
	0x93C4C457, 0x55A7A7F2,  0xFC7E7E82, 0x7A3D3D47, 0xC86464AC, 0xBA5D5DE7,  0x3219192B, 0xE6737395,
	0xC06060A0, 0x19818198,  0x9E4F4FD1, 0xA3DCDC7F, 0x44222266, 0x542A2A7E,  0x3B9090AB, 0x0B888883,
	0x8C4646CA, 0xC7EEEE29,  0x6BB8B8D3, 0x2814143C, 0xA7DEDE79, 0xBC5E5EE2,  0x160B0B1D, 0xADDBDB76,
	0xDBE0E03B, 0x64323256,  0x743A3A4E, 0x140A0A1E, 0x924949DB, 0x0C06060A,  0x4824246C, 0xB85C5CE4,
	0x9FC2C25D, 0xBDD3D36E,  0x43ACACEF, 0xC46262A6, 0x399191A8, 0x319595A4,  0xD3E4E437, 0xF279798B,
	0xD5E7E732, 0x8BC8C843,  0x6E373759, 0xDA6D6DB7, 0x018D8D8C, 0xB1D5D564,  0x9C4E4ED2, 0x49A9A9E0,
	0xD86C6CB4, 0xAC5656FA,  0xF3F4F407, 0xCFEAEA25, 0xCA6565AF, 0xF47A7A8E,  0x47AEAEE9, 0x10080818,
	0x6FBABAD5, 0xF0787888,  0x4A25256F, 0x5C2E2E72, 0x381C1C24, 0x57A6A6F1,  0x73B4B4C7, 0x97C6C651,
	0xCBE8E823, 0xA1DDDD7C,  0xE874749C, 0x3E1F1F21, 0x964B4BDD, 0x61BDBDDC,  0x0D8B8B86, 0x0F8A8A85,
	0xE0707090, 0x7C3E3E42,  0x71B5B5C4, 0xCC6666AA, 0x904848D8, 0x06030305,  0xF7F6F601, 0x1C0E0E12,
	0xC26161A3, 0x6A35355F,  0xAE5757F9, 0x69B9B9D0, 0x17868691, 0x99C1C158,  0x3A1D1D27, 0x279E9EB9,
	0xD9E1E138, 0xEBF8F813,  0x2B9898B3, 0x22111133, 0xD26969BB, 0xA9D9D970,  0x078E8E89, 0x339494A7,
	0x2D9B9BB6, 0x3C1E1E22,  0x15878792, 0xC9E9E920, 0x87CECE49, 0xAA5555FF,  0x50282878, 0xA5DFDF7A,
	0x038C8C8F, 0x59A1A1F8,  0x09898980, 0x1A0D0D17, 0x65BFBFDA, 0xD7E6E631,  0x844242C6, 0xD06868B8,
	0x824141C3, 0x299999B0,  0x5A2D2D77, 0x1E0F0F11, 0x7BB0B0CB, 0xA85454FC,  0x6DBBBBD6, 0x2C16163A,
};

/* Corresponds to _T1[256] */
unsigned int mtpz_aes_ft4[] =
{
	0xA5C66363, 0x84F87C7C,  0x99EE7777, 0x8DF67B7B, 0x0DFFF2F2, 0xBDD66B6B,  0xB1DE6F6F, 0x5491C5C5,
	0x50603030, 0x03020101,  0xA9CE6767, 0x7D562B2B, 0x19E7FEFE, 0x62B5D7D7,  0xE64DABAB, 0x9AEC7676,
	0x458FCACA, 0x9D1F8282,  0x4089C9C9, 0x87FA7D7D, 0x15EFFAFA, 0xEBB25959,  0xC98E4747, 0x0BFBF0F0,
	0xEC41ADAD, 0x67B3D4D4,  0xFD5FA2A2, 0xEA45AFAF, 0xBF239C9C, 0xF753A4A4,  0x96E47272, 0x5B9BC0C0,
	0xC275B7B7, 0x1CE1FDFD,  0xAE3D9393, 0x6A4C2626, 0x5A6C3636, 0x417E3F3F,  0x02F5F7F7, 0x4F83CCCC,
	0x5C683434, 0xF451A5A5,  0x34D1E5E5, 0x08F9F1F1, 0x93E27171, 0x73ABD8D8,  0x53623131, 0x3F2A1515,
	0x0C080404, 0x5295C7C7,  0x65462323, 0x5E9DC3C3, 0x28301818, 0xA1379696,  0x0F0A0505, 0xB52F9A9A,
	0x090E0707, 0x36241212,  0x9B1B8080, 0x3DDFE2E2, 0x26CDEBEB, 0x694E2727,  0xCD7FB2B2, 0x9FEA7575,
	0x1B120909, 0x9E1D8383,  0x74582C2C, 0x2E341A1A, 0x2D361B1B, 0xB2DC6E6E,  0xEEB45A5A, 0xFB5BA0A0,
	0xF6A45252, 0x4D763B3B,  0x61B7D6D6, 0xCE7DB3B3, 0x7B522929, 0x3EDDE3E3,  0x715E2F2F, 0x97138484,
	0xF5A65353, 0x68B9D1D1,  0x00000000, 0x2CC1EDED, 0x60402020, 0x1FE3FCFC,  0xC879B1B1, 0xEDB65B5B,
	0xBED46A6A, 0x468DCBCB,  0xD967BEBE, 0x4B723939, 0xDE944A4A, 0xD4984C4C,  0xE8B05858, 0x4A85CFCF,
	0x6BBBD0D0, 0x2AC5EFEF,  0xE54FAAAA, 0x16EDFBFB, 0xC5864343, 0xD79A4D4D,  0x55663333, 0x94118585,
	0xCF8A4545, 0x10E9F9F9,  0x06040202, 0x81FE7F7F, 0xF0A05050, 0x44783C3C,  0xBA259F9F, 0xE34BA8A8,
	0xF3A25151, 0xFE5DA3A3,  0xC0804040, 0x8A058F8F, 0xAD3F9292, 0xBC219D9D,  0x48703838, 0x04F1F5F5,
	0xDF63BCBC, 0xC177B6B6,  0x75AFDADA, 0x63422121, 0x30201010, 0x1AE5FFFF,  0x0EFDF3F3, 0x6DBFD2D2,
	0x4C81CDCD, 0x14180C0C,  0x35261313, 0x2FC3ECEC, 0xE1BE5F5F, 0xA2359797,  0xCC884444, 0x392E1717,
	0x5793C4C4, 0xF255A7A7,  0x82FC7E7E, 0x477A3D3D, 0xACC86464, 0xE7BA5D5D,  0x2B321919, 0x95E67373,
	0xA0C06060, 0x98198181,  0xD19E4F4F, 0x7FA3DCDC, 0x66442222, 0x7E542A2A,  0xAB3B9090, 0x830B8888,
	0xCA8C4646, 0x29C7EEEE,  0xD36BB8B8, 0x3C281414, 0x79A7DEDE, 0xE2BC5E5E,  0x1D160B0B, 0x76ADDBDB,
	0x3BDBE0E0, 0x56643232,  0x4E743A3A, 0x1E140A0A, 0xDB924949, 0x0A0C0606,  0x6C482424, 0xE4B85C5C,
	0x5D9FC2C2, 0x6EBDD3D3,  0xEF43ACAC, 0xA6C46262, 0xA8399191, 0xA4319595,  0x37D3E4E4, 0x8BF27979,
	0x32D5E7E7, 0x438BC8C8,  0x596E3737, 0xB7DA6D6D, 0x8C018D8D, 0x64B1D5D5,  0xD29C4E4E, 0xE049A9A9,
	0xB4D86C6C, 0xFAAC5656,  0x07F3F4F4, 0x25CFEAEA, 0xAFCA6565, 0x8EF47A7A,  0xE947AEAE, 0x18100808,
	0xD56FBABA, 0x88F07878,  0x6F4A2525, 0x725C2E2E, 0x24381C1C, 0xF157A6A6,  0xC773B4B4, 0x5197C6C6,
	0x23CBE8E8, 0x7CA1DDDD,  0x9CE87474, 0x213E1F1F, 0xDD964B4B, 0xDC61BDBD,  0x860D8B8B, 0x850F8A8A,
	0x90E07070, 0x427C3E3E,  0xC471B5B5, 0xAACC6666, 0xD8904848, 0x05060303,  0x01F7F6F6, 0x121C0E0E,
	0xA3C26161, 0x5F6A3535,  0xF9AE5757, 0xD069B9B9, 0x91178686, 0x5899C1C1,  0x273A1D1D, 0xB9279E9E,
	0x38D9E1E1, 0x13EBF8F8,  0xB32B9898, 0x33221111, 0xBBD26969, 0x70A9D9D9,  0x89078E8E, 0xA7339494,
	0xB62D9B9B, 0x223C1E1E,  0x92158787, 0x20C9E9E9, 0x4987CECE, 0xFFAA5555,  0x78502828, 0x7AA5DFDF,
	0x8F038C8C, 0xF859A1A1,  0x80098989, 0x171A0D0D, 0xDA65BFBF, 0x31D7E6E6,  0xC6844242, 0xB8D06868,
	0xC3824141, 0xB0299999,  0x775A2D2D, 0x111E0F0F, 0xCB7BB0B0, 0xFCA85454,  0xD66DBBBB, 0x3A2C1616,
};

/* Corresponds to _TInv3[256] */
unsigned int mtpz_aes_rt1[] =
{
	0xF4A75051, 0x4165537E,  0x17A4C31A, 0x275E963A, 0xAB6BCB3B, 0x9D45F11F,  0xFA58ABAC, 0xE303934B,
	0x30FA5520, 0x766DF6AD,  0xCC769188, 0x024C25F5, 0xE5D7FC4F, 0x2ACBD7C5,  0x35448026, 0x62A38FB5,
	0xB15A49DE, 0xBA1B6725,  0xEA0E9845, 0xFEC0E15D, 0x2F7502C3, 0x4CF01281,  0x4697A38D, 0xD3F9C66B,
	0x8F5FE703, 0x929C9515,  0x6D7AEBBF, 0x5259DA95, 0xBE832DD4, 0x7421D358,  0xE0692949, 0xC9C8448E,
	0xC2896A75, 0x8E7978F4,  0x583E6B99, 0xB971DD27, 0xE14FB6BE, 0x88AD17F0,  0x20AC66C9, 0xCE3AB47D,
	0xDF4A1863, 0x1A3182E5,  0x51336097, 0x537F4562, 0x6477E0B1, 0x6BAE84BB,  0x81A01CFE, 0x082B94F9,
	0x48685870, 0x45FD198F,  0xDE6C8794, 0x7BF8B752, 0x73D323AB, 0x4B02E272,  0x1F8F57E3, 0x55AB2A66,
	0xEB2807B2, 0xB5C2032F,  0xC57B9A86, 0x3708A5D3, 0x2887F230, 0xBFA5B223,  0x036ABA02, 0x16825CED,
	0xCF1C2B8A, 0x79B492A7,  0x07F2F0F3, 0x69E2A14E, 0xDAF4CD65, 0x05BED506,  0x34621FD1, 0xA6FE8AC4,
	0x2E539D34, 0xF355A0A2,  0x8AE13205, 0xF6EB75A4, 0x83EC390B, 0x60EFAA40,  0x719F065E, 0x6E1051BD,
	0x218AF93E, 0xDD063D96,  0x3E05AEDD, 0xE6BD464D, 0x548DB591, 0xC45D0571,  0x06D46F04, 0x5015FF60,
	0x98FB2419, 0xBDE997D6,  0x4043CC89, 0xD99E7767, 0xE842BDB0, 0x898B8807,  0x195B38E7, 0xC8EEDB79,
	0x7C0A47A1, 0x420FE97C,  0x841EC9F8, 0x00000000, 0x80868309, 0x2BED4832,  0x1170AC1E, 0x5A724E6C,
	0x0EFFFBFD, 0x8538560F,  0xAED51E3D, 0x2D392736, 0x0FD9640A, 0x5CA62168,  0x5B54D19B, 0x362E3A24,
	0x0A67B10C, 0x57E70F93,  0xEE96D2B4, 0x9B919E1B, 0xC0C54F80, 0xDC20A261,  0x774B695A, 0x121A161C,
	0x93BA0AE2, 0xA02AE5C0,  0x22E0433C, 0x1B171D12, 0x090D0B0E, 0x8BC7ADF2,  0xB6A8B92D, 0x1EA9C814,
	0xF1198557, 0x75074CAF,  0x99DDBBEE, 0x7F60FDA3, 0x01269FF7, 0x72F5BC5C,  0x663BC544, 0xFB7E345B,
	0x4329768B, 0x23C6DCCB,  0xEDFC68B6, 0xE4F163B8, 0x31DCCAD7, 0x63851042,  0x97224013, 0xC6112084,
	0x4A247D85, 0xBB3DF8D2,  0xF93211AE, 0x29A16DC7, 0x9E2F4B1D, 0xB230F3DC,  0x8652EC0D, 0xC1E3D077,
	0xB3166C2B, 0x70B999A9,  0x9448FA11, 0xE9642247, 0xFC8CC4A8, 0xF03F1AA0,  0x7D2CD856, 0x3390EF22,
	0x494EC787, 0x38D1C1D9,  0xCAA2FE8C, 0xD40B3698, 0xF581CFA6, 0x7ADE28A5,  0xB78E26DA, 0xADBFA43F,
	0x3A9DE42C, 0x78920D50,  0x5FCC9B6A, 0x7E466254, 0x8D13C2F6, 0xD8B8E890,  0x39F75E2E, 0xC3AFF582,
	0x5D80BE9F, 0xD0937C69,  0xD52DA96F, 0x2512B3CF, 0xAC993BC8, 0x187DA710,  0x9C636EE8, 0x3BBB7BDB,
	0x267809CD, 0x5918F46E,  0x9AB701EC, 0x4F9AA883, 0x956E65E6, 0xFFE67EAA,  0xBCCF0821, 0x15E8E6EF,
	0xE79BD9BA, 0x6F36CE4A,  0x9F09D4EA, 0xB07CD629, 0xA4B2AF31, 0x3F23312A,  0xA59430C6, 0xA266C035,
	0x4EBC3774, 0x82CAA6FC,  0x90D0B0E0, 0xA7D81533, 0x04984AF1, 0xECDAF741,  0xCD500E7F, 0x91F62F17,
	0x4DD68D76, 0xEFB04D43,  0xAA4D54CC, 0x9604DFE4, 0xD1B5E39E, 0x6A881B4C,  0x2C1FB8C1, 0x65517F46,
	0x5EEA049D, 0x8C355D01,  0x877473FA, 0x0B412EFB, 0x671D5AB3, 0xDBD25292,  0x105633E9, 0xD647136D,
	0xD7618C9A, 0xA10C7A37,  0xF8148E59, 0x133C89EB, 0xA927EECE, 0x61C935B7,  0x1CE5EDE1, 0x47B13C7A,
	0xD2DF599C, 0xF2733F55,  0x14CE7918, 0xC737BF73, 0xF7CDEA53, 0xFDAA5B5F,  0x3D6F14DF, 0x44DB8678,
	0xAFF381CA, 0x68C43EB9,  0x24342C38, 0xA3405FC2, 0x1DC37216, 0xE2250CBC,  0x3C498B28, 0x0D9541FF,
	0xA8017139, 0x0CB3DE08,  0xB4E49CD8, 0x56C19064, 0xCB84617B, 0x32B670D5,  0x6C5C7448, 0xB85742D0
};

/* Corresponds to _TInv2[256] */
unsigned int mtpz_aes_rt2[] =
{
	0xA75051F4, 0x65537E41,  0xA4C31A17, 0x5E963A27, 0x6BCB3BAB, 0x45F11F9D,  0x58ABACFA, 0x03934BE3,
	0xFA552030, 0x6DF6AD76,  0x769188CC, 0x4C25F502, 0xD7FC4FE5, 0xCBD7C52A,  0x44802635, 0xA38FB562,
	0x5A49DEB1, 0x1B6725BA,  0x0E9845EA, 0xC0E15DFE, 0x7502C32F, 0xF012814C,  0x97A38D46, 0xF9C66BD3,
	0x5FE7038F, 0x9C951592,  0x7AEBBF6D, 0x59DA9552, 0x832DD4BE, 0x21D35874,  0x692949E0, 0xC8448EC9,
	0x896A75C2, 0x7978F48E,  0x3E6B9958, 0x71DD27B9, 0x4FB6BEE1, 0xAD17F088,  0xAC66C920, 0x3AB47DCE,
	0x4A1863DF, 0x3182E51A,  0x33609751, 0x7F456253, 0x77E0B164, 0xAE84BB6B,  0xA01CFE81, 0x2B94F908,
	0x68587048, 0xFD198F45,  0x6C8794DE, 0xF8B7527B, 0xD323AB73, 0x02E2724B,  0x8F57E31F, 0xAB2A6655,
	0x2807B2EB, 0xC2032FB5,  0x7B9A86C5, 0x08A5D337, 0x87F23028, 0xA5B223BF,  0x6ABA0203, 0x825CED16,
	0x1C2B8ACF, 0xB492A779,  0xF2F0F307, 0xE2A14E69, 0xF4CD65DA, 0xBED50605,  0x621FD134, 0xFE8AC4A6,
	0x539D342E, 0x55A0A2F3,  0xE132058A, 0xEB75A4F6, 0xEC390B83, 0xEFAA4060,  0x9F065E71, 0x1051BD6E,
	0x8AF93E21, 0x063D96DD,  0x05AEDD3E, 0xBD464DE6, 0x8DB59154, 0x5D0571C4,  0xD46F0406, 0x15FF6050,
	0xFB241998, 0xE997D6BD,  0x43CC8940, 0x9E7767D9, 0x42BDB0E8, 0x8B880789,  0x5B38E719, 0xEEDB79C8,
	0x0A47A17C, 0x0FE97C42,  0x1EC9F884, 0x00000000, 0x86830980, 0xED48322B,  0x70AC1E11, 0x724E6C5A,
	0xFFFBFD0E, 0x38560F85,  0xD51E3DAE, 0x3927362D, 0xD9640A0F, 0xA621685C,  0x54D19B5B, 0x2E3A2436,
	0x67B10C0A, 0xE70F9357,  0x96D2B4EE, 0x919E1B9B, 0xC54F80C0, 0x20A261DC,  0x4B695A77, 0x1A161C12,
	0xBA0AE293, 0x2AE5C0A0,  0xE0433C22, 0x171D121B, 0x0D0B0E09, 0xC7ADF28B,  0xA8B92DB6, 0xA9C8141E,
	0x198557F1, 0x074CAF75,  0xDDBBEE99, 0x60FDA37F, 0x269FF701, 0xF5BC5C72,  0x3BC54466, 0x7E345BFB,
	0x29768B43, 0xC6DCCB23,  0xFC68B6ED, 0xF163B8E4, 0xDCCAD731, 0x85104263,  0x22401397, 0x112084C6,
	0x247D854A, 0x3DF8D2BB,  0x3211AEF9, 0xA16DC729, 0x2F4B1D9E, 0x30F3DCB2,  0x52EC0D86, 0xE3D077C1,
	0x166C2BB3, 0xB999A970,  0x48FA1194, 0x642247E9, 0x8CC4A8FC, 0x3F1AA0F0,  0x2CD8567D, 0x90EF2233,
	0x4EC78749, 0xD1C1D938,  0xA2FE8CCA, 0x0B3698D4, 0x81CFA6F5, 0xDE28A57A,  0x8E26DAB7, 0xBFA43FAD,
	0x9DE42C3A, 0x920D5078,  0xCC9B6A5F, 0x4662547E, 0x13C2F68D, 0xB8E890D8,  0xF75E2E39, 0xAFF582C3,
	0x80BE9F5D, 0x937C69D0,  0x2DA96FD5, 0x12B3CF25, 0x993BC8AC, 0x7DA71018,  0x636EE89C, 0xBB7BDB3B,
	0x7809CD26, 0x18F46E59,  0xB701EC9A, 0x9AA8834F, 0x6E65E695, 0xE67EAAFF,  0xCF0821BC, 0xE8E6EF15,
	0x9BD9BAE7, 0x36CE4A6F,  0x09D4EA9F, 0x7CD629B0, 0xB2AF31A4, 0x23312A3F,  0x9430C6A5, 0x66C035A2,
	0xBC37744E, 0xCAA6FC82,  0xD0B0E090, 0xD81533A7, 0x984AF104, 0xDAF741EC,  0x500E7FCD, 0xF62F1791,
	0xD68D764D, 0xB04D43EF,  0x4D54CCAA, 0x04DFE496, 0xB5E39ED1, 0x881B4C6A,  0x1FB8C12C, 0x517F4665,
	0xEA049D5E, 0x355D018C,  0x7473FA87, 0x412EFB0B, 0x1D5AB367, 0xD25292DB,  0x5633E910, 0x47136DD6,
	0x618C9AD7, 0x0C7A37A1,  0x148E59F8, 0x3C89EB13, 0x27EECEA9, 0xC935B761,  0xE5EDE11C, 0xB13C7A47,
	0xDF599CD2, 0x733F55F2,  0xCE791814, 0x37BF73C7, 0xCDEA53F7, 0xAA5B5FFD,  0x6F14DF3D, 0xDB867844,
	0xF381CAAF, 0xC43EB968,  0x342C3824, 0x405FC2A3, 0xC372161D, 0x250CBCE2,  0x498B283C, 0x9541FF0D,
	0x017139A8, 0xB3DE080C,  0xE49CD8B4, 0xC1906456, 0x84617BCB, 0xB670D532,  0x5C74486C, 0x5742D0B8
};

/* Corresponds to _TInv0[256] */
unsigned int mtpz_aes_rt3[] =
{
	0x51F4A750, 0x7E416553,  0x1A17A4C3, 0x3A275E96, 0x3BAB6BCB, 0x1F9D45F1,  0xACFA58AB, 0x4BE30393,
	0x2030FA55, 0xAD766DF6,  0x88CC7691, 0xF5024C25, 0x4FE5D7FC, 0xC52ACBD7,  0x26354480, 0xB562A38F,
	0xDEB15A49, 0x25BA1B67,  0x45EA0E98, 0x5DFEC0E1, 0xC32F7502, 0x814CF012,  0x8D4697A3, 0x6BD3F9C6,
	0x038F5FE7, 0x15929C95,  0xBF6D7AEB, 0x955259DA, 0xD4BE832D, 0x587421D3,  0x49E06929, 0x8EC9C844,
	0x75C2896A, 0xF48E7978,  0x99583E6B, 0x27B971DD, 0xBEE14FB6, 0xF088AD17,  0xC920AC66, 0x7DCE3AB4,
	0x63DF4A18, 0xE51A3182,  0x97513360, 0x62537F45, 0xB16477E0, 0xBB6BAE84,  0xFE81A01C, 0xF9082B94,
	0x70486858, 0x8F45FD19,  0x94DE6C87, 0x527BF8B7, 0xAB73D323, 0x724B02E2,  0xE31F8F57, 0x6655AB2A,
	0xB2EB2807, 0x2FB5C203,  0x86C57B9A, 0xD33708A5, 0x302887F2, 0x23BFA5B2,  0x02036ABA, 0xED16825C,
	0x8ACF1C2B, 0xA779B492,  0xF307F2F0, 0x4E69E2A1, 0x65DAF4CD, 0x0605BED5,  0xD134621F, 0xC4A6FE8A,
	0x342E539D, 0xA2F355A0,  0x058AE132, 0xA4F6EB75, 0x0B83EC39, 0x4060EFAA,  0x5E719F06, 0xBD6E1051,
	0x3E218AF9, 0x96DD063D,  0xDD3E05AE, 0x4DE6BD46, 0x91548DB5, 0x71C45D05,  0x0406D46F, 0x605015FF,
	0x1998FB24, 0xD6BDE997,  0x894043CC, 0x67D99E77, 0xB0E842BD, 0x07898B88,  0xE7195B38, 0x79C8EEDB,
	0xA17C0A47, 0x7C420FE9,  0xF8841EC9, 0x00000000, 0x09808683, 0x322BED48,  0x1E1170AC, 0x6C5A724E,
	0xFD0EFFFB, 0x0F853856,  0x3DAED51E, 0x362D3927, 0x0A0FD964, 0x685CA621,  0x9B5B54D1, 0x24362E3A,
	0x0C0A67B1, 0x9357E70F,  0xB4EE96D2, 0x1B9B919E, 0x80C0C54F, 0x61DC20A2,  0x5A774B69, 0x1C121A16,
	0xE293BA0A, 0xC0A02AE5,  0x3C22E043, 0x121B171D, 0x0E090D0B, 0xF28BC7AD,  0x2DB6A8B9, 0x141EA9C8,
	0x57F11985, 0xAF75074C,  0xEE99DDBB, 0xA37F60FD, 0xF701269F, 0x5C72F5BC,  0x44663BC5, 0x5BFB7E34,
	0x8B432976, 0xCB23C6DC,  0xB6EDFC68, 0xB8E4F163, 0xD731DCCA, 0x42638510,  0x13972240, 0x84C61120,
	0x854A247D, 0xD2BB3DF8,  0xAEF93211, 0xC729A16D, 0x1D9E2F4B, 0xDCB230F3,  0x0D8652EC, 0x77C1E3D0,
	0x2BB3166C, 0xA970B999,  0x119448FA, 0x47E96422, 0xA8FC8CC4, 0xA0F03F1A,  0x567D2CD8, 0x223390EF,
	0x87494EC7, 0xD938D1C1,  0x8CCAA2FE, 0x98D40B36, 0xA6F581CF, 0xA57ADE28,  0xDAB78E26, 0x3FADBFA4,
	0x2C3A9DE4, 0x5078920D,  0x6A5FCC9B, 0x547E4662, 0xF68D13C2, 0x90D8B8E8,  0x2E39F75E, 0x82C3AFF5,
	0x9F5D80BE, 0x69D0937C,  0x6FD52DA9, 0xCF2512B3, 0xC8AC993B, 0x10187DA7,  0xE89C636E, 0xDB3BBB7B,
	0xCD267809, 0x6E5918F4,  0xEC9AB701, 0x834F9AA8, 0xE6956E65, 0xAAFFE67E,  0x21BCCF08, 0xEF15E8E6,
	0xBAE79BD9, 0x4A6F36CE,  0xEA9F09D4, 0x29B07CD6, 0x31A4B2AF, 0x2A3F2331,  0xC6A59430, 0x35A266C0,
	0x744EBC37, 0xFC82CAA6,  0xE090D0B0, 0x33A7D815, 0xF104984A, 0x41ECDAF7,  0x7FCD500E, 0x1791F62F,
	0x764DD68D, 0x43EFB04D,  0xCCAA4D54, 0xE49604DF, 0x9ED1B5E3, 0x4C6A881B,  0xC12C1FB8, 0x4665517F,
	0x9D5EEA04, 0x018C355D,  0xFA877473, 0xFB0B412E, 0xB3671D5A, 0x92DBD252,  0xE9105633, 0x6DD64713,
	0x9AD7618C, 0x37A10C7A,  0x59F8148E, 0xEB133C89, 0xCEA927EE, 0xB761C935,  0xE11CE5ED, 0x7A47B13C,
	0x9CD2DF59, 0x55F2733F,  0x1814CE79, 0x73C737BF, 0x53F7CDEA, 0x5FFDAA5B,  0xDF3D6F14, 0x7844DB86,
	0xCAAFF381, 0xB968C43E,  0x3824342C, 0xC2A3405F, 0x161DC372, 0xBCE2250C,  0x283C498B, 0xFF0D9541,
	0x39A80171, 0x080CB3DE,  0xD8B4E49C, 0x6456C190, 0x7BCB8461, 0xD532B670,  0x486C5C74, 0xD0B85742
};

/* Corresponds to _TInv1[256] */
unsigned int mtpz_aes_rt4[] =
{
	0x5051F4A7, 0x537E4165,  0xC31A17A4, 0x963A275E, 0xCB3BAB6B, 0xF11F9D45,  0xABACFA58, 0x934BE303,
	0x552030FA, 0xF6AD766D,  0x9188CC76, 0x25F5024C, 0xFC4FE5D7, 0xD7C52ACB,  0x80263544, 0x8FB562A3,
	0x49DEB15A, 0x6725BA1B,  0x9845EA0E, 0xE15DFEC0, 0x02C32F75, 0x12814CF0,  0xA38D4697, 0xC66BD3F9,
	0xE7038F5F, 0x9515929C,  0xEBBF6D7A, 0xDA955259, 0x2DD4BE83, 0xD3587421,  0x2949E069, 0x448EC9C8,
	0x6A75C289, 0x78F48E79,  0x6B99583E, 0xDD27B971, 0xB6BEE14F, 0x17F088AD,  0x66C920AC, 0xB47DCE3A,
	0x1863DF4A, 0x82E51A31,  0x60975133, 0x4562537F, 0xE0B16477, 0x84BB6BAE,  0x1CFE81A0, 0x94F9082B,
	0x58704868, 0x198F45FD,  0x8794DE6C, 0xB7527BF8, 0x23AB73D3, 0xE2724B02,  0x57E31F8F, 0x2A6655AB,
	0x07B2EB28, 0x032FB5C2,  0x9A86C57B, 0xA5D33708, 0xF2302887, 0xB223BFA5,  0xBA02036A, 0x5CED1682,
	0x2B8ACF1C, 0x92A779B4,  0xF0F307F2, 0xA14E69E2, 0xCD65DAF4, 0xD50605BE,  0x1FD13462, 0x8AC4A6FE,
	0x9D342E53, 0xA0A2F355,  0x32058AE1, 0x75A4F6EB, 0x390B83EC, 0xAA4060EF,  0x065E719F, 0x51BD6E10,
	0xF93E218A, 0x3D96DD06,  0xAEDD3E05, 0x464DE6BD, 0xB591548D, 0x0571C45D,  0x6F0406D4, 0xFF605015,
	0x241998FB, 0x97D6BDE9,  0xCC894043, 0x7767D99E, 0xBDB0E842, 0x8807898B,  0x38E7195B, 0xDB79C8EE,
	0x47A17C0A, 0xE97C420F,  0xC9F8841E, 0x00000000, 0x83098086, 0x48322BED,  0xAC1E1170, 0x4E6C5A72,
	0xFBFD0EFF, 0x560F8538,  0x1E3DAED5, 0x27362D39, 0x640A0FD9, 0x21685CA6,  0xD19B5B54, 0x3A24362E,
	0xB10C0A67, 0x0F9357E7,  0xD2B4EE96, 0x9E1B9B91, 0x4F80C0C5, 0xA261DC20,  0x695A774B, 0x161C121A,
	0x0AE293BA, 0xE5C0A02A,  0x433C22E0, 0x1D121B17, 0x0B0E090D, 0xADF28BC7,  0xB92DB6A8, 0xC8141EA9,
	0x8557F119, 0x4CAF7507,  0xBBEE99DD, 0xFDA37F60, 0x9FF70126, 0xBC5C72F5,  0xC544663B, 0x345BFB7E,
	0x768B4329, 0xDCCB23C6,  0x68B6EDFC, 0x63B8E4F1, 0xCAD731DC, 0x10426385,  0x40139722, 0x2084C611,
	0x7D854A24, 0xF8D2BB3D,  0x11AEF932, 0x6DC729A1, 0x4B1D9E2F, 0xF3DCB230,  0xEC0D8652, 0xD077C1E3,
	0x6C2BB316, 0x99A970B9,  0xFA119448, 0x2247E964, 0xC4A8FC8C, 0x1AA0F03F,  0xD8567D2C, 0xEF223390,
	0xC787494E, 0xC1D938D1,  0xFE8CCAA2, 0x3698D40B, 0xCFA6F581, 0x28A57ADE,  0x26DAB78E, 0xA43FADBF,
	0xE42C3A9D, 0x0D507892,  0x9B6A5FCC, 0x62547E46, 0xC2F68D13, 0xE890D8B8,  0x5E2E39F7, 0xF582C3AF,
	0xBE9F5D80, 0x7C69D093,  0xA96FD52D, 0xB3CF2512, 0x3BC8AC99, 0xA710187D,  0x6EE89C63, 0x7BDB3BBB,
	0x09CD2678, 0xF46E5918,  0x01EC9AB7, 0xA8834F9A, 0x65E6956E, 0x7EAAFFE6,  0x0821BCCF, 0xE6EF15E8,
	0xD9BAE79B, 0xCE4A6F36,  0xD4EA9F09, 0xD629B07C, 0xAF31A4B2, 0x312A3F23,  0x30C6A594, 0xC035A266,
	0x37744EBC, 0xA6FC82CA,  0xB0E090D0, 0x1533A7D8, 0x4AF10498, 0xF741ECDA,  0x0E7FCD50, 0x2F1791F6,
	0x8D764DD6, 0x4D43EFB0,  0x54CCAA4D, 0xDFE49604, 0xE39ED1B5, 0x1B4C6A88,  0xB8C12C1F, 0x7F466551,
	0x049D5EEA, 0x5D018C35,  0x73FA8774, 0x2EFB0B41, 0x5AB3671D, 0x5292DBD2,  0x33E91056, 0x136DD647,
	0x8C9AD761, 0x7A37A10C,  0x8E59F814, 0x89EB133C, 0xEECEA927, 0x35B761C9,  0xEDE11CE5, 0x3C7A47B1,
	0x599CD2DF, 0x3F55F273,  0x791814CE, 0xBF73C737, 0xEA53F7CD, 0x5B5FFDAA,  0x14DF3D6F, 0x867844DB,
	0x81CAAFF3, 0x3EB968C4,  0x2C382434, 0x5FC2A340, 0x72161DC3, 0x0CBCE225,  0x8B283C49, 0x41FF0D95,
	0x7139A801, 0xDE080CB3,  0x9CD8B4E4, 0x906456C1, 0x617BCB84, 0x70D532B6,  0x74486C5C, 0x42D0B857
};

/* Corresponds to _IMXC1[256] */
unsigned int mtpz_aes_gb11[] =
{
	0x00000000, 0x0B0E090D,  0x161C121A, 0x1D121B17, 0x2C382434, 0x27362D39,  0x3A24362E, 0x312A3F23,
	0x58704868, 0x537E4165,  0x4E6C5A72, 0x4562537F, 0x74486C5C, 0x7F466551,  0x62547E46, 0x695A774B,
	0xB0E090D0, 0xBBEE99DD,  0xA6FC82CA, 0xADF28BC7, 0x9CD8B4E4, 0x97D6BDE9,  0x8AC4A6FE, 0x81CAAFF3,
	0xE890D8B8, 0xE39ED1B5,  0xFE8CCAA2, 0xF582C3AF, 0xC4A8FC8C, 0xCFA6F581,  0xD2B4EE96, 0xD9BAE79B,
	0x7BDB3BBB, 0x70D532B6,  0x6DC729A1, 0x66C920AC, 0x57E31F8F, 0x5CED1682,  0x41FF0D95, 0x4AF10498,
	0x23AB73D3, 0x28A57ADE,  0x35B761C9, 0x3EB968C4, 0x0F9357E7, 0x049D5EEA,  0x198F45FD, 0x12814CF0,
	0xCB3BAB6B, 0xC035A266,  0xDD27B971, 0xD629B07C, 0xE7038F5F, 0xEC0D8652,  0xF11F9D45, 0xFA119448,
	0x934BE303, 0x9845EA0E,  0x8557F119, 0x8E59F814, 0xBF73C737, 0xB47DCE3A,  0xA96FD52D, 0xA261DC20,
	0xF6AD766D, 0xFDA37F60,  0xE0B16477, 0xEBBF6D7A, 0xDA955259, 0xD19B5B54,  0xCC894043, 0xC787494E,
	0xAEDD3E05, 0xA5D33708,  0xB8C12C1F, 0xB3CF2512, 0x82E51A31, 0x89EB133C,  0x94F9082B, 0x9FF70126,
	0x464DE6BD, 0x4D43EFB0,  0x5051F4A7, 0x5B5FFDAA, 0x6A75C289, 0x617BCB84,  0x7C69D093, 0x7767D99E,
	0x1E3DAED5, 0x1533A7D8,  0x0821BCCF, 0x032FB5C2, 0x32058AE1, 0x390B83EC,  0x241998FB, 0x2F1791F6,
	0x8D764DD6, 0x867844DB,  0x9B6A5FCC, 0x906456C1, 0xA14E69E2, 0xAA4060EF,  0xB7527BF8, 0xBC5C72F5,
	0xD50605BE, 0xDE080CB3,  0xC31A17A4, 0xC8141EA9, 0xF93E218A, 0xF2302887,  0xEF223390, 0xE42C3A9D,
	0x3D96DD06, 0x3698D40B,  0x2B8ACF1C, 0x2084C611, 0x11AEF932, 0x1AA0F03F,  0x07B2EB28, 0x0CBCE225,
	0x65E6956E, 0x6EE89C63,  0x73FA8774, 0x78F48E79, 0x49DEB15A, 0x42D0B857,  0x5FC2A340, 0x54CCAA4D,
	0xF741ECDA, 0xFC4FE5D7,  0xE15DFEC0, 0xEA53F7CD, 0xDB79C8EE, 0xD077C1E3,  0xCD65DAF4, 0xC66BD3F9,
	0xAF31A4B2, 0xA43FADBF,  0xB92DB6A8, 0xB223BFA5, 0x83098086, 0x8807898B,  0x9515929C, 0x9E1B9B91,
	0x47A17C0A, 0x4CAF7507,  0x51BD6E10, 0x5AB3671D, 0x6B99583E, 0x60975133,  0x7D854A24, 0x768B4329,
	0x1FD13462, 0x14DF3D6F,  0x09CD2678, 0x02C32F75, 0x33E91056, 0x38E7195B,  0x25F5024C, 0x2EFB0B41,
	0x8C9AD761, 0x8794DE6C,  0x9A86C57B, 0x9188CC76, 0xA0A2F355, 0xABACFA58,  0xB6BEE14F, 0xBDB0E842,
	0xD4EA9F09, 0xDFE49604,  0xC2F68D13, 0xC9F8841E, 0xF8D2BB3D, 0xF3DCB230,  0xEECEA927, 0xE5C0A02A,
	0x3C7A47B1, 0x37744EBC,  0x2A6655AB, 0x21685CA6, 0x10426385, 0x1B4C6A88,  0x065E719F, 0x0D507892,
	0x640A0FD9, 0x6F0406D4,  0x72161DC3, 0x791814CE, 0x48322BED, 0x433C22E0,  0x5E2E39F7, 0x552030FA,
	0x01EC9AB7, 0x0AE293BA,  0x17F088AD, 0x1CFE81A0, 0x2DD4BE83, 0x26DAB78E,  0x3BC8AC99, 0x30C6A594,
	0x599CD2DF, 0x5292DBD2,  0x4F80C0C5, 0x448EC9C8, 0x75A4F6EB, 0x7EAAFFE6,  0x63B8E4F1, 0x68B6EDFC,
	0xB10C0A67, 0xBA02036A,  0xA710187D, 0xAC1E1170, 0x9D342E53, 0x963A275E,  0x8B283C49, 0x80263544,
	0xE97C420F, 0xE2724B02,  0xFF605015, 0xF46E5918, 0xC544663B, 0xCE4A6F36,  0xD3587421, 0xD8567D2C,
	0x7A37A10C, 0x7139A801,  0x6C2BB316, 0x6725BA1B, 0x560F8538, 0x5D018C35,  0x40139722, 0x4B1D9E2F,
	0x2247E964, 0x2949E069,  0x345BFB7E, 0x3F55F273, 0x0E7FCD50, 0x0571C45D,  0x1863DF4A, 0x136DD647,
	0xCAD731DC, 0xC1D938D1,  0xDCCB23C6, 0xD7C52ACB, 0xE6EF15E8, 0xEDE11CE5,  0xF0F307F2, 0xFBFD0EFF,
	0x92A779B4, 0x99A970B9,  0x84BB6BAE, 0x8FB562A3, 0xBE9F5D80, 0xB591548D,  0xA8834F9A, 0xA38D4697,
};

/* Corresponds to _IMXC0[256] */
unsigned int mtpz_aes_gb14[] =
{
	0x00000000,  0x0E090D0B,   0x1C121A16,  0x121B171D,  0x3824342C,  0x362D3927,   0x24362E3A,  0x2A3F2331,
	0x70486858,  0x7E416553,   0x6C5A724E,  0x62537F45,  0x486C5C74,  0x4665517F,   0x547E4662,  0x5A774B69,
	0xE090D0B0,  0xEE99DDBB,   0xFC82CAA6,  0xF28BC7AD,  0xD8B4E49C,  0xD6BDE997,   0xC4A6FE8A,  0xCAAFF381,
	0x90D8B8E8,  0x9ED1B5E3,   0x8CCAA2FE,  0x82C3AFF5,  0xA8FC8CC4,  0xA6F581CF,   0xB4EE96D2,  0xBAE79BD9,
	0xDB3BBB7B,  0xD532B670,   0xC729A16D,  0xC920AC66,  0xE31F8F57,  0xED16825C,   0xFF0D9541,  0xF104984A,
	0xAB73D323,  0xA57ADE28,   0xB761C935,  0xB968C43E,  0x9357E70F,  0x9D5EEA04,   0x8F45FD19,  0x814CF012,
	0x3BAB6BCB,  0x35A266C0,   0x27B971DD,  0x29B07CD6,  0x038F5FE7,  0x0D8652EC,   0x1F9D45F1,  0x119448FA,
	0x4BE30393,  0x45EA0E98,   0x57F11985,  0x59F8148E,  0x73C737BF,  0x7DCE3AB4,   0x6FD52DA9,  0x61DC20A2,
	0xAD766DF6,  0xA37F60FD,   0xB16477E0,  0xBF6D7AEB,  0x955259DA,  0x9B5B54D1,   0x894043CC,  0x87494EC7,
	0xDD3E05AE,  0xD33708A5,   0xC12C1FB8,  0xCF2512B3,  0xE51A3182,  0xEB133C89,   0xF9082B94,  0xF701269F,
	0x4DE6BD46,  0x43EFB04D,   0x51F4A750,  0x5FFDAA5B,  0x75C2896A,  0x7BCB8461,   0x69D0937C,  0x67D99E77,
	0x3DAED51E,  0x33A7D815,   0x21BCCF08,  0x2FB5C203,  0x058AE132,  0x0B83EC39,   0x1998FB24,  0x1791F62F,
	0x764DD68D,  0x7844DB86,   0x6A5FCC9B,  0x6456C190,  0x4E69E2A1,  0x4060EFAA,   0x527BF8B7,  0x5C72F5BC,
	0x0605BED5,  0x080CB3DE,   0x1A17A4C3,  0x141EA9C8,  0x3E218AF9,  0x302887F2,   0x223390EF,  0x2C3A9DE4,
	0x96DD063D,  0x98D40B36,   0x8ACF1C2B,  0x84C61120,  0xAEF93211,  0xA0F03F1A,   0xB2EB2807,  0xBCE2250C,
	0xE6956E65,  0xE89C636E,   0xFA877473,  0xF48E7978,  0xDEB15A49,  0xD0B85742,   0xC2A3405F,  0xCCAA4D54,
	0x41ECDAF7,  0x4FE5D7FC,   0x5DFEC0E1,  0x53F7CDEA,  0x79C8EEDB,  0x77C1E3D0,   0x65DAF4CD,  0x6BD3F9C6,
	0x31A4B2AF,  0x3FADBFA4,   0x2DB6A8B9,  0x23BFA5B2,  0x09808683,  0x07898B88,   0x15929C95,  0x1B9B919E,
	0xA17C0A47,  0xAF75074C,   0xBD6E1051,  0xB3671D5A,  0x99583E6B,  0x97513360,   0x854A247D,  0x8B432976,
	0xD134621F,  0xDF3D6F14,   0xCD267809,  0xC32F7502,  0xE9105633,  0xE7195B38,   0xF5024C25,  0xFB0B412E,
	0x9AD7618C,  0x94DE6C87,   0x86C57B9A,  0x88CC7691,  0xA2F355A0,  0xACFA58AB,   0xBEE14FB6,  0xB0E842BD,
	0xEA9F09D4,  0xE49604DF,   0xF68D13C2,  0xF8841EC9,  0xD2BB3DF8,  0xDCB230F3,   0xCEA927EE,  0xC0A02AE5,
	0x7A47B13C,  0x744EBC37,   0x6655AB2A,  0x685CA621,  0x42638510,  0x4C6A881B,   0x5E719F06,  0x5078920D,
	0x0A0FD964,  0x0406D46F,   0x161DC372,  0x1814CE79,  0x322BED48,  0x3C22E043,   0x2E39F75E,  0x2030FA55,
	0xEC9AB701,  0xE293BA0A,   0xF088AD17,  0xFE81A01C,  0xD4BE832D,  0xDAB78E26,   0xC8AC993B,  0xC6A59430,
	0x9CD2DF59,  0x92DBD252,   0x80C0C54F,  0x8EC9C844,  0xA4F6EB75,  0xAAFFE67E,   0xB8E4F163,  0xB6EDFC68,
	0x0C0A67B1,  0x02036ABA,   0x10187DA7,  0x1E1170AC,  0x342E539D,  0x3A275E96,   0x283C498B,  0x26354480,
	0x7C420FE9,  0x724B02E2,   0x605015FF,  0x6E5918F4,  0x44663BC5,  0x4A6F36CE,   0x587421D3,  0x567D2CD8,
	0x37A10C7A,  0x39A80171,   0x2BB3166C,  0x25BA1B67,  0x0F853856,  0x018C355D,   0x13972240,  0x1D9E2F4B,
	0x47E96422,  0x49E06929,   0x5BFB7E34,  0x55F2733F,  0x7FCD500E,  0x71C45D05,   0x63DF4A18,  0x6DD64713,
	0xD731DCCA,  0xD938D1C1,   0xCB23C6DC,  0xC52ACBD7,  0xEF15E8E6,  0xE11CE5ED,   0xF307F2F0,  0xFD0EFFFB,
	0xA779B492,  0xA970B999,   0xBB6BAE84,  0xB562A38F,  0x9F5D80BE,  0x91548DB5,   0x834F9AA8,  0x8D4697A3,
} ;

/* Corresponds to _IMXC2[256] */
unsigned int mtpz_aes_gb13[] =
{
	0x00000000,  0x0D0B0E09,   0x1A161C12,  0x171D121B,  0x342C3824,  0x3927362D,   0x2E3A2436,  0x23312A3F,
	0x68587048,  0x65537E41,   0x724E6C5A,  0x7F456253,  0x5C74486C,  0x517F4665,   0x4662547E,  0x4B695A77,
	0xD0B0E090,  0xDDBBEE99,   0xCAA6FC82,  0xC7ADF28B,  0xE49CD8B4,  0xE997D6BD,   0xFE8AC4A6,  0xF381CAAF,
	0xB8E890D8,  0xB5E39ED1,   0xA2FE8CCA,  0xAFF582C3,  0x8CC4A8FC,  0x81CFA6F5,   0x96D2B4EE,  0x9BD9BAE7,
	0xBB7BDB3B,  0xB670D532,   0xA16DC729,  0xAC66C920,  0x8F57E31F,  0x825CED16,   0x9541FF0D,  0x984AF104,
	0xD323AB73,  0xDE28A57A,   0xC935B761,  0xC43EB968,  0xE70F9357,  0xEA049D5E,   0xFD198F45,  0xF012814C,
	0x6BCB3BAB,  0x66C035A2,   0x71DD27B9,  0x7CD629B0,  0x5FE7038F,  0x52EC0D86,   0x45F11F9D,  0x48FA1194,
	0x03934BE3,  0x0E9845EA,   0x198557F1,  0x148E59F8,  0x37BF73C7,  0x3AB47DCE,   0x2DA96FD5,  0x20A261DC,
	0x6DF6AD76,  0x60FDA37F,   0x77E0B164,  0x7AEBBF6D,  0x59DA9552,  0x54D19B5B,   0x43CC8940,  0x4EC78749,
	0x05AEDD3E,  0x08A5D337,   0x1FB8C12C,  0x12B3CF25,  0x3182E51A,  0x3C89EB13,   0x2B94F908,  0x269FF701,
	0xBD464DE6,  0xB04D43EF,   0xA75051F4,  0xAA5B5FFD,  0x896A75C2,  0x84617BCB,   0x937C69D0,  0x9E7767D9,
	0xD51E3DAE,  0xD81533A7,   0xCF0821BC,  0xC2032FB5,  0xE132058A,  0xEC390B83,   0xFB241998,  0xF62F1791,
	0xD68D764D,  0xDB867844,   0xCC9B6A5F,  0xC1906456,  0xE2A14E69,  0xEFAA4060,   0xF8B7527B,  0xF5BC5C72,
	0xBED50605,  0xB3DE080C,   0xA4C31A17,  0xA9C8141E,  0x8AF93E21,  0x87F23028,   0x90EF2233,  0x9DE42C3A,
	0x063D96DD,  0x0B3698D4,   0x1C2B8ACF,  0x112084C6,  0x3211AEF9,  0x3F1AA0F0,   0x2807B2EB,  0x250CBCE2,
	0x6E65E695,  0x636EE89C,   0x7473FA87,  0x7978F48E,  0x5A49DEB1,  0x5742D0B8,   0x405FC2A3,  0x4D54CCAA,
	0xDAF741EC,  0xD7FC4FE5,   0xC0E15DFE,  0xCDEA53F7,  0xEEDB79C8,  0xE3D077C1,   0xF4CD65DA,  0xF9C66BD3,
	0xB2AF31A4,  0xBFA43FAD,   0xA8B92DB6,  0xA5B223BF,  0x86830980,  0x8B880789,   0x9C951592,  0x919E1B9B,
	0x0A47A17C,  0x074CAF75,   0x1051BD6E,  0x1D5AB367,  0x3E6B9958,  0x33609751,   0x247D854A,  0x29768B43,
	0x621FD134,  0x6F14DF3D,   0x7809CD26,  0x7502C32F,  0x5633E910,  0x5B38E719,   0x4C25F502,  0x412EFB0B,
	0x618C9AD7,  0x6C8794DE,   0x7B9A86C5,  0x769188CC,  0x55A0A2F3,  0x58ABACFA,   0x4FB6BEE1,  0x42BDB0E8,
	0x09D4EA9F,  0x04DFE496,   0x13C2F68D,  0x1EC9F884,  0x3DF8D2BB,  0x30F3DCB2,   0x27EECEA9,  0x2AE5C0A0,
	0xB13C7A47,  0xBC37744E,   0xAB2A6655,  0xA621685C,  0x85104263,  0x881B4C6A,   0x9F065E71,  0x920D5078,
	0xD9640A0F,  0xD46F0406,   0xC372161D,  0xCE791814,  0xED48322B,  0xE0433C22,   0xF75E2E39,  0xFA552030,
	0xB701EC9A,  0xBA0AE293,   0xAD17F088,  0xA01CFE81,  0x832DD4BE,  0x8E26DAB7,   0x993BC8AC,  0x9430C6A5,
	0xDF599CD2,  0xD25292DB,   0xC54F80C0,  0xC8448EC9,  0xEB75A4F6,  0xE67EAAFF,   0xF163B8E4,  0xFC68B6ED,
	0x67B10C0A,  0x6ABA0203,   0x7DA71018,  0x70AC1E11,  0x539D342E,  0x5E963A27,   0x498B283C,  0x44802635,
	0x0FE97C42,  0x02E2724B,   0x15FF6050,  0x18F46E59,  0x3BC54466,  0x36CE4A6F,   0x21D35874,  0x2CD8567D,
	0x0C7A37A1,  0x017139A8,   0x166C2BB3,  0x1B6725BA,  0x38560F85,  0x355D018C,   0x22401397,  0x2F4B1D9E,
	0x642247E9,  0x692949E0,   0x7E345BFB,  0x733F55F2,  0x500E7FCD,  0x5D0571C4,   0x4A1863DF,  0x47136DD6,
	0xDCCAD731,  0xD1C1D938,   0xC6DCCB23,  0xCBD7C52A,  0xE8E6EF15,  0xE5EDE11C,   0xF2F0F307,  0xFFFBFD0E,
	0xB492A779,  0xB999A970,   0xAE84BB6B,  0xA38FB562,  0x80BE9F5D,  0x8DB59154,   0x9AA8834F,  0x97A38D46,
};

/* Corresponds to _IMXC3[256] */
unsigned int mtpz_aes_gb9[] =
{
	0x00000000,  0x090D0B0E,   0x121A161C,  0x1B171D12,  0x24342C38,  0x2D392736,   0x362E3A24,  0x3F23312A,
	0x48685870,  0x4165537E,   0x5A724E6C,  0x537F4562,  0x6C5C7448,  0x65517F46,   0x7E466254,  0x774B695A,
	0x90D0B0E0,  0x99DDBBEE,   0x82CAA6FC,  0x8BC7ADF2,  0xB4E49CD8,  0xBDE997D6,   0xA6FE8AC4,  0xAFF381CA,
	0xD8B8E890,  0xD1B5E39E,   0xCAA2FE8C,  0xC3AFF582,  0xFC8CC4A8,  0xF581CFA6,   0xEE96D2B4,  0xE79BD9BA,
	0x3BBB7BDB,  0x32B670D5,   0x29A16DC7,  0x20AC66C9,  0x1F8F57E3,  0x16825CED,   0x0D9541FF,  0x04984AF1,
	0x73D323AB,  0x7ADE28A5,   0x61C935B7,  0x68C43EB9,  0x57E70F93,  0x5EEA049D,   0x45FD198F,  0x4CF01281,
	0xAB6BCB3B,  0xA266C035,   0xB971DD27,  0xB07CD629,  0x8F5FE703,  0x8652EC0D,   0x9D45F11F,  0x9448FA11,
	0xE303934B,  0xEA0E9845,   0xF1198557,  0xF8148E59,  0xC737BF73,  0xCE3AB47D,   0xD52DA96F,  0xDC20A261,
	0x766DF6AD,  0x7F60FDA3,   0x6477E0B1,  0x6D7AEBBF,  0x5259DA95,  0x5B54D19B,   0x4043CC89,  0x494EC787,
	0x3E05AEDD,  0x3708A5D3,   0x2C1FB8C1,  0x2512B3CF,  0x1A3182E5,  0x133C89EB,   0x082B94F9,  0x01269FF7,
	0xE6BD464D,  0xEFB04D43,   0xF4A75051,  0xFDAA5B5F,  0xC2896A75,  0xCB84617B,   0xD0937C69,  0xD99E7767,
	0xAED51E3D,  0xA7D81533,   0xBCCF0821,  0xB5C2032F,  0x8AE13205,  0x83EC390B,   0x98FB2419,  0x91F62F17,
	0x4DD68D76,  0x44DB8678,   0x5FCC9B6A,  0x56C19064,  0x69E2A14E,  0x60EFAA40,   0x7BF8B752,  0x72F5BC5C,
	0x05BED506,  0x0CB3DE08,   0x17A4C31A,  0x1EA9C814,  0x218AF93E,  0x2887F230,   0x3390EF22,  0x3A9DE42C,
	0xDD063D96,  0xD40B3698,   0xCF1C2B8A,  0xC6112084,  0xF93211AE,  0xF03F1AA0,   0xEB2807B2,  0xE2250CBC,
	0x956E65E6,  0x9C636EE8,   0x877473FA,  0x8E7978F4,  0xB15A49DE,  0xB85742D0,   0xA3405FC2,  0xAA4D54CC,
	0xECDAF741,  0xE5D7FC4F,   0xFEC0E15D,  0xF7CDEA53,  0xC8EEDB79,  0xC1E3D077,   0xDAF4CD65,  0xD3F9C66B,
	0xA4B2AF31,  0xADBFA43F,   0xB6A8B92D,  0xBFA5B223,  0x80868309,  0x898B8807,   0x929C9515,  0x9B919E1B,
	0x7C0A47A1,  0x75074CAF,   0x6E1051BD,  0x671D5AB3,  0x583E6B99,  0x51336097,   0x4A247D85,  0x4329768B,
	0x34621FD1,  0x3D6F14DF,   0x267809CD,  0x2F7502C3,  0x105633E9,  0x195B38E7,   0x024C25F5,  0x0B412EFB,
	0xD7618C9A,  0xDE6C8794,   0xC57B9A86,  0xCC769188,  0xF355A0A2,  0xFA58ABAC,   0xE14FB6BE,  0xE842BDB0,
	0x9F09D4EA,  0x9604DFE4,   0x8D13C2F6,  0x841EC9F8,  0xBB3DF8D2,  0xB230F3DC,   0xA927EECE,  0xA02AE5C0,
	0x47B13C7A,  0x4EBC3774,   0x55AB2A66,  0x5CA62168,  0x63851042,  0x6A881B4C,   0x719F065E,  0x78920D50,
	0x0FD9640A,  0x06D46F04,   0x1DC37216,  0x14CE7918,  0x2BED4832,  0x22E0433C,   0x39F75E2E,  0x30FA5520,
	0x9AB701EC,  0x93BA0AE2,   0x88AD17F0,  0x81A01CFE,  0xBE832DD4,  0xB78E26DA,   0xAC993BC8,  0xA59430C6,
	0xD2DF599C,  0xDBD25292,   0xC0C54F80,  0xC9C8448E,  0xF6EB75A4,  0xFFE67EAA,   0xE4F163B8,  0xEDFC68B6,
	0x0A67B10C,  0x036ABA02,   0x187DA710,  0x1170AC1E,  0x2E539D34,  0x275E963A,   0x3C498B28,  0x35448026,
	0x420FE97C,  0x4B02E272,   0x5015FF60,  0x5918F46E,  0x663BC544,  0x6F36CE4A,   0x7421D358,  0x7D2CD856,
	0xA10C7A37,  0xA8017139,   0xB3166C2B,  0xBA1B6725,  0x8538560F,  0x8C355D01,   0x97224013,  0x9E2F4B1D,
	0xE9642247,  0xE0692949,   0xFB7E345B,  0xF2733F55,  0xCD500E7F,  0xC45D0571,   0xDF4A1863,  0xD647136D,
	0x31DCCAD7,  0x38D1C1D9,   0x23C6DCCB,  0x2ACBD7C5,  0x15E8E6EF,  0x1CE5EDE1,   0x07F2F0F3,  0x0EFFFBFD,
	0x79B492A7,  0x70B999A9,   0x6BAE84BB,  0x62A38FB5,  0x5D80BE9F,  0x548DB591,   0x4F9AA883,  0x4697A38D,
};

static uint16_t
ptp_mtpz_validatehandshakeresponse (PTPParams* params, unsigned char *random, unsigned char **calculatedHash)
{
	uint16_t ret;
	unsigned int len;
	unsigned char* response = NULL;

	ret = ptp_mtpz_getwmdrmpdappresponse (params, &response, &len);
	if (ret != PTP_RC_OK)
	{
		LIBMTP_INFO ("(MTPZ) Failure - did not receive device's response.\n");
		return ret;
	}

	char *reader = (char *)response;
	int i;

	if (*(reader++) != '\x02')
	{
		return -1;
	}

	if (*(reader++) != '\x02')
	{
		return -1;
	}

	// Message is always 128 bytes.
	reader++;
	if (*(reader++) != '\x80')
	{
		return -1;
	}

	char *message = (char *)malloc(128);
	memcpy(message, reader, 128);
	reader += 128;

	// Decrypt the hash-key-message..
	char *msg_dec = (char *)malloc(128);
	memset(msg_dec, 0, 128);

	mtpz_rsa_t *rsa = mtpz_rsa_init(MTPZ_MODULUS, MTPZ_PRIVATE_KEY, MTPZ_PUBLIC_EXPONENT);
	if (!rsa)
	{
		LIBMTP_INFO ("(MTPZ) Failure - could not instantiate RSA object.\n");
		free(message);
		free(msg_dec);
		return -1;
	}

	if (mtpz_rsa_decrypt(128, (unsigned char *)message, 128, (unsigned char *)msg_dec, rsa) == 0)
	{
		LIBMTP_INFO ("(MTPZ) Failure - could not perform RSA decryption.\n");

		free(message);
		free(msg_dec);
		mtpz_rsa_free(rsa);
		return -1;
	}

	mtpz_rsa_free(rsa);
	rsa = NULL;

	char *state = mtpz_hash_init_state();
	char *hash_key = (char *)malloc(16);
	char *v10 = mtpz_hash_custom6A5DC(state, msg_dec + 21, 107, 20);

	for (i = 0; i < 20; i++)
		msg_dec[i + 1] ^= v10[i];

	char *v11 = mtpz_hash_custom6A5DC(state, msg_dec + 1, 20, 107);

	for (i = 0; i < 107; i++)
		msg_dec[i + 21] ^= v11[i];

	memcpy(hash_key, msg_dec + 112, 16);

	// Encrypted message is 0x340 bytes.
	reader += 2;
	if (*(reader++) != '\x03' || *(reader++) != '\x40')
	{
		free(v10);
		free(v11);
		free(hash_key);
		return -1;
	}

	unsigned char *act_msg = (unsigned char *)malloc(832);
	unsigned char *act_reader = act_msg;
	memcpy(act_msg, reader, 832);
	reader = NULL;

	mtpz_encryption_cipher_advanced((unsigned char *)hash_key, 16, act_msg, 832, 0);

	act_reader++;
	unsigned int certs_length = MTPZ_SWAP(*(unsigned int *)(act_reader));
	act_reader += 4;
	act_reader += certs_length;

	unsigned int rand_length = MTPZ_SWAP(*(unsigned short *)(act_reader) << 16);
	act_reader += 2;
	unsigned char *rand_data = (unsigned char *)malloc(rand_length);
	memcpy(rand_data, act_reader, rand_length);
	if (memcmp(rand_data, random, 16) != 0)
	{
		free(rand_data);
		free(hash_key);
		free(v10);
		free(v11);
		return -1;
	}
	free(rand_data);
	act_reader += rand_length;

	unsigned int dev_rand_length = MTPZ_SWAP(*(unsigned short *)(act_reader) << 16);
	act_reader += 2;
	act_reader += dev_rand_length;

	act_reader++;

	unsigned int sig_length = MTPZ_SWAP(*(unsigned short *)(act_reader) << 16);
	act_reader += 2;
	act_reader += sig_length;

	act_reader++;

	unsigned int machash_length = MTPZ_SWAP(*(unsigned short *)(act_reader) << 16);
	act_reader += 2;
	unsigned char *machash_data = (unsigned char *)malloc(machash_length);
	memcpy(machash_data, act_reader, machash_length);
	act_reader += machash_length;

	*calculatedHash = machash_data;

	free(message);
	free(msg_dec);
	free(state);
	free(v10);
	free(v11);
	free(act_msg);
	free(hash_key);

	return ret;
}

static uint16_t
ptp_mtpz_opensecuresyncsession (PTPParams* params, unsigned char *hash)
{
	unsigned char	mch[16];
	uint32_t	*hashparams = (unsigned int *)mch;
	unsigned int	macCount = *(unsigned int *)(hash + 16);
	uint16_t	ret;

	mtpz_encryption_encrypt_mac(hash, 16, (unsigned char *)(&macCount), 4, mch);

	ret = ptp_mtpz_wmdrmpd_enabletrustedfilesoperations(params,
		MTPZ_SWAP(hashparams[0]), MTPZ_SWAP(hashparams[1]),
		MTPZ_SWAP(hashparams[2]), MTPZ_SWAP(hashparams[3]));
	return ret;
};

static unsigned char *
ptp_mtpz_makeapplicationcertificatemessage (unsigned int *out_len, unsigned char **out_random)
{
	*out_len = 785;

	unsigned char *acm = (unsigned char *)malloc(785);
	unsigned char *target = acm;
	memset(acm, 0, 785);

	unsigned char *random = (unsigned char *)malloc(16);

	int i = 0;
	int certsLength = 0x275;

	// Write the marker bytes, length of certificates, and certificates themselves.
	*(target++) = '\x02';
	*(target++) = '\x01';
	*(target++) = '\x01';
	*(target++) = '\x00';
	*(target++) = '\x00';
	*(target++) = '\x02';
	*(target++) = '\x75';
	memcpy(target, MTPZ_CERTIFICATES, certsLength);
	target += certsLength;

	// Write the random bytes.
	*(target++) = '\x00';	*(target++) = '\x10';
	srand(time(NULL));

	for (i = 0; i < 16; i++)
		*(random + i) = (unsigned char)(rand() % 256);

	*out_random = random;
	memcpy(target, random, 16);
	target += 16;

	char *state = mtpz_hash_init_state();
	char *v16 = (char *)malloc(28); memset(v16, 0, 28);
	char *hash = (char *)malloc(20); memset(hash, 0, 20);
	char *odata = (char *)malloc(128); memset(odata, 0, 128);

	mtpz_hash_reset_state(state);
	mtpz_hash_transform_hash(state, (char *)acm + 2, (target - acm - 2));
	mtpz_hash_finalize_hash(state, v16 + 8);

	mtpz_hash_reset_state(state);
	mtpz_hash_transform_hash(state, v16, 28);
	mtpz_hash_finalize_hash(state, hash);

	char *v17 = mtpz_hash_custom6A5DC(state, hash, 20, 107);

	for (i = 0; i < 20; i++)
		odata[107 + i] = hash[i];

	odata[106] = '\x01';

	if (v17 != NULL)
	{
		for (i = 0; i < 107; i++)
			odata[i] ^= v17[i];

		odata[0] &= 127;
		odata[127] = 188;
	}

	// Free up some jazz.
	free(state); state = NULL;
	free(v16); v16 = NULL;
	free(v17); v17 = NULL;
	free(hash); hash = NULL;

	// Take care of some RSA jazz.
	mtpz_rsa_t *rsa = mtpz_rsa_init(MTPZ_MODULUS, MTPZ_PRIVATE_KEY, MTPZ_PUBLIC_EXPONENT);
	if (!rsa)
	{
		LIBMTP_INFO("(MTPZ) Failure - could not instantiate RSA object.\n");
		*out_len = 0;
		free(odata);
		return NULL;
	}

	char *signature = (char *)malloc(128);
	memset(signature, 0, 128);
	mtpz_rsa_sign(128, (unsigned char *)odata, 128, (unsigned char *)signature, rsa);

	// Free some more things.
	mtpz_rsa_free(rsa); rsa = NULL;
	free(odata); odata = NULL;

	// Write the signature + bytes.
	*(target++) = '\x01'; *(target++) = '\x00'; *(target++) = '\x80';
	memcpy(target, signature, 128);

	// Kill target.
	target = NULL;

	return acm;
};

static unsigned char *
ptp_mtpz_makeconfirmationmessage (unsigned char *hash, unsigned int *out_len)
{
	*out_len = 20;
	unsigned char *message = (unsigned char *)malloc(20);
	message[0] = (unsigned char)0x02;
	message[1] = (unsigned char)0x03;
	message[2] = (unsigned char)0x00;
	message[3] = (unsigned char)0x10;

	unsigned char *seed = (unsigned char *)malloc(16);
	memset(seed, 0, 16);
	seed[15] = (unsigned char)(0x01);

	mtpz_encryption_encrypt_mac(hash, 16u, seed, 16u, message + 4);

	free(seed);

	return message;
}

uint16_t ptp_mtpz_handshake (PTPParams* params)
{
	uint16_t ret = PTP_RC_OK;
	uint32_t size;
	unsigned char *hash=NULL;
	unsigned char *random=NULL;
	PTPPropertyValue propval;
	unsigned char*	applicationCertificateMessage;
	unsigned char*	message;

	/* FIXME: do other places of libmtp set it? should we set it? */
	LIBMTP_INFO ("(MTPZ) Setting session initiator info.\n");
	propval.str = "libmtp/Sajid Anwar - MTPZClassDriver";
	ret = ptp_setdevicepropvalue(params,
		   PTP_DPC_MTP_SessionInitiatorInfo,
		   &propval,
		   PTP_DTC_STR);
	if (ret != PTP_RC_OK)
		return ret;

	LIBMTP_INFO ("(MTPZ) Resetting handshake.\n");
	ret = ptp_mtpz_resethandshake(params);
	if (ret != PTP_RC_OK)
		return ret;

	LIBMTP_INFO ("(MTPZ) Sending application certificate message.\n");
	applicationCertificateMessage = ptp_mtpz_makeapplicationcertificatemessage(&size, &random);
	ret = ptp_mtpz_sendwmdrmpdapprequest (params, applicationCertificateMessage, size);
	free (applicationCertificateMessage);
	if (ret != PTP_RC_OK)
		return ret;

	LIBMTP_INFO ("(MTPZ) Getting and validating handshake response.\n");
	ret = ptp_mtpz_validatehandshakeresponse(params, random, &hash);
	if (ret != PTP_RC_OK)
		goto free_random;

	LIBMTP_INFO ("(MTPZ) Sending confirmation message.\n");
	message = ptp_mtpz_makeconfirmationmessage(hash, &size);
        ret = ptp_mtpz_sendwmdrmpdapprequest (params, message, size);
	if (ret != PTP_RC_OK)
		goto free_hash;
	free (message);

	LIBMTP_INFO ("(MTPZ) Opening secure sync session.\n");
	ret = ptp_mtpz_opensecuresyncsession(params, hash);
free_hash:
	free(hash);
free_random:
	free(random);
	return ret;
}
