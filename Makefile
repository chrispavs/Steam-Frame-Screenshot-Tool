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

install: frameshot
	./install.sh

release: frameshot install.sh packaging/frame-screenshot.desktop.in packaging/frameshot-keys.service.in
	rm -rf dist/frameshot-linux-arm64
	mkdir -p dist/frameshot-linux-arm64/packaging
	cp frameshot install.sh dist/frameshot-linux-arm64/
	cp packaging/frame-screenshot.desktop.in packaging/frameshot-keys.service.in dist/frameshot-linux-arm64/packaging/
	tar -C dist -czf dist/frameshot-linux-arm64.tar.gz frameshot-linux-arm64
	@echo dist/frameshot-linux-arm64.tar.gz

clean:
	rm -rf frameshot $(PROTOCOL_H) $(PROTOCOL_C) $(PROTOCOL_O) dist
