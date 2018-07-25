CC = g++
AR = ar
ROOT = $(shell pwd)
CFLAGS = -O3 -Iinc -DWEBRTC_APM_DEBUG_DUMP=0 -DWEBRTC_LINUX -DWEBRTC_POSIX -std=c++11
TARGET = libdse.a
SRCS = $(shell find $(ROOT)/src/ -name "*.cpp")
OBJS = $(shell find $(ROOT)/src/ -name "*.o")
SRCOBJS = $(SRCS:%.cpp=%.o) 

$(TARGET): $(SRCOBJS)
	$(AR) -cr $(TARGET) $(SRCOBJS)

%.o: %.cpp
	$(CC) -c $< -o $@ $(CFLAGS)

clean:
	rm -rf $(OBJS) $(TARGET)

