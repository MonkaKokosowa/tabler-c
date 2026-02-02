# Define the compiler
CC = gcc

# Define compiler flags
#CFLAGS = -I. -lvgl -O3 -Wall -Wshadow -Wundef -Wmaybe-uninitialized
# Add -DLV_CONF_INCLUDE_SIMPLE so it finds your config file
CFLAGS = -I. -O3 -Wall -Wshadow -Wundef -Wmaybe-uninitialized -DLV_CONF_INCLUDE_SIMPLE
LDFLAGS = -lcurl -lcjson -lm

# Find all C files in the lvgl directory
SRCS = main.c $(shell find lvgl -name '*.c')

# Define the object files
OBJS = $(SRCS:.c=.o)

# Target executable
TARGET = dashboard

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $(TARGET) $(LDFLAGS)

clean:
	rm -f $(OBJS) $(TARGET)
