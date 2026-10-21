CXX      = g++
CXXFLAGS = -O2 -std=c++17 -pthread -Wall
LDLIBS   = -lbluetooth
TARGET   = btcpp
SRC      = btcpp.cpp

all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f $(TARGET)

install: $(TARGET)
	install -m 755 $(TARGET) /usr/local/bin/

.PHONY: all clean install