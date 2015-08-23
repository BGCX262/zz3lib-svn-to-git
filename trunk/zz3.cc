// zz3.cc -- implementation of the zz3 library
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
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <iomanip>
#include <iostream>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include "zz3.h"
#include <zlib.h>
using std::cout;
using std::endl;
using std::string;

/*
 *  File Format:
 *  ZZ3FILE   : "zz3_X.X\n" SIZE BLOCKSIZE MAPLOC compressed-data MAP
 *  SIZE      : uint64 (0 if map is not present)
 *  BLOCKSIZE : uint32 (0 if map is not present)
 *  MAPLOC    : uint64 (0 if map is not present)
 *  MAP       : 'M' BLOCK* '0' 'F' BLOCK* '0' 'E'
 *  BLOCK     : BLOCK1 | BLOCK2
 *  BLOCK1    : OFFSET LENGTH
 *  BLOCK2    : '1' COUNT (for empty blocks in a sparse file)
 *  OFFSET    : uintx
 *  LENGTH    : uintx
 *  COUNT     : uintx
 *  
 *  uint32    : 32bit unsigned (MSB first)
 *  uint64    : 64bit unsigned (MSB first)
 *  uintx     : variable length unsigned int, bit7 is continuation bit, MSB first, 64bits max
 *  
 * NOTES:
 * seek/tell fpos/flen:
 *   seek/tell can set/report position to anywhere - even beyond flen
 *   read beyond flen returns 0 bytes without error and without moving fpos
 *   write beyond flen implicitly fills intermediate bytes with nuls ('\0') and moves flen (see "holes" below)
 *   only writes can move flen
 *   all writes in append mode reset fpos to flen before writing, reads in "a+" mode use the current fpos
 *   reads and writes can be intermixed without seeking - even though many systems require intervening seeks
 *   seeking within the current block and then reading will not reload the block
 * eof:
 *   return true for eof() whenever the current fpos is at or past the end-of-file position
 *   note that this is different from posix where eof is a flag set whenever an attempt is made to read past eof
 * unget:
 *   only one character is saved (in "pushback")
 *   unget(-1) effectively clears the unget buffer
 *   if unget buffer is anything but -1, the next character read will be char(pushback & 0xFF) without moving fpos
 *   typically the pushed back character matches the previous character in the file - but, even if it doesn't, the file is not changed
 *   "seeks" and "writes" clear the unget buffer
 * "dirty" flag
 *   physical file sets the dirty flag (e.g. it sets the maploc to 0 in the header) when the map is missing or incorrect
 *   new files (modes w/w+) start out "dirty" because they don't have a map on disk yet
 *   existing files (modes r/r+/a/a+) start out "clean" and are marked dirty when a compressed block is updated/added to the physical file
 *   flush() will flush the current write buffer, write the map, set the maploc in the header, and clear the dirty flag 
 * holes:
 *   holes are created by writing with fpos beyond flen
 *   holes are always followed by data - there is no way to create a hole at the end of the file.
 *   zero length writes don't move flen and don't create holes
 *   any block that wasn't filled before seeking may be "short" and has implicit nils to fill the block
 *   when reading compressed data, always pad the block with nils - any nils before flen are part of a hole and any past flen will be ignored
 * when reading:
 *   the buffer contains the entire block and ptr1 can be moved at will
 *   ptr1 always points to the next character to read (buffer.begin() <= ptr1 <= buffer.end())
 *   ptr2 == buffer.end()
 * when writing:
 *   only the characters in the range [ptr1, ptr2) are valid in the buffer
 *   ptr2 points at the next character location for writing
 *   when the buffer is flushed we may have to read data before ptr1 or after ptr2 and merge with the new data
 *   after flushing a block, the buffer contains the whole block and can be used for reading that block
 *   when switching from read to write we can move ptr1 to buffer.begin() so we don't have to reread the data before ptr1 on flush
*/

