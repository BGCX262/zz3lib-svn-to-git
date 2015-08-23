// zz3.i - swig interface file for python to use the zz3 library 
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
%module "zz3";
%feature("autodoc", "1");

%{
#include "zz3.h"
#include <iostream>
#include <boost/shared_ptr.hpp>
using std::cout;
%}

%include "std_string.i"
%include "stdint.i"

%feature("kwargs") Zz3File::Zz3File;
%feature("kwargs") open;
%newobject open;
%newobject Zz3File::__iter__;
%newobject Zz3File::__enter__;

%feature("python:slot", "tp_iter", functype="getiterfunc") Zz3File::__iter__;
%feature("python:slot", "tp_iternext", functype="iternextfunc") Zz3File::__iternext__;

%ignore zz3_file_closed;
%ignore zz3_io_error;
%ignore zz3_stop_iteration;

%typemap(throws) zz3_stop_iteration {
    (void)$1;
    SWIG_SetErrorObj(PyExc_StopIteration, SWIG_Py_Void());
    SWIG_fail;
}

%typemap(throws) zz3_io_error {
    PyErr_SetString(PyExc_IOError, $1.what());
    SWIG_fail;
}

%ignore Zz3File::Zz3File(const Zz3File &);
%catches(zz3_io_error) Zz3File::Zz3File;
%catches(zz3_io_error) open;
%catches(zz3_stop_iteration) Zz3File::__iternext__;

%exception {
try {
    $action
} catch (zz3error &e) {
    PyErr_SetString(PyExc_RuntimeError, e.what());
    SWIG_fail;
} catch (zz3_file_closed &e) {
    PyErr_SetString(PyExc_ValueError, "I/O operation on closed file");
    SWIG_fail;
} catch (std::exception &e) {
    PyErr_SetString(PyExc_RuntimeError, e.what());
    SWIG_fail;
}
}

%inline %{

struct zz3_stop_iteration {};
struct zz3_file_closed {};
class zz3_io_error {
  public:
    zz3_io_error(const std::string &msg) : msg_(msg) {}
    const char *what() const {return msg_.c_str();}
  private:
    std::string msg_;
};
  
class Zz3File {
  public:
    Zz3File(const Zz3File &zfile) : zf(zfile.zf) {
        // cout << "copied Zz3File " << (void*)this << "\n";
    }
    Zz3File(const char *path, const char *mode = "r", uint64_t blocksize = 8096, int effort = 1)
    : zf(zz3_open(path, mode, blocksize, effort), zz3_close) {

        // cout << "alloc  Zz3File " << (void*)this << "\n";
        if (!zf) throw zz3_io_error(std::string("Cannot open file: '") + path + "' mode: '" + mode + "'");
    }
    ~Zz3File() {
        // cout << "free   Zz3File " << (void*)this << "\n";
    }
    void close() {
        if (!zf) throw zz3_file_closed();
        zz3_flush(zf.get());
        zf.reset();
    }
    void flush() {
        zz3_flush(zf.get());
    }
    std::string read(int maxlen=-1) {
        if (!zf) throw zz3_file_closed();
        std::string result;
        zz3_read(zf.get(), result, maxlen);
        return result;
    }
    std::string name() {
        return zz3_pathname(zf.get());
    }
    std::string readline(int maxlen=-1) {
        if (!zf) throw zz3_file_closed();
        std::string result;
        if (maxlen == -1) {
            zz3_getline(zf.get(), result);
        } else {
            zz3_getline(zf.get(), result, '\n', maxlen);
        }
        return result;
    }
    void rewind() {
        if (!zf) throw zz3_file_closed();
        zz3_seek(zf.get(), 0);
    }
    uint64_t tell() {
        if (!zf) throw zz3_file_closed();
        return zz3_tell(zf.get());
    }
    void seek(int64_t offset, int whence=0) {
        if (!zf) throw zz3_file_closed();
        switch (whence) {
            case 0: break;
            case 1: offset += zz3_tell(zf.get()); break;
            case 2: offset += zz3_length(zf.get()); break;
            default: throw zz3error("invalid value for whence argument");
        }
        if (offset < 0) offset = 0;
        zz3_seek(zf.get(), offset);
    }
    void write(std::string data) {
        if (!zf) throw zz3_file_closed();
        zz3_write(zf.get(), data);
    }
    Zz3File *__iter__() {
        if (!zf) throw zz3_file_closed();
        return new Zz3File(*this);
    }
    std::string __iternext__() {
        if (!zf) throw zz3_file_closed();
        if (zz3_eof(zf.get())) throw zz3_stop_iteration();
        std::string result;
        zz3_getline(zf.get(), result);
        return result;
    }
    Zz3File *__enter__() {
        return new Zz3File(*this);
    }
    void __exit__(PyObject *, PyObject *, PyObject *) {
        this->close();
    }
  private:
    boost::shared_ptr<ZZ3FILE> zf;
};

Zz3File *open(const char *filename, const char *mode="r", uint64_t blocksize = 8096, int compresslevel = 1) {
    return new Zz3File(filename, mode, blocksize, compresslevel);
}

%}
