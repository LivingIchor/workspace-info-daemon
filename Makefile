CC = gcc
CFLAGS = -g -Wall -pthread

BINDIR = bin
OBJDIR = obj
SRCDIR = src

TARGET = workspace-info-daemon
SRCS = $(waldcard $(SRCDIR)/*.c)
OBJS = $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.o,$(SRCS))

all : $(BINDIR)/$(TARGET)

release : CFLAGS = -Wall -pthread
release : clean
release : $(BINDIR)/$(TARGET)

$(BINDIR)/$(TARGET) : $(OBJS) | $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $^

$(BINDIR) :
	mkdir $(BINDIR)

$(OBJDIR)/%.o : $(SRCDIR)/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJDIR) :
	mkdir $(OBJDIR)

.PHONY : clean
clean :
	rm -rf $(OBJDIR)
	rm -rf $(BINDIR)
