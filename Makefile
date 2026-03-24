CXX = g++
CXXFLAGS = -Wall -O2 -std=c++17
LIBS = -lsqlite3 -lpthread -lcrypt

TARGET = bin/portal

all: $(TARGET)

$(TARGET): src/main.cpp src/db.cpp src/auth.cpp src/helpers.cpp src/upload.cpp
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LIBS) -I src

clean:
	rm -f bin/portal
