CXX = g++
CXXFLAGS = -O2 -Wall -Wextra -Wpedantic
OPENVR = /opt/steamvr/tools/hellovr_vulkan_linux/src/openvr/headers
LIBS = -L/opt/steamvr/bin/linuxarm64 -lopenvr_api -Wl,-rpath,/opt/steamvr/bin/linuxarm64

frameshot: frameshot.cpp
	$(CXX) $(CXXFLAGS) -I$(OPENVR) -o $@ frameshot.cpp $(LIBS)

clean:
	rm -f frameshot
