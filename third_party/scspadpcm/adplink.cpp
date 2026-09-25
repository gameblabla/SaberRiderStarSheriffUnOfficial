/*
 * adplink.cpp
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
#include <stdlib.h>
#include <inttypes.h>
#include <string.h>
#include <ctype.h>
#include <vector>

#include "types.h"

const unsigned driver_size = 0x2000;

bool write32(FILE* fp, uint32 v)
{
 const uint8 tmp[4] = { (uint8)(v >> 24), (uint8)(v >> 16), (uint8)(v >> 8), (uint8)v };

 return fwrite(tmp, 1, 4, fp) == sizeof(tmp);
}

static void print_id(const char* path, unsigned n)
{
 const char* a = strrchr(path, '/');
 const char* b = strrchr(path, '\\');

 if((a && b && b > a) || !a)
  a = b;

 if(a)
  a++;
 else
  a = path;

 const char* d = strrchr(a, '.');

 if(!d)
  d = a + strlen(a);

 printf("#define SAMPLE_ID_");
 while(a != d)
 {
  char c = toupper(*a);

  if(!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')))
   c = '_';

  fputc(c, stdout);
  a++;
 }
 printf(" %u\n", n);
}

int main(int argc, char* argv[])
{
 int ret = 0;
 FILE* fp = NULL;
 FILE* ofp = NULL;
 std::vector<uint32> table;

 table.resize(argc - 2);

 if(!(ofp = fopen(argv[1], "wb")))
 {
  fprintf(stderr, "Error opening \"%s\": %m\n", argv[1]);
  ret = -1;
  goto cleanup;
 }

 for(unsigned i = 0; i < table.size(); i++)
 {
  if(!write32(ofp, table[i]))
  {
   fprintf(stderr, "Error writing to file: %m\n");
   ret = -1;
   goto cleanup;
  }
 }

 for(int i = 2; i < argc; i++)
 {
  int c;
  long fpos;

  if(!(fp = fopen(argv[i], "rb")))
  {
   fprintf(stderr, "Error opening \"%s\": %m\n", argv[i]);
   ret = -1;
   goto cleanup;
  }

  print_id(argv[i], i - 2);

  if((fpos = ftell(ofp)) == -1)
  {
   fprintf(stderr, "Error getting position in file: %m\n");
   ret = -1;
   goto cleanup;
  }
  table[i - 2] = driver_size + fpos;

  while((c = fgetc(fp)) >= 0)
  {
   if(fputc(c, ofp) == EOF)
   {
    fprintf(stderr, "Error writing to file: %m\n");
    ret = -1;
    goto cleanup;
   }
  }

  if((fpos = ftell(ofp)) == -1)
  {
   fprintf(stderr, "Error getting position in file: %m\n");
   ret = -1;
   goto cleanup;
  }  
  if(fpos & 1)
  {
   if(fputc(0x00, ofp) == EOF)
   {
    fprintf(stderr, "Error writing to file: %m\n");
    ret = -1;
    goto cleanup;
   }
  }

  fclose(fp);
  fp = NULL;
 }

 if(fseek(ofp, 0, SEEK_SET) == -1)
 {
  fprintf(stderr, "Error seeking in file: %m\n");
  ret = -1;
  goto cleanup;
 }

 for(unsigned i = 0; i < table.size(); i++)
 {
  if(!write32(ofp, table[i]))
  {
   fprintf(stderr, "Error writing to file: %m\n");
   ret = -1;
   goto cleanup;
  }
 }
 //
 //
 //
 cleanup:;
 if(fp)
 {
  fclose(fp);
  fp = NULL;
 }

 if(ofp)
 {
  if(fclose(ofp) == EOF)
  {
   fprintf(stderr, "Error closing file: %m\n");
   ret = -1;
  }
  ofp = NULL;
 }

 return ret;
}