namespace {

const char    *MAGIC_STRING = "zz3_1.0\n";
const uint32_t DEFAULT_BLOCKSIZE = 8096;
const int      DEFAULT_EFFORT = 1;

typedef std::vector<unsigned char> CharBuff;
typedef std::vector<unsigned char>::iterator BuffIter;
typedef std::vector<unsigned char>::const_iterator ConstBuffIter;

struct Block {
    Block() : cpos(0), clen(0) {}
    Block(uint64_t pos, uint32_t len) : cpos(pos), clen(len) {}
    bool empty() const {return cpos == 0 && clen == 0;}
    uint64_t cpos;
    uint32_t clen;
};

struct CompareBySize {
    bool operator()(const Block &lhs, const Block &rhs) {
        return (lhs.clen < rhs.clen) || (lhs.clen == rhs.clen && lhs.cpos < rhs.cpos);
    }
};

struct CompareByPos {
    bool operator()(const Block *lhs, const Block *rhs) {
        return lhs->cpos < rhs->cpos;
    }
};

class noncopyable {
  public:
    noncopyable() {}
  private:
    noncopyable(const noncopyable &); // not-implemented
    noncopyable& operator=(const noncopyable &); // not-implemented
};

class Blockmap : noncopyable {
  public:
    Blockmap() {}
    Block &get(size_t i) {
        if (blocks.size() <= i) blocks.resize(i + 1);
        return blocks[i];
    }
    Block &operator[](size_t i) {
        return blocks.at(i);
    }
    size_t size() const {
        return blocks.size();
    }
    void show() const;
    void write(FILE *fp);
    void read(FILE *fp);
    void add_free(Block block);
    bool get_free(Block &block);
    void clear() {
        blocks.clear();
        freeblocks_by_position.clear();
        freeblocks_by_size.clear();
    }
  private:
    typedef std::set<Block,  CompareBySize> FreeBlocksBySize;
    typedef std::set<const Block*, CompareByPos>  FreeBlocksByPos;
    std::vector<Block> blocks;
    FreeBlocksBySize freeblocks_by_size;
    FreeBlocksByPos freeblocks_by_position;
};
} // anonymous namespace

struct _zz3file {
    string   pathname;
    uint32_t blocksize;
    int      effort;

    FILE    *fh;
    bool     readable;
    bool     writeable;
    bool     reading;
    bool     writing;
    bool     append;
    bool     dirty;
    int      pushback;

    uint64_t fpos;
    uint64_t flen;
    uint64_t maploc;

    Blockmap blockmap;
    size_t blocknum;
    CharBuff buffer;
    CharBuff cbuffer;
    BuffIter ptr1;
    BuffIter ptr2;
};

