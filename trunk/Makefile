CXX = g++
SWIG = /home/ralbacht/opt/swig-2.0.8/bin/swig
OPT = -g -O -DNDEBUG
BOOST_ROOT = /home/ralbacht/opt/boost_1_54_0

all : libzz3.so _zz3.so zz3zip zz3unzip zz3cat

zz3.o : zz3.h zz3.cc
	$(CXX) $(OPT) -c -Wall -Wconversion -fpic zz3.cc

libzz3.so : zz3.o
	$(CXX) $(OPT) -shared -o libzz3.so zz3.o

zz3_wrap.cc : zz3.i
	$(SWIG) -c++ -builtin -O -python -o zz3_wrap.cc zz3.i

zz3_wrap.o: zz3.h zz3_wrap.cc
	$(CXX) $(OPT) -I/tools/share/python/2.7.1/linux64/include/python2.7 -I$(BOOST_ROOT)/include -c -Wall -Wno-unused -fpic zz3_wrap.cc

_zz3.so : zz3_wrap.o
	$(CXX) -shared -o _zz3.so zz3_wrap.o -Wl,--enable-new-dtags -Wl,-rpath,\$$ORIGIN -L. -lzz3 -lz

% : %.cc zz3.o
	$(CXX) $(OPT) $< -o $@ zz3.o -lz

clean:
	rm -f *.o *.so *_wrap.* zz3.py zz3.pyc zz3zip zz3unzip zz3cat

