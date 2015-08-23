// zz3unzip.cc -- sample utility to convert uncompressed files to zz3 compressed files
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
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <assert.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <string>
#include <iostream>
#include "zz3.h"
using std::string;
using std::cout;
using std::endl;

bool endswith(const char *name, const char *sfx)
{
    size_t len1 = strlen(name);
    size_t len2 = strlen(sfx);
    if (len1 <= len2) return false;
    return 0 == strcmp(name + len1 - len2, sfx);
}

void zz3unzip(const char *ipath)
{
    char buff[32768];
    if (!endswith(ipath, ".zz3")) {
        cout << ipath << ": not a zz3 file";
        exit(1);
    }
    string opath(ipath, ipath + strlen(ipath) - 4);

    // get access and modification times from sourcefile
    struct stat statbuf;
    if (0 != stat(ipath, &statbuf)) {
        perror(ipath);
        exit(1);
    }
    struct timeval src_times[2];
    TIMESPEC_TO_TIMEVAL(&src_times[0], &statbuf.st_atim);
    TIMESPEC_TO_TIMEVAL(&src_times[1], &statbuf.st_mtim);

    cout << "Decompressing " << ipath << "..." << endl;
    ZZ3FILE *ifp = zz3_open(ipath, "r");
    FILE *ofp = fopen(opath.c_str(), "w");
    while (1) {
        size_t len = zz3_read(ifp, buff, sizeof(buff));
        if (len == 0) break;
        size_t len2 = fwrite(buff, 1, len, ofp);
        assert(len == len2);
    }
    fclose(ofp);
    zz3_close(ifp);
    // copy access & modification times from sourcefile to destfile
    if (0 != utimes(opath.c_str(), src_times)) {
        perror(opath.c_str());
        exit(1);
    }
}

int main(int argc, char *argv[])
{
    for (int i = 1; i < argc; ++i) {
        zz3unzip(argv[i]);
    }
    cout << "Done." << endl;
    exit(0);
}
