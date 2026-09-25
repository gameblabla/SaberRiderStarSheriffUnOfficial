/*
 * adpencode.cpp
 *
 * Copyright (C) 2021 celeriyacon - https://github.com/celeriyacon
 *
 * This software is provided 'as-is', without any express or implied
 * warranty.  In no event will the authors be held liable for any damages
 * arising from the use of this software.
 *
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 *
 * 1. The origin of this software must not be misrepresented; you must not
 *    claim that you wrote the original software. If you use this software
 *    in a product, an acknowledgment in the product documentation would be
 *    appreciated but is not required.
 * 2. Altered source versions must be plainly marked as such, and must not be
 *    misrepresented as being the original software.
 * 3. This notice may not be removed or altered from any source distribution.
 *
 */

#include <stdio.h>
#include <inttypes.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <assert.h>
#include <errno.h>
#include <sndfile.h>
#include <algorithm>
#include <vector>

#include "types.h"

#include "tables.h"

enum : unsigned { max_samples_per_block = 16 };
enum : unsigned { max_bytes_per_block = 9 };

struct ADContext
{
 unsigned samples_per_block;
 unsigned samples_per_byte;
 unsigned bits_per_sample;
 unsigned encoded_byte_xor;
 int32 samps[3];
};

static inline int32 CalcPredicted(ADContext* const c, const uint8 pwhich)
{
 int32 predicted = 0;

 for(unsigned j = 0; j < 3; j++)
 {
  predicted += ((int64)c->samps[j] * filter_tab[pwhich][j]) >> 12;
  predicted = sign_extend(26, predicted);
 }

 return predicted;
}

static inline int32 DecodeSample(ADContext* const c, const uint8 shift, const int32 predicted, unsigned encoded)
{
 const int32 scale = scale_tab[shift];
 int32 ret = predicted;

 if(c->bits_per_sample == 1)
 {
  const int tab[2] = { 3, -3 };

  ret += ((int64)tab[encoded] * 524288 * scale) >> 12;
 }
 else if(c->bits_per_sample == 2)
 {
  const int tab[4] = { 3, 7, -7, -3 };

  ret += ((int64)tab[encoded] * 524288 * scale) >> 12;
 }
 else
  ret += ((int64)sign_extend(4, encoded) * 524288 * scale) >> 12;

 ret = sign_extend(26, ret) << 1;

 return ret;
}

static inline void PushSampleHistory(ADContext* const c, int32 decoded)
{
 c->samps[2] = c->samps[1];
 c->samps[1] = c->samps[0];
 c->samps[0] = decoded;
}

static inline void TestEncodeBlock(ADContext* c, uint8 pwhich, uint8 shift, const int16* input, int16* output, uint8* nibbles_output)
{
 for(unsigned i = 0; i < c->samples_per_block; i++)
 {
  const int32 predicted = CalcPredicted(c, pwhich);;
  int32 min_error = 0x7FFFFFFF;
  uint32 gonib = 0;
  int32 gopt = 0;

  for(unsigned trynib = 0x0; trynib < (1U << c->bits_per_sample); trynib++)
  {
   int32 pt = DecodeSample(c, shift, predicted, trynib);
   const int32 error = abs(input[i] - (pt >> 8));

   if(error < min_error)
   {
    gonib = trynib;
    gopt = pt;
    min_error = error;
   }
  }

  PushSampleHistory(c, gopt);

  nibbles_output[i] = gonib;
  output[i] = gopt >> 8;  
 }
}

static inline void DecodeBlock(ADContext* const c, const uint8 header, uint8* const encoded_samples, int16* output)
{
 const uint8 shift = header & 0xF;
 const uint8 pw = header >> 4;

 for(unsigned i = 0; i < c->samples_per_block; i++)
 {
  const unsigned encoded = ((encoded_samples[(i / c->samples_per_byte)] ^ c->encoded_byte_xor) >> ((i % c->samples_per_byte) * c->bits_per_sample)) & ((1U << c->bits_per_sample) - 1);
  const int32 predicted = CalcPredicted(c, pw);
  const int32 decoded = DecodeSample(c, shift, predicted, encoded);

  PushSampleHistory(c, decoded);

  if(output)
   output[i] = decoded >> 8;
 }
}

