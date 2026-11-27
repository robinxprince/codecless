// Live numbers for the stats page: every frame request the host makes, written at most once a
// second to ~/Library/Logs/MediaEngine/stats.js, which stats.html next to it shows.
#pragma once

#include <cstdint>
#include <string>

namespace me::stats {

enum class Outcome { HostCache, Decoded, Failed };

struct Request {
    const void* clip = nullptr;
    int64_t frame = 0;
    int intent = -1;          // imRenderIntent
    bool background = false;  // served by the clip's background lane
    Outcome outcome = Outcome::Decoded;
    double waitMs = 0;        // for the lane's decoder
    double workMs = 0;        // decoding and filling the host's frame
    int64_t pictures = 0;     // decoded to produce it; 0 when the decoder already had it
    int width = 0;
    int height = 0;
};

void addClip(const void* clip, const std::string& name, const std::string& summary);
void removeClip(const void* clip);
void record(const Request& request);

}  // namespace me::stats
