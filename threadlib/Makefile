# Makefile for threadlib

# Compiler
CXX := clang++
PYTHON := python3

ifndef RELEASE 
	#address, undefined, thread
	SAN := -fsanitize=thread
	CXXFLAGS := -g -Wall -Wextra -pedantic -fPIC 
else
	SAN :=
	CXXFLAGS := -O3 -Wall -Wextra -pedantic -fPIC
endif

# Compiler flags

OBJDIR := obj/
SRCDIR := src/
TESTDIR := tests/

TESTBINDIR := compiled-tests/
LIBDIR := threadlib/
CLANGDIR := clang/

SRCOBJDIR := $(OBJDIR)$(SRCDIR)
TESTOBJDIR := $(OBJDIR)$(LIBDIR)
CLANGOBJDIR := $(OBJDIR)$(CLANGDIR)

SRC := threadlib.cpp JobState.cpp ThreadPool.cpp
OBJS := $(addprefix $(SRCOBJDIR), $(SRC:.cpp=.o))
TESTSRCS := $(wildcard $(TESTDIR)*.c)

TESTOBJS := $(addprefix $(TESTOBJDIR), $(notdir $(TESTSRCS:.c=.o)))
CLANGTESTOBJS := $(addprefix $(CLANGOBJDIR), $(notdir $(TESTSRCS:.c=.o)))

TESTS := $(patsubst $(TESTOBJDIR)%.o, test-%, $(TESTOBJS))
CLANGTESTS := $(patsubst $(CLANGOBJDIR)%.o, clang-test-%, $(CLANGTESTOBJS))

TARGET := libthreadlib.so

.PHONY: all clean clean-tests all-tests
.PRECIOUS: $(TESTOBJDIR)%.o $(CLANGOBJDIR)%.o

$(SRCOBJDIR)%.o: $(SRCDIR)%.cpp 
	@mkdir -p $(SRCOBJDIR)
	$(CXX) $(CXXFLAGS) $(SAN) -c -pthread $< -o $@

$(TESTOBJDIR)%.o: $(TESTDIR)%.c
	@mkdir -p $(TESTOBJDIR)
	${LLVM_BIN}/clang -O3 -flto -mllvm --enable-extract-loop-bodies -c $< -o $@ 

$(CLANGOBJDIR)%.o: $(TESTDIR)%.c
	@mkdir -p $(CLANGOBJDIR)
	clang -O3 -c $< -o $@

test-%: $(TESTOBJDIR)%.o $(TARGET)
	@mkdir -p $(TESTBINDIR)$(LIBDIR)
	@#add -lubsan for ubsan linking
	${LLVM_BIN}/clang -O3 $(SAN) -flto -L. -o $(TESTBINDIR)$(LIBDIR)$@ $< -lthreadlib  

clang-test-%: $(CLANGOBJDIR)%.o
	@mkdir -p $(TESTBINDIR)$(CLANGDIR)
	clang -O3 -L. -o $(TESTBINDIR)$(CLANGDIR)$@ $< 

$(TARGET): $(OBJS) 
	$(CXX) $(CXXFLAGS) $(SAN) -shared -o $@ $^

all-tests: $(TESTS) $(CLANGTESTS)

all: $(TARGET) all-tests

benchmark: all
	@$(PYTHON) benchmark.py	

clean-tests:
	rm -rf $(TESTBINDIR) 
	rm -rf $(TESTOBJDIR)
	rm -rf $(CLANGOBJDIR)
	rm -rf logs/
	rm -f execution_times.tex

clean: clean-tests
	rm -rf $(OBJDIR) 
	rm -f  $(TARGET) 

