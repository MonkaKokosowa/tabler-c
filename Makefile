CC = gcc

# -DLV_CONF_INCLUDE_SIMPLE so lvgl picks up our lv_conf.h
CFLAGS = -I. -O3 -Wall -Wshadow -Wundef -Wmaybe-uninitialized -DLV_CONF_INCLUDE_SIMPLE
LDFLAGS = -lcurl -lcjson -lm

SRCS = main.c $(shell find lvgl -name '*.c')
OBJS = $(SRCS:.c=.o)
TARGET = dashboard

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $(TARGET) $(LDFLAGS)

clean:
	rm -f $(OBJS) $(TARGET)
