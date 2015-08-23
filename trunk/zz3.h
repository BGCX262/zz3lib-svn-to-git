// zz3.h -- interface of the 'zz3' ssekable compressed files library
//
// Copyright (C) 2015 Rudy Albachten rudy@albachten.com
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
#ifndef ZZ3_LIB_INCLUDED
#define ZZ3_LIB_INCLUDED
#include <stdint.h>
#include <string>
#include <stdexcept>

class zz3error : public std::runtime_error {
  public:
    zz3error(const char *msg) : std::runtime_error(msg) {}
    zz3error(const std::string &msg) : std::runtime_error(msg.c_str()) {}
};

typedef struct _zz3file ZZ3FILE;

ZZ3FILE *zz3_open(const char *path, const char *mode);
ZZ3FILE *zz3_open(const char *path, const char *mode, uint32_t blocksize, int effort);
void zz3_flush(ZZ3FILE *zf);
void zz3_close(ZZ3FILE *zf);

std::string zz3_pathname(ZZ3FILE *zf);
int zz3_effort(ZZ3FILE *zf);
uint32_t zz3_blocksize(ZZ3FILE *zf);
void zz3_set_effort(ZZ3FILE *zf, int effort);

uint64_t zz3_length(ZZ3FILE *zf);
void zz3_seek(ZZ3FILE *zf, uint64_t pos);
uint64_t zz3_tell(ZZ3FILE *zf);
bool zz3_eof(ZZ3FILE *zf);

int zz3_getc(ZZ3FILE *zf);
int zz3_ungetc(ZZ3FILE *zf, int c);
int zz3_peek(ZZ3FILE *zf);
int zz3_putc(ZZ3FILE *zf, int c);

size_t zz3_read(ZZ3FILE *zf, char *buff, size_t bufflen);
size_t zz3_read(ZZ3FILE *zf, std::string &str, uint64_t maxlen);
size_t zz3_write(ZZ3FILE *zf, const char *buff, size_t bufflen);
size_t zz3_write(ZZ3FILE *zf, const std::string &str);
size_t zz3_puts(ZZ3FILE *zf, const char *buff);
size_t zz3_getline(ZZ3FILE *zf, char *buff, size_t bufflen);
size_t zz3_getline(ZZ3FILE *zf, std::string &line);
size_t zz3_getline(ZZ3FILE *zf, std::string &line, char delim);
size_t zz3_getline(ZZ3FILE *zf, std::string &line, char delim, uint64_t maxlen);

void zz3_showmap(ZZ3FILE *zf);
void zz3_showblocks(ZZ3FILE *zf);

#endif