static inline int32 EncodeBlock(ADContext* c, const int16* input, int16* output, uint8* block_data, bool is_loop_target)
{
 double max_error = 0x7FFFFFFF;
 unsigned used_pw = 0;
 unsigned used_shift = 0;
 ADContext c_gowith;
 uint8 nibbles[max_samples_per_block];
 unsigned pw_min = 0;
 unsigned pw_bound = FILTER_TAB_COUNT;

 if(is_loop_target)
 {
  pw_min = 0x01;
  pw_bound = pw_min + 1;
  assert(!(filter_tab[pw_min][0] | filter_tab[pw_min][1] | filter_tab[pw_min][2]));
 }

 for(unsigned pw = pw_min; pw < pw_bound; pw++)
 {
  for(unsigned shift = 0; shift < 16; shift++)
  {
   ADContext tmpc = *c;
   double error = 0;
   uint8 nibbles_tmp[max_samples_per_block];
   int16 output_tmp[max_samples_per_block];

   TestEncodeBlock(&tmpc, pw, shift, input, output_tmp, nibbles_tmp);

   for(unsigned i = 0; i < c->samples_per_block; i++)
    error += abs(input[i] - output_tmp[i]) * sqrtf(1.0 + fabsf(i - ((c->samples_per_block - 1) * 0.5)) / 2);

   if(error < max_error)
   {
    memcpy(nibbles, nibbles_tmp, sizeof(nibbles));
    memcpy(output, output_tmp, sizeof(output_tmp));
    max_error = error;
    used_pw = pw;
    used_shift = shift;
    c_gowith = tmpc;
   }
  }
 }

 *c = c_gowith;

 //
 //
 //
 block_data[0] = (used_shift & 0xF) | (used_pw << 4);

 memset(block_data + 1, 0x00, c->samples_per_block / c->samples_per_byte);
 for(unsigned i = 0; i < c->samples_per_block; i++)
  block_data[1 + (i / c->samples_per_byte)] |= nibbles[i] << ((i % c->samples_per_byte) * c->bits_per_sample);

 for(unsigned i = 0; i < c->samples_per_block / c->samples_per_byte; i++)
  block_data[1 + i] ^= c->encoded_byte_xor;

 return max_error;
}

static bool write16(const uint16 value, FILE* fp)
{
 return fputc(value >> 8, fp) != EOF && fputc(value >> 0, fp) != EOF;
}

