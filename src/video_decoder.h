// Frame-accurate AV1/VP9 video decoding on Apple's media engine (VideoToolbox),
// with an automatic software fallback (dav1d / libavcodec vp9).
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

namespace me {

enum class Backend { Hardware, Software };

struct VideoInfo {
    std::string codec;  // "av1" or "vp9"
    int width = 0;
    int height = 0;
    AVRational frameRate{0, 1};  // constant rate the clip is presented at
    AVRational sampleAspect{1, 1};
    int64_t frameCount = 0;      // duration in frames at frameRate
    int bitDepth = 8;            // of the decoded pictures
    double startSeconds = 0;     // container time of frame 0; audio aligns to it
    AVColorPrimaries primaries = AVCOL_PRI_UNSPECIFIED;
    AVColorTransferCharacteristic transfer = AVCOL_TRC_UNSPECIFIED;
    AVColorSpace matrix = AVCOL_SPC_UNSPECIFIED;
    bool fullRange = false;
};

struct FrameCacheUsage {
    size_t bytes = 0;
    size_t budget = 0;
    size_t pictures = 0;
    int64_t hits = 0;
    int64_t misses = 0;
};

// The cache of decoded pictures shared by every decoder in the process.
FrameCacheUsage frameCacheUsage();

// Not thread-safe: callers serialize access per instance.
class VideoDecoder {
public:
    static std::unique_ptr<VideoDecoder> open(const std::string& path, std::string& error);
    ~VideoDecoder();

    // Another decoder of the same file with its own read position, opened without reading
    // the file again.
    std::unique_ptr<VideoDecoder> sibling(std::string& error) const;

    const VideoInfo& info() const { return info_; }
    Backend backend() const { return backend_; }

    // Returns the picture shown at frame number `frame` (0-based, at info().frameRate),
    // or nullptr with `error` set. Variable-frame-rate sources map to the frame on screen
    // at that time. The frame stays valid until the next call. Hardware frames have format
    // AV_PIX_FMT_VIDEOTOOLBOX and carry a CVPixelBufferRef in data[3].
    const AVFrame* frameAt(int64_t frame, std::string& error);

    // Pictures decoded so far, including those decoded only to reach a later one.
    int64_t picturesDecoded() const { return picturesDecoded_; }

private:
    VideoDecoder() = default;
    bool openFile(const std::string& path, std::string& error);
    bool buildIndex(std::string& error);
    bool openCodec(Backend backend, std::string& error);
    const AVFrame* decodeIndex(int64_t index, std::string& error);
    void seekToKeyframeBefore(int64_t index, int stepsBack);
    int decodeNext();  // 0 = frame_ holds the next frame; otherwise AVERROR_EOF or an error
    int64_t indexOfTimestamp(int64_t ts) const;

    AVFormatContext* format_ = nullptr;
    AVCodecContext* codec_ = nullptr;
    AVBufferRef* hwDevice_ = nullptr;
    AVPacket* packet_ = nullptr;
    AVFrame* frame_ = nullptr;   // the decoder's latest picture
    AVFrame* next_ = nullptr;
    AVFrame* cached_ = nullptr;  // our reference to a picture served from the shared cache
    int stream_ = -1;
    Backend backend_ = Backend::Hardware;
    VideoInfo info_;

    std::vector<int64_t> timestamps_;  // presentation timestamps, ascending, one per picture
    std::vector<int64_t> keyframes_;   // picture indices of keyframes, ascending
    std::vector<int64_t> hidden_;      // timestamps of pictures an edit list hides, ascending
    int64_t current_ = -1;             // picture index held in frame_, -1 if none
    int64_t picturesDecoded_ = 0;
    bool packetPending_ = false;       // packet_ was refused with EAGAIN and must be resent
    bool draining_ = false;
};

}  // namespace me
