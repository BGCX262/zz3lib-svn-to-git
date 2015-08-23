// zz3zip.cc -- sample utility to convert zz3 compressed files to uncompressed files
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
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdlib.h>
#include <string>
#include <iostream>
#include "zz3.h"
using std::string;
using std::cout;
using std::endl;

void zz3zip(const string &ipath, const string &opath, uint32_t blocksize, int effort)
{
    char buff[32768];

    FILE *ifp = (ipath == "-") ? stdin : fopen(ipath.c_str(), "r");
    ZZ3FILE *ofp = zz3_open(opath.c_str(), "w", blocksize, effort);
    while (1) {
        size_t len = fread(buff, 1, sizeof(buff), ifp);
        if (len == 0) break;
        zz3_write(ofp, buff, len);
    }
    zz3_close(ofp);
    if (ipath != "-") fclose(ifp);
}

int main(int argc, char *argv[])
{
    uint32_t blocksize = 8192;
    int effort = 6;
    for (int i = 1; i < argc; ++i) {
        if (0 == strncmp(argv[i], "--blocksize", strlen(argv[i])) && strlen(argv[i]) >= 3 && (i + 1) < argc) {
            blocksize = static_cast<uint32_t>(atoi(argv[++i]));
        } else if (0 == strncmp(argv[i], "--effort", strlen(argv[i])) && strlen(argv[i]) >= 3 && (i + 1) < argc) {
            effort = atoi(argv[++i]);
        } else if (0 == strcmp("-", argv[i]) && (i + 1) < argc) {
            string ipath(argv[i]);
            string opath(argv[++i]);
            zz3zip(ipath, opath, blocksize, effort);
        } else {
            string ipath(argv[i]);

            // get access and modification times from sourcefile
            struct stat statbuf;
            if (0 != stat(ipath.c_str(), &statbuf)) {
                perror(ipath.c_str());
                exit(1);
            }
            struct timeval src_times[2];
            TIMESPEC_TO_TIMEVAL(&src_times[0], &statbuf.st_atim);
            TIMESPEC_TO_TIMEVAL(&src_times[1], &statbuf.st_mtim);

            string opath(ipath);
            opath += ".zz3";
            zz3zip(ipath, opath, blocksize, effort);
            // copy access & modification times from sourcefile
            if (0 != utimes(opath.c_str(), src_times)) {
                perror(opath.c_str());
                exit(1);
            }
        }
    }
    exit(0);
}
