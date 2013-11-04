CFLAGS = -g -O3 -DSTATISTICS #-DPROFILER
ifdef MEMCHECK
TRACE_FLAGS = -faddress-sanitizer
endif
TRACE_FLAGS += -fno-omit-frame-pointer # for better stack traces in error messages
TRACE_FLAGS += -fno-optimize-sibling-calls # disable tail call elimination
CLANG_FLAGS = -Wno-attributes
LDFLAGS = -lpthread $(TRACE_FLAGS) -lprofiler -rdynamic -laio -lnuma -lrt
CXXFLAGS = -g -O3 -Iinclude -I. -Wall -std=c++0x $(TRACE_FLAGS) $(CLANG_FLAGS) -DPROFILER -DSTATISTICS
CPPFLAGS := -MD

SOURCE := $(wildcard *.c) $(wildcard *.cpp)
OBJS := $(patsubst %.c,%.o,$(patsubst %.cpp,%.o,$(SOURCE)))
DEPS := $(patsubst %.o,%.d,$(OBJS))
MISSING_DEPS := $(filter-out $(wildcard $(DEPS)),$(DEPS))
MISSING_DEPS_SOURCES := $(wildcard $(patsubst %.d,%.c,$(MISSING_DEPS)) $(patsubst %.d,%.cc,$(MISSING_DEPS)))
ifdef MEMCHECK
CXXFLAGS += -DMEMCHECK
CC = clang
CXX = clang++
else
CC = gcc
CXX = g++
endif

all: build_lib unit_test tools apps test utils

build_lib:
	$(MAKE) -C libsafs

unit_test: build_lib
ifndef MEMCHECK
	$(MAKE) -C unit-test
endif

test: build_lib
	$(MAKE) -C test

tools: build_lib
	$(MAKE) -C tools

utils: build_lib
	$(MAKE) -C utils

apps: build_lib
	$(MAKE) -C apps

clean:
	rm -f *.d
	rm -f *.o
	rm -f *~
	rm -f include/*~
	find -name core -delete
	make --ignore-errors -C unit-test clean
	make --ignore-errors -C test clean
	make --ignore-errors -C libsafs clean
	make --ignore-errors -C tools clean
	make --ignore-errors -C utils clean
	make --ignore-errors -C apps clean

-include $(DEPS) 
