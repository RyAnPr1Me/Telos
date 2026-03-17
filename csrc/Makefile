CC      = gcc
CFLAGS  = -std=c11 -Wall -Wextra -O2
LDFLAGS =

SRCDIR  = .
SRCS    = $(SRCDIR)/lexer.c \
          $(SRCDIR)/parser.c \
          $(SRCDIR)/ir.c \
          $(SRCDIR)/lifting.c \
          $(SRCDIR)/optimizer.c \
          $(SRCDIR)/codegen_x86.c \
          $(SRCDIR)/codegen_c.c \
          $(SRCDIR)/executable.c \
          $(SRCDIR)/compiler.c

OBJS    = $(SRCS:.c=.o)

# Main compiler binary
TARGET  = telos

# Test binary
TEST_SRC = tests/test_main.c
TEST_OBJS = $(filter-out $(SRCDIR)/compiler.o, $(OBJS))
TEST_BIN = tests/test_telos

.PHONY: all clean test

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c telos.h
	$(CC) $(CFLAGS) -c -o $@ $<

test: $(TEST_BIN)
	./$(TEST_BIN)

$(TEST_BIN): $(TEST_SRC) $(TEST_OBJS) telos.h
	$(CC) $(CFLAGS) -o $@ $(TEST_SRC) $(TEST_OBJS) $(LDFLAGS) -lm

clean:
	rm -f $(OBJS) $(TARGET) $(TEST_BIN)