int main(int argc, char* argv[])
{
 int ret = 0;
 int16 inbuf[max_samples_per_block];
 ADContext c;
 SNDFILE* insf = NULL;
 SF_INFO sfi;
 FILE* outfp = NULL;
 //
 SNDFILE *outsf = NULL;
 uint32 pw_usage[FILTER_TAB_COUNT] = { 0 };
 uint32 shift_usage[16] = { 0 };
 //
 std::vector<uint8> header_data;
 std::vector<uint8> nybble_data;
 uint8 block_data[max_bytes_per_block];
 int16 outbuf[max_samples_per_block];
 int64 total_error = 0;

 if(argc < 4 || argc > 5)
 {
  printf("Invalid number of arguments.\n\n");
  printf("Usage: %s [FORMAT] [INPUT_FILE] [OUTPUT_FILE]\n\n", argv[0]);
  return -1;
 }

 const int format = atoi(argv[1]);

 if(format < 0 || format > 2)
 {
  printf("Unsupported format.\n");
  return -1;
 }

 memset(&sfi, 0, sizeof(sfi));
 if(!(insf = sf_open(argv[2], SFM_READ, &sfi)))
 {
  printf("Error opening \"%s\".\n", argv[2]);
  ret = -1;
  goto cleanup;
 }

 if(!(outfp = fopen(argv[3], "wb")))
 {
  printf("Error opening \"%s\": %m\n", argv[3]);
  ret = -1;
  goto cleanup;
 }

 if(argc >= 5)
 {
  SF_INFO outsfi = sfi;

  outsfi.channels = 2;
  outsfi.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;

  if(!(outsf = sf_open(argv[4], SFM_WRITE, &outsfi)))
  {
   printf("Error opening \"%s\".\n", argv[4]);
   ret = -1;
   goto cleanup;
  }
 }
 //
 //
 //
 int32 input_block_count, input_loop_block;

 memset(&c, 0, sizeof(c));
 c.bits_per_sample = 4 >> format;
 c.samples_per_block = 16;
 c.samples_per_byte = 8 / c.bits_per_sample;
 c.encoded_byte_xor = encoded_byte_xor_tab[format];
 assert(c.samples_per_block <= max_samples_per_block);
 assert((c.samples_per_block / c.samples_per_byte + 1) <= max_bytes_per_block);

 input_block_count = (sfi.frames + c.samples_per_block - 1) / c.samples_per_block;
 input_loop_block = input_block_count;

 {
  const char* comment;

  if((comment = sf_get_string(insf, SF_STR_COMMENT)))
  {
   unsigned input_loop_sample;

   if(sscanf(comment, "adp_loop=%u", &input_loop_sample) == 1)
   {
    if(input_loop_sample % c.samples_per_block)
    {
     printf("Loop point of %u is not on a %u-sample boundary.\n", input_loop_sample, c.samples_per_block);
     ret = -1;
     goto cleanup;
    }
    input_loop_block = input_loop_sample / c.samples_per_block;
   }
  }
 }

 if(1)
 {
  const uint32 max_sample_count = ((0xFFFE - 2) / (c.samples_per_block / c.samples_per_byte) - 1) * c.samples_per_block;

  if((c.samples_per_block * input_block_count) > max_sample_count)
  {
   printf("Input too large.  Maximum size for format %u is %u samples.\n", format, max_sample_count);
   ret = -1;
   goto cleanup;
  }
 }

 for(unsigned i = 0; i < (1 + 1); i++)
  header_data.push_back(0x10);

 for(unsigned i = 0; i < (1 + c.samples_per_block / c.samples_per_byte); i++)
  nybble_data.push_back(0x00);

 DecodeBlock(&c, header_data[1], &nybble_data[1], NULL);

 assert((!c.samps[0] && !c.samps[1] && !c.samps[2]) || (c.bits_per_sample < 4));

 for(int32 block_counter = 0; block_counter < input_block_count; block_counter++)
 {
  sf_count_t incount = sf_read_short(insf, inbuf, c.samples_per_block);

  if(incount <= 0)
   break;

  for(unsigned i = incount; i < c.samples_per_block; i++)
   inbuf[i] = inbuf[incount - 1];

  total_error += EncodeBlock(&c, inbuf, outbuf, block_data, block_counter == input_loop_block);

  header_data.push_back(block_data[0]);
  for(unsigned j = 0; j < (c.samples_per_block / c.samples_per_byte); j++)
   nybble_data.push_back(block_data[1 + j]);
  //
  //
  //
  pw_usage[block_data[0] >> 4]++;
  shift_usage[block_data[0] & 0xF]++;

  if(outsf)
  {
   int16 oebuf[max_samples_per_block * 2];

   for(unsigned i = 0; i < c.samples_per_block; i++)
   {
    oebuf[(i << 1) + 0] = outbuf[i];
    oebuf[(i << 1) + 1] = std::max<int>(-32768, std::min<int32>(32767, inbuf[i] - outbuf[i]));
   }
   sf_write_short(outsf, oebuf, c.samples_per_block * 2);
  }
 }

 //
 // Make sure last byte is 0x00(min shift, filter that will decay to 0), as we'll infinitely loop playback
 // on it instead of allowing the waveform to end normally, to work around an SCSP bug.
 //
 assert(nybble_data.front() == 0x00);
 nybble_data.push_back(0x00);

 write16(1 + input_block_count, outfp);
 write16(1 + input_loop_block, outfp);
 fputc(format, outfp);
 fwrite(header_data.data(), 1, header_data.size(), outfp);
 fwrite(nybble_data.data(), 1, nybble_data.size(), outfp);

 if(ftell(outfp) & 1)
  fputc(0x00, outfp);

 printf("Total Error: %16lld\n", (long long)total_error);

 for(unsigned i = 0; i < FILTER_TAB_COUNT; i++)
 {
  printf("/* 0x%08x 0x%02x */ { % 6d, % 6d, % 6d }, \n", pw_usage[i], i, filter_tab[i][0], filter_tab[i][1], filter_tab[i][2]);
 }

 for(unsigned i = 0; i < 16; i++)
 {
  printf("Shift 0x%02x: %10u\n", i, shift_usage[i]);
 }
 //
 //
 //
 cleanup:;
 if(outfp)
 {
  fclose(outfp);
  outfp = NULL;
 }

 if(insf)
 {
  sf_close(insf);
  insf = NULL;
 }

 if(outsf)
 {
  sf_close(outsf);
  outsf = NULL;
 }

 return ret;
}
