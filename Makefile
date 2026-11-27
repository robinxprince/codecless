FFMPEG   ?= /opt/homebrew/opt/ffmpeg
SDK      ?= sdk/Premiere Pro 26.0 C++ SDK/Examples/Headers
INSTALL  ?= /Library/Application Support/Adobe/Common/Plug-ins/7.0/MediaCore/MediaEngine
STATS    ?= $(HOME)/Library/Logs/MediaEngine
CXX      ?= clang++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -arch arm64
CPPFLAGS += -I$(FFMPEG)/include
LDLIBS    = -L$(FFMPEG)/lib -lavformat -lavcodec -lavutil -lswresample -lswscale

CORE   = src/video_decoder.cpp src/audio_decoder.cpp
HDRS   = src/video_decoder.h src/audio_decoder.h
BUNDLE = build/MediaEngine.bundle

all: build/meprobe $(BUNDLE)

build/meprobe: tools/meprobe.cpp $(CORE) $(HDRS)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -o $@ tools/meprobe.cpp $(CORE) $(LDLIBS)

$(BUNDLE): src/importer.cpp src/stats.cpp src/stats.h src/importer.r Info.plist $(CORE) $(HDRS)
	@mkdir -p $@/Contents/MacOS $@/Contents/Resources
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -I"$(SDK)" -Wno-unused-parameter -Wno-four-char-constants -bundle \
		-o $@/Contents/MacOS/MediaEngine src/importer.cpp src/stats.cpp $(CORE) $(LDLIBS) \
		-framework CoreFoundation -framework CoreVideo
	Rez -useDF -o $@/Contents/Resources/MediaEngine.rsrc src/importer.r
	cp Info.plist $@/Contents/Info.plist
	codesign --force --sign - $@

install: $(BUNDLE)
	rm -rf "$(INSTALL)/MediaEngine.bundle"
	cp -R $(BUNDLE) "$(INSTALL)/"
	mkdir -p "$(STATS)"
	cp tools/stats.html "$(STATS)/stats.html"

# A self-contained plugin to give to other Macs: static FFmpeg and dav1d from
# tools/build-deps.sh, Apple silicon with macOS 13 or later, no Homebrew needed.
DEPS    = build/deps/out
PACKAGE = build/package/MediaEngine
PBUNDLE = $(PACKAGE)/MediaEngine.bundle

$(DEPS)/lib/libavcodec.a:
	tools/build-deps.sh

package: $(DEPS)/lib/libavcodec.a
	rm -rf $(PACKAGE) && mkdir -p $(PBUNDLE)/Contents/MacOS $(PBUNDLE)/Contents/Resources dist
	$(CXX) $(CXXFLAGS) -mmacosx-version-min=13.0 -I$(DEPS)/include -I"$(SDK)" -Wno-unused-parameter \
		-Wno-four-char-constants -bundle -o $(PBUNDLE)/Contents/MacOS/MediaEngine src/importer.cpp src/stats.cpp $(CORE) \
		$$(PKG_CONFIG_PATH=$(DEPS)/lib/pkgconfig pkg-config --static --libs libavformat libavcodec libswscale libswresample libavutil) \
		-framework CoreFoundation -framework CoreVideo
	Rez -useDF -o $(PBUNDLE)/Contents/Resources/MediaEngine.rsrc src/importer.r
	cp Info.plist $(PBUNDLE)/Contents/Info.plist
	codesign --force --sign - $(PBUNDLE)
	cp tools/stats.html $(PACKAGE)/
	cp LICENSE THIRD_PARTY_NOTICES.txt $(PACKAGE)/
	cp -R tools/package/. $(PACKAGE)/
	rm -f dist/MediaEngine-Importer.zip
	cd build/package && ditto -c -k --norsrc --noextattr --keepParent MediaEngine ../../dist/MediaEngine-Importer.zip
	@if otool -L $(PBUNDLE)/Contents/MacOS/MediaEngine | tail -n +2 | grep -v -E "/System/|/usr/lib/"; then \
		echo "error: the packaged plugin links a non-system library"; exit 1; fi
	@echo "dist/MediaEngine-Importer.zip"

clean:
	rm -rf build

.PHONY: all install package clean
