// zz3cat.cc -- sample utility to decompress zz3 files to stdout
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
#include <stdio.h>

#include <stdlib.h>
#include <assert.h>
#include <string>
#include <iostream>
#include "zz3.h"
using std::string;
using std::cout;
using std::endl;

void zz3cat(const char *ipath)
{
    char buff[32768];

    ZZ3FILE *ifp = zz3_open(ipath, "r");
    while (1) {
        size_t len = zz3_read(ifp, buff, sizeof(buff));
        if (len == 0) break;
        size_t len2 = fwrite(buff, 1, len, stdout);
        assert(len == len2);
    }
    zz3_close(ifp);
}

int main(int argc, char *argv[])
{
    for (int i = 1; i < argc; ++i) {
        zz3cat(argv[i]);
    }
    exit(0);
}
