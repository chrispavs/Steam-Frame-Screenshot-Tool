CXX = g++
CXXFLAGS = -O2 -Wall -Wextra -Wpedantic
OPENVR = /opt/steamvr/tools/hellovr_vulkan_linux/src/openvr/headers
LIBS = -L/opt/steamvr/bin/linuxarm64 -lopenvr_api -Wl,-rpath,/opt/steamvr/bin/linuxarm64 -lwayland-client

PROTOCOL_H = protocol/gamescope-action-binding-client-protocol.h
PROTOCOL_C = protocol/gamescope-action-binding-protocol.c
PROTOCOL_O = protocol/gamescope-action-binding-protocol.o

frameshot: frameshot.cpp $(PROTOCOL_O)
	$(CXX) $(CXXFLAGS) -I$(OPENVR) -Iprotocol -o $@ frameshot.cpp $(PROTOCOL_O) $(LIBS)

$(PROTOCOL_H): protocol/gamescope-action-binding.xml
	wayland-scanner client-header $< $@

$(PROTOCOL_C): protocol/gamescope-action-binding.xml
	wayland-scanner private-code $< $@

$(PROTOCOL_O): $(PROTOCOL_C) $(PROTOCOL_H)
	gcc -O2 -Iprotocol -c -o $@ $(PROTOCOL_C)

clean:
	rm -f frameshot $(PROTOCOL_H) $(PROTOCOL_C) $(PROTOCOL_O)