namespace {

////////////////////////////////////////////////////////////////////////////////////////////////////////////
// General i/o & misc functions

void write_u1(FILE *fp, unsigned char x)
{
    if (fwrite(&x, 1, 1, fp) != 1) throw zz3error("io error");
}

void write_u32(FILE *fp, uint32_t x)
{
    unsigned i = 4;
    do {
        char c = static_cast<char>((x >> (--i * 8)) & 0xff);
        if (fwrite(&c, 1, 1, fp) != 1) throw zz3error("io error");
    } while (i);
}

void write_u64(FILE *fp, uint64_t x)
{
    unsigned i = 8;
    do {
        char c = static_cast<char>((x >> (--i * 8)) & 0xff);
        if (fwrite(&c, 1, 1, fp) != 1) throw zz3error("io error");
    } while (i);
}

void write_u(FILE *fp, uint64_t x)
{
    do {
        unsigned char bits = 0x7F & x;
        if (x >>= 7) bits |= 0x80;
        write_u1(fp, bits);
    } while (x);
}

void write_string(FILE *fp, const char *x)
{
    size_t len = strlen(x);
    if (fwrite(x, 1, len, fp) != len) throw zz3error("io error");
}

unsigned char read_u1(FILE *fp)
{
    unsigned char result;
    if (fread(&result, 1, 1, fp) != 1) throw zz3error("io error");
    return result;
}

uint32_t read_u32(FILE *fp)
{
    uint32_t result = 0;
    unsigned char x;
    unsigned i = 4;
    do {
        if (fread(&x, 1, 1, fp) != 1) throw zz3error("io error");
        result |= (uint32_t(x) << (8 * --i));
    } while (i);
    return result;
}

uint64_t read_u64(FILE *fp)
{
    uint64_t result = 0;
    unsigned char x;
    unsigned i = 8;
    do {
        if (fread(&x, 1, 1, fp) != 1) throw zz3error("io error");
        result |= (uint64_t(x) << (8 * --i));
    } while (i);
    return result;
}

uint64_t read_u(FILE *fp)
{
    uint64_t result = 0;
    unsigned char x;
    unsigned shift = 0;
    do {
        x = read_u1(fp);
        result |= static_cast<uint64_t>(x & 0x7f) << shift;
        shift += 7;
    } while (x & 0x80);
    return result;
}

void read_string(FILE *fp, char *s, size_t len)
{
    if (fread(s, 1, len, fp) != len) throw zz3error("io error");
}

void expect_string(FILE *fp, const char *s)
{
    char buff[64];
    size_t len = strlen(s);
    assert(len < sizeof(buff));
    read_string(fp, buff, len);
    if (0 != strncmp(buff, s, len)) throw zz3error("format_error");
}


void showbuffer(const CharBuff &buffer)
{
    for (size_t i = 0; i < buffer.size(); ++i) {
        if (isprint(buffer[i])) cout << buffer[i];
        else cout << ".";
    }
    cout << "\n";
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Blockmap methods

void Blockmap::show() const
{
    cout << "BLOCKMAP:\n";
    for (size_t i = 0; i < blocks.size(); ++i) {
        if (blocks[i].cpos == 0 and blocks[i].clen == 0) {
            cout << "block[" << i << "] EMPTY\n";
        } else {
            uint64_t endpos = blocks[i].cpos + blocks[i].clen;
            cout << "block[" << i << "] pos=" << blocks[i].cpos << " (0x" << std::hex << blocks[i].cpos << std::dec << ")"
                                   << " end=" << endpos         << " (0x" << std::hex << endpos         << std::dec << ")"
                                   << " len=" << blocks[i].clen << " (0x" << std::hex << blocks[i].clen << std::dec << ")\n";
        }
        size_t j = i;
        while (true) {
            if (j + 1 >= blocks.size()) break;
            if (blocks[j + 1].cpos != 0) break;
            if (blocks[j + 1].clen != 0) break;
            ++j;
        }
        if (i != j) cout << "* " << (j - i) << " more empty blocks\n";
        i = j;
    }
    FreeBlocksByPos::const_iterator iter = freeblocks_by_position.begin();
    FreeBlocksByPos::const_iterator end  = freeblocks_by_position.end();
    for ( ; iter != end; ++iter) {
        const Block *block = *iter;
        cout << "freeblock = (" << block->cpos << ", " << block->clen << ")\n";
    }
}

void Blockmap::write(FILE *fp)
{
    write_string(fp, "M");
    size_t holes = 0;
    for (size_t i = 0; i < blocks.size(); ++i) {
        const Block &block = blocks[i];
        if (block.cpos == 0 && block.clen == 0) {
            ++holes;
        } else {
            if (holes) {
                write_u(fp, 1);
                write_u(fp, holes);
                holes = 0;
            }
            assert(block.cpos != 0);
            assert(block.clen != 0);
            write_u(fp, block.cpos);
            write_u(fp, block.clen);
        }
    }
    assert(!holes);
    write_u(fp, 0);
    write_string(fp, "F");

    FreeBlocksBySize::const_iterator iter = freeblocks_by_size.begin();
    FreeBlocksBySize::const_iterator end  = freeblocks_by_size.end();
    for ( ; iter != end; ++iter) {
        const Block &block = *iter;
        assert(block.cpos != 0);
        write_u(fp, block.cpos);
        write_u(fp, block.clen);
    }
    write_u(fp, 0);
    write_string(fp, "E");
}

void Blockmap::read(FILE *fp)
{
    blocks.clear();
    freeblocks_by_position.clear();
    freeblocks_by_size.clear();
    expect_string(fp, "M");
    size_t i = 0;
    while (true) {
        uint64_t pos = read_u(fp);
        uint64_t len = 0;
        if (pos == 0) break;
        len = read_u(fp);
        assert(len <= std::numeric_limits<uint32_t>::max());
        if (pos == 1) {
            i += len;
            // the blocks will get created with pos = len = 0 when we create the next non-hole block
        } else {
            Block &block = this->get(i++);
            block.cpos = pos;
            block.clen = static_cast<uint32_t>(len);
        }
    }
    expect_string(fp, "F");
    Block block;
    for (size_t i = 0; ; ++i) {
        block.cpos = read_u(fp);
        if (block.cpos == 0) break;
        assert(block.cpos != 1);
        block.clen = static_cast<uint32_t>(read_u(fp));
        add_free(block);
    }
    expect_string(fp, "E");
}

void Blockmap::add_free(Block block)
{
    if (!block.clen) return;

    const Block *prev = NULL;
    const Block *next = NULL;
    if (!freeblocks_by_position.empty()) {
        FreeBlocksByPos::iterator iter = freeblocks_by_position.lower_bound(&block);
        if (iter != freeblocks_by_position.end()) next = *iter;
        if (iter != freeblocks_by_position.begin()) prev = *--iter;
    }
    assert(!prev || (prev->cpos + prev->clen) <= block.cpos);
    if (prev && (prev->cpos + prev->clen) == block.cpos) {
        // coallesce with previous
        block.cpos = prev->cpos;
        block.clen += prev->clen;
        freeblocks_by_position.erase(prev);
        freeblocks_by_size.erase(*prev);
    }
    assert(!next || (block.cpos + block.clen) <= next->cpos);
    if (next && (block.cpos + block.clen) == next->cpos) {
        // coallesce with next
        block.clen += next->clen;
        freeblocks_by_position.erase(next);
        freeblocks_by_size.erase(*next);
    }

    std::pair<FreeBlocksBySize::iterator, bool> result1 = freeblocks_by_size.insert(block);
    assert(result1.second);
    freeblocks_by_position.insert(&*result1.first);
}

bool Blockmap::get_free(Block &block)
{
    block.cpos = 0; // important - otherwise an exact match that occurs at a lower cpos would be ignored
    std::set<Block>::iterator iter = freeblocks_by_size.lower_bound(block);
    if (iter == freeblocks_by_size.end()) return false;
    Block fblock(*iter);
    freeblocks_by_position.erase(&*iter);
    freeblocks_by_size.erase(iter);
    block.cpos = fblock.cpos;
    fblock.cpos += block.clen;
    fblock.clen -= block.clen;
    add_free(fblock);
    return true;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////
// zz3 helper functions

void fatal(string pathname, const string &msg)
{
    throw zz3error(pathname + ": " + msg);
}

void fatal(ZZ3FILE *zf, const string &msg)
{
    fatal(zf->pathname, msg);
}

void fatal_from_errno(ZZ3FILE *zf)
{
    fatal(zf->pathname, strerror(errno));
    // if (errno < 0 || errno >= sys_nerr) {
        // fatal(zf->pathname, "unknown system error");
    // } else fatal(zf->pathname, sys_errlist[errno]);
}

void check_mode(const char *pathname, const char *mode)
{
    if (NULL != strchr("rwa", mode[0]) && (mode[1] == '\0' || 0 == strcmp(mode + 1, "+"))) return;
    throw zz3error(string(pathname) + ": bad mode \"" + mode + "\"");
}

int constrain_effort(int effort)
{
    return effort < 0 ? 0 :
           effort > 9 ? 9 : effort;
}

void read_header(ZZ3FILE *zf)
{
    char buff[256];
    if (0 != fseek(zf->fh, 0L, SEEK_SET)) fatal_from_errno(zf);
    if (!fgets(buff, sizeof(buff), zf->fh)) fatal_from_errno(zf);
    if (0 != strcmp(buff, MAGIC_STRING)) fatal(zf, "bad header");
    zf->flen = read_u64(zf->fh);
    zf->blocksize = read_u32(zf->fh);
    zf->maploc = read_u64(zf->fh);
    if (zf->maploc == 0) fatal(zf, "incomplete file");
    // cout << "flen=" << zf->flen << endl;
    // cout << "blocksize=" << zf->blocksize << endl;
    // cout << "maploc=" << zf->maploc << endl;
}

void write_header(ZZ3FILE *zf)
{
    if (0 != fseek(zf->fh, 0L, SEEK_SET)) fatal_from_errno(zf);
    if (0 > fputs(MAGIC_STRING, zf->fh)) fatal_from_errno(zf);
    write_u64(zf->fh, zf->flen);
    write_u32(zf->fh, zf->blocksize);
    write_u64(zf->fh, zf->maploc);
}

void write_dirty_header(ZZ3FILE *zf)
{
    if (0 != fseek(zf->fh, 0L, SEEK_SET)) fatal_from_errno(zf);
    if (0 > fputs(MAGIC_STRING, zf->fh)) fatal_from_errno(zf);
    write_u64(zf->fh, 0);
    write_u32(zf->fh, 0);
    write_u64(zf->fh, 0);
}

void read_tables(ZZ3FILE *zf)
{
    if (0 != fseek(zf->fh, zf->maploc, SEEK_SET)) fatal_from_errno(zf);
    zf->blockmap.read(zf->fh);
}

void write_tables(ZZ3FILE *zf)
{
    if (0 != fseek(zf->fh, zf->maploc, SEEK_SET)) fatal_from_errno(zf);
    zf->blockmap.write(zf->fh);
}

bool ptr2_at_eof(ZZ3FILE *zf)
{
    uint64_t bnum = zf->flen / zf->blocksize;
    if (bnum != zf->blocknum) return false;
    ConstBuffIter end_of_file = zf->buffer.begin() + (zf->flen - bnum * zf->blocksize);
    assert(zf->buffer.begin() <= end_of_file && end_of_file <= zf->buffer.end());
    assert(zf->ptr2 <= end_of_file);
    return zf->ptr2 == end_of_file;
}

void read_block(ZZ3FILE *zf)
{
    static const Block EMPTYBLOCK;
    const Block &block = (zf->blocknum < zf->blockmap.size()) ? zf->blockmap[zf->blocknum] : EMPTYBLOCK;
    if (block.cpos == 0) {
        // block is a hole - all nils
        zf->buffer.assign(zf->buffer.size(), '\0');
        return;
    }
    uLongf len1 = block.clen;
    uLongf len2 = zf->buffer.size();
    assert(len1 != 0);
    assert(len1 <= zf->cbuffer.size());
    if (block.cpos != static_cast<uint64_t>(ftell(zf->fh))) {
        // we won't need to seek if we're reading a file sequentially that has all it's blocks in-order
        if (0 != fseek(zf->fh, block.cpos, SEEK_SET)) fatal_from_errno(zf);
    }
    if (1 != fread(&zf->cbuffer[0], len1, 1, zf->fh)) fatal_from_errno(zf);
    if (Z_OK != uncompress(&zf->buffer[0], &len2, &zf->cbuffer[0], len1)) fatal(zf, "uncompress error");
    if (len2 < zf->buffer.size()) {
        fill(zf->buffer.begin() + len2, zf->buffer.end(), '\0'); // pad with nils
    }
}

void flush_block(ZZ3FILE *zf)
{
    if (!zf || !zf->writing || zf->ptr1 == zf->ptr2) return;
    if (!zf->dirty) {
        zf->dirty = true;
        write_dirty_header(zf);
    }
    assert(zf->ptr1 < zf->ptr2);
    assert(zf->buffer.begin() <= zf->ptr1 && zf->ptr1 <  zf->buffer.end());
    assert(zf->buffer.begin() <  zf->ptr2 && zf->ptr2 <= zf->buffer.end());
    BuffIter saved_ptr2 = zf->ptr2;

    // if partially writing into a block we must merge any prefix/suffix data from the existing block
    // don't worry about the truncation of the last block - it is padded with nils so all blocks are the same size
    if (zf->ptr1 != zf->buffer.begin() || ((zf->ptr2 != zf->buffer.end()) && !ptr2_at_eof(zf))) {
        CharBuff savedata(zf->buffer);
        read_block(zf);
        BuffIter src = savedata.begin() + (zf->ptr1 - zf->buffer.begin());
        copy(src, src + (zf->ptr2 - zf->ptr1), zf->ptr1);
    }
    zf->ptr1 = zf->buffer.begin();
    zf->ptr2 = zf->buffer.end();

    // tiny optimization - trim off trailing nils - all blocks are padded with trailing nils as needed when decompressing
    while (zf->ptr2 > zf->ptr1 && zf->ptr2[-1] == '\0') --zf->ptr2;
    // if entire block is nil keep one nil character
    if (zf->ptr1 == zf->ptr2) ++zf->ptr2;

    uLongf csize = zf->cbuffer.size();
    if (Z_OK != compress2(&zf->cbuffer[0], &csize, &zf->buffer[0], zf->ptr2 - zf->ptr1, zf->effort)) fatal(zf, "compress failed");

    // find a location for the compressed data
    Block &block = zf->blockmap.get(zf->blocknum);
    if (block.clen != csize) {
        zf->blockmap.add_free(block);
        assert(csize <= std::numeric_limits<uint32_t>::max());
        block.clen = static_cast<uint32_t>(csize);
        if (!zf->blockmap.get_free(block)) {
            block.cpos = zf->maploc;
            zf->maploc += block.clen;
        }
    }

    // write the compressed block
    if (block.cpos != static_cast<uint64_t>(ftell(zf->fh))) {
        // we won't need to seek if we're writing a brand new file sequentially and adding each block at the end
        if (0 != fseek(zf->fh, block.cpos, SEEK_SET)) fatal_from_errno(zf);
    }
    if (block.clen != fwrite(&zf->cbuffer[0], 1, csize, zf->fh)) fatal_from_errno(zf);

    // if readable, switch to reading since the buffer contains all the data for the block
    if (zf->readable) {
        zf->writing = false;
        zf->reading = true;
        zf->ptr1 = saved_ptr2;
        zf->ptr2 = zf->buffer.end();
    } else {
        // restore ptr2 since we're in write mode and ptr2 is the next write location
        zf->ptr2 = saved_ptr2;
    }
}

size_t raw_read(ZZ3FILE *zf, char *buff, size_t count, bool check_stop_char, char stop_char)
{
    if (!zf || !zf->fh) return 0;
    size_t total_pushback = 0;
    if (zf->pushback != -1) {
        if (!zf->readable) fatal(zf, "reading not allowed");
        total_pushback = 1;
        --count;
        *buff++ = char(unsigned(zf->pushback) & 0xFF);
        zf->pushback = -1;
    }
    if (zf->writing) {
        // seek() is already optimized to flush writes and switch to reading the block
        // and the buffer will contain the whole block
        zz3_seek(zf, zf->fpos);
    }
    // don't allow reading past flen
    if ((zf->fpos + count) > zf->flen) count = (zf->flen > zf->fpos) ? (zf->flen - zf->fpos) : 0;
    if (count == 0) {
        if (!zf->readable) fatal(zf, "reading not allowed");
        return total_pushback;
    }

    if (!zf->reading) {
        if (!zf->readable) fatal(zf, "reading not allowed");
        if (count == 0) return 0;
        uint64_t bnum = zf->fpos / zf->blocksize;
        if (bnum > std::numeric_limits<size_t>::max()) fatal(zf, "position overflow");
        zf->blocknum = static_cast<size_t>(bnum);
        read_block(zf);
        zf->ptr1 = zf->buffer.begin() + (zf->fpos - bnum * static_cast<uint64_t>(zf->blocksize));
        zf->ptr2 = zf->buffer.end();
        zf->reading = true;
    }

    // in read mode, the whole buffer is always equal to the whole block and ptr1 points to the next data to read
    size_t remaining = count;
    do {
        if (zf->ptr1 == zf->ptr2) {
            ++zf->blocknum;
            read_block(zf);
            zf->ptr1 = zf->buffer.begin();
        }
        char c = *buff++ = *zf->ptr1++;
        --remaining;
        if (check_stop_char && c == stop_char) break;
    } while (remaining);
    size_t total_read = count - remaining;
    zf->fpos += total_read;
    return total_read + total_pushback;
}

size_t raw_write(ZZ3FILE *zf, const char *buff, size_t count)
{
    if (!zf || !zf->fh) fatal(zf, "file not open");
    if (count == 0) {
        if (!zf->writeable) fatal(zf, "writing not allowed");
        return 0;
    }
    if (zf->reading) {
        // ptr1 is the current position and ptr2 is buffer.end()
        // the buffer contains the whole block
        zf->ptr2 = zf->ptr1;
        zf->ptr1 = zf->buffer.begin();
        zf->pushback = -1;
        zf->reading = false;
        zf->writing = true;
    }
    if (!zf->writing) {
        if (!zf->writeable) fatal(zf, "writing not allowed");
        zf->writing = true;
        uint64_t bnum = zf->fpos / zf->blocksize;
        if (bnum > std::numeric_limits<size_t>::max()) fatal(zf, "write overflow");
        zf->blocknum = static_cast<size_t>(bnum);
        zf->ptr1 = zf->ptr2 = zf->buffer.begin() + (zf->fpos - bnum * zf->blocksize);
    }
    size_t remaining = count;
    do {
        if (zf->ptr2 == zf->buffer.end()) {
            flush_block(zf);
            if (zf->blocknum == std::numeric_limits<size_t>::max()) fatal(zf, "write overflow");
            ++zf->blocknum;
            zf->ptr1 = zf->ptr2 = zf->buffer.begin();
        }
        *zf->ptr2++ = *buff++;
    } while (--remaining);
    zf->fpos += count;
    if (zf->fpos > zf->flen) zf->flen = zf->fpos;
    return count;
}

} // anonymous namespace

////////////////////////////////////////////////////////////////////////////////////////////////////////////
// zz3 public methods


ZZ3FILE *zz3_open(const char *path, const char *mode)
{
    return zz3_open(path, mode, DEFAULT_BLOCKSIZE, DEFAULT_EFFORT);
}

ZZ3FILE *zz3_open(const char *path, const char *mode, uint32_t blocksize, int effort)
{
    check_mode(path, mode);
    if (blocksize > std::numeric_limits<uint32_t>::max()) fatal(path, "ridiculously large blocksize");
    effort = constrain_effort(effort);

    FILE *fh = fopen(path, mode[0] == 'a' ? "r+" : mode);
    if (!fh) return NULL;

    ZZ3FILE *zf = new ZZ3FILE;
    zf->fh = fh;
    zf->pathname = path;
    zf->blocksize = blocksize;
    zf->effort = (effort < 0) ? 0 : (effort > 9) ? 9 : effort;
    zf->readable = mode[0] == 'r' || mode[1] == '+';                    // r . . r+ w+ a+
    zf->writeable = mode[0] == 'w' || mode[0] == 'a' || mode[1] == '+'; // . w a r+ w+ a+
    zf->append = (mode[0] == 'a');
    zf->reading = zf->writing = zf->dirty = false;
    zf->pushback = -1;
    zf->fpos = zf->flen = zf->maploc = 0;
    zf->blocknum = 0;
    if (mode[0] == 'r' || mode[0] == 'a') {
        read_header(zf);
        read_tables(zf);
    } else {
        write_dirty_header(zf);
        zf->maploc = static_cast<uint64_t>(ftell(fh));
        zf->dirty = true;
        zf->writing = true;
    }
    zf->buffer.resize(zf->blocksize);
    zf->cbuffer.resize(compressBound(zf->blocksize));
    zf->ptr1 = zf->ptr2 = zf->buffer.begin();
    // cout << (void*)zf << ": zz3_open(" << path << ", " << mode << ", " << blocksize << ", " << effort << ")\n";
    return zf;
}

void zz3_flush(ZZ3FILE *zf)
{
    if (!zf || !zf->fh) return;
    flush_block(zf);
    if (zf->dirty) {
        zf->dirty = false;
        write_tables(zf);
        write_header(zf);
        // TODO: remove lastfree and truncate file
    }
}

void zz3_close(ZZ3FILE *zf)
{
    // cout << (void*)zf << ": zz3_close()\n";
    if (!zf || !zf->fh) return;
    zz3_flush(zf);
    fclose(zf->fh);
    zf->fh = NULL;
    zf->pathname.clear();
    zf->blockmap.clear();
    zf->buffer.clear();
    zf->cbuffer.clear();
    delete zf;
}

string zz3_pathname(ZZ3FILE *zf)
{
    if (!zf || !zf->fh) return string();
    return zf->pathname;
}

int zz3_effort(ZZ3FILE *zf)
{
    return zf ? zf->effort : -1;
}

uint32_t zz3_blocksize(ZZ3FILE *zf)
{
    return zf ? zf->blocksize : 0;
}

void zz3_set_effort(ZZ3FILE *zf, int effort)
{
    if (!zf || !zf->fh) return;
    zf->effort = constrain_effort(effort);
}

uint64_t zz3_length(ZZ3FILE *zf)
{
    return (zf && zf->fh) ? zf->flen : 0;
}

void zz3_seek(ZZ3FILE *zf, uint64_t newpos)
{
    if (!zf || !zf->fh) return;
    uint64_t bnum = newpos / zf->blocksize;
    if (bnum > std::numeric_limits<size_t>::max()) fatal(zf, "position overflow");
    zf->fpos = newpos;
    if (zf->writing) {
        flush_block(zf);
        zf->writing = false;
        if (zf->readable && bnum == zf->blocknum) {
            zf->reading = true;
            zf->ptr2 = zf->buffer.end();
        }
    }
    if (zf->reading && bnum != zf->blocknum) zf->reading = false;
    if (zf->reading) zf->ptr1 = zf->buffer.begin() + (zf->fpos - bnum * zf->blocksize);
}

uint64_t zz3_tell(ZZ3FILE *zf)
{
    return (zf && zf->fh) ? zf->fpos : static_cast<uint64_t>(-1);
}

bool zz3_eof(ZZ3FILE *zf)
{
    return (zf && zf->fh) ? zf->fpos >= zf->flen : true;
}

int zz3_getc(ZZ3FILE *zf)
{
    char c;
    size_t x = raw_read(zf, &c, 1, false, '\0');
    return x == 1 ? (int)(unsigned char)c : -1;
}

int zz3_ungetc(ZZ3FILE *zf, int c)
{
    if (!zf || !zf->fh) return -1;
    zf->pushback = c;
    return c;
}

int zz3_peek(ZZ3FILE *zf)
{
    char c;
    size_t len = raw_read(zf, &c, 1, false, '\0');
    if (len != 1) return -1;
    zz3_ungetc(zf, (int)(unsigned char)c);
    return (int)(unsigned char)c;
}

int zz3_putc(ZZ3FILE *zf, int c)
{
   char c2 = (char)c;
   size_t len = raw_write(zf, &c2, 1);
   return (len == 1) ? c : -1;
}

size_t zz3_read(ZZ3FILE *zf, char *buff, size_t bufflen)
{
    return raw_read(zf, buff, bufflen, false, '\0');
}

size_t zz3_read(ZZ3FILE *zf, string &str, size_t maxlen)
{
    if (!zf || !zf->fh) return 0;
    size_t remaining = zf->fpos > zf->flen ? 0 : zf->flen - zf->fpos;
    size_t len = maxlen < remaining ? maxlen : remaining;
    str.resize(len);
    size_t len2 = raw_read(zf, &str[0], len, false, '\0');
    assert(len == len2);
    return len2;
}

size_t zz3_write(ZZ3FILE *zf, const char *buff, size_t count)
{
    return raw_write(zf, buff, count);
}

size_t zz3_write(ZZ3FILE *zf, const string &str)
{
    return raw_write(zf, str.c_str(), str.size());
}

size_t zz3_puts(ZZ3FILE *zf, const char *buff)
{
    return raw_write(zf, buff, strlen(buff));
}

size_t zz3_gets(ZZ3FILE *zf, const char *buff, size_t bufflen);

size_t zz3_getline(ZZ3FILE *zf, char *buff, size_t bufflen)
{
    return raw_read(zf, buff, bufflen, true, '\n');
}

size_t zz3_getline(ZZ3FILE *zf, string &line)
{
    return zz3_getline(zf, line, '\n');
}

size_t zz3_getline(ZZ3FILE *zf, string &line, char delim, uint64_t maxlen)
{
    char buffer[4096];
    line.clear();
    while (1) {
        size_t len = std::min(maxlen - line.size(), sizeof(buffer));
        len = raw_read(zf, buffer, len, true, delim);
        if (len == 0) break;
        line.append(buffer, buffer + len);
        if (buffer[len - 1] == delim) break;
        if (line.size() >= maxlen) break;
    }
    return line.size();
}

size_t zz3_getline(ZZ3FILE *zf, string &line, char delim)
{
    char buffer[4096];
    line.clear();
    while (1) {
        size_t len = raw_read(zf, buffer, sizeof(buffer), true, delim);
        if (len == 0) break;
        line.append(buffer, buffer + len);
        if (buffer[len - 1] == delim) break;
    }
    return line.size();
}

void zz3_showmap(ZZ3FILE *zf)
{
    if (!zf || !zf->fh) return;
    zf->blockmap.show();
}

void zz3_showblocks(ZZ3FILE *zf)
{
    if (!zf || !zf->fh) return;
    flush_block(zf);
    zf->reading = false;
    for (zf->blocknum = 0; zf->blocknum < zf->blockmap.size(); ++zf->blocknum) {
        Block &block = zf->blockmap[zf->blocknum];
        if (block.empty()) continue;
        read_block(zf);
        cout << "block[" << zf->blocknum << "]=";
        showbuffer(zf->buffer);
    }
    zf->blocknum = 0;
    zf->ptr1 = zf->ptr2 = zf->buffer.begin();
}

