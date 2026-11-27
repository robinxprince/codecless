#include "stats.h"

#include <dispatch/dispatch.h>
#include <sys/stat.h>

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <map>
#include <mutex>
#include <vector>

#include "video_decoder.h"

namespace me::stats {
namespace {

using Clock = std::chrono::steady_clock;

constexpr size_t kKeptRequests = 2000;  // about a minute of busy scrubbing
constexpr size_t kKeptClosedClips = 20;

struct Totals {
    int id = 0;
    std::string name;
    std::string summary;
    int64_t requests = 0;
    int64_t hostCache = 0;
    int64_t decoded = 0;
    int64_t failed = 0;
    int64_t pictures = 0;
    double waitMs = 0;
    double workMs = 0;
};

struct Row {
    double t;  // ms since the plugin loaded
    int clip;
    Request request;
};

struct State {
    std::mutex mutex;
    const Clock::time_point start = Clock::now();
    const int64_t startEpochMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::system_clock::now().time_since_epoch())
                                     .count();
    std::map<const void*, Totals> clips;
    std::vector<Totals> closed;
    int nextId = 1;
    std::deque<Row> rows;
    bool flushPending = false;
};

State& state() {
    static State s;
    return s;
}

double since(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

void appendString(std::string& out, const std::string& text) {
    out += '"';
    for (unsigned char c : text) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += char(c);
        } else if (c < 0x20) {
            char escaped[8];
            std::snprintf(escaped, sizeof escaped, "\\u%04x", c);
            out += escaped;
        } else {
            out += char(c);
        }
    }
    out += '"';
}

void appendf(std::string& out, const char* format, ...) __attribute__((format(printf, 2, 3)));
void appendf(std::string& out, const char* format, ...) {
    char buffer[256];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof buffer, format, args);
    va_end(args);
    out += buffer;
}

void appendTotals(std::string& out, const Totals& t, bool open) {
    appendf(out, "{\"id\":%d,\"open\":%s,\"name\":", t.id, open ? "true" : "false");
    appendString(out, t.name);
    out += ",\"summary\":";
    appendString(out, t.summary);
    appendf(out,
            ",\"requests\":%lld,\"hostCache\":%lld,\"decoded\":%lld,\"failed\":%lld,\"pictures\":%lld,"
            "\"waitMs\":%.1f,\"workMs\":%.1f}",
            (long long)t.requests, (long long)t.hostCache, (long long)t.decoded, (long long)t.failed,
            (long long)t.pictures, t.waitMs, t.workMs);
}

// The page loads this as a script: browsers refuse to fetch local files from a local page.
std::string serialize(const State& s, const FrameCacheUsage& cache) {
    std::string out = "window.mediaEngineStats = {";
    appendf(out, "\"version\":1,\"started\":%lld,\"updated\":%lld,", (long long)s.startEpochMs,
            (long long)(s.startEpochMs + int64_t(since(s.start))));
    appendf(out, "\"cache\":{\"bytes\":%zu,\"budget\":%zu,\"pictures\":%zu,\"hits\":%lld,\"misses\":%lld},",
            cache.bytes, cache.budget, cache.pictures, (long long)cache.hits, (long long)cache.misses);
    out += "\"clips\":[";
    bool first = true;
    for (const auto& [key, totals] : s.clips) {
        if (!first) out += ',';
        appendTotals(out, totals, true);
        first = false;
    }
    for (const Totals& totals : s.closed) {
        if (!first) out += ',';
        appendTotals(out, totals, false);
        first = false;
    }
    // [t, clip, frame, intent, background, outcome, waitMs, workMs, pictures, width, height]
    out += "],\"requests\":[";
    first = true;
    for (const Row& row : s.rows) {
        const Request& r = row.request;
        appendf(out, "%s[%.1f,%d,%lld,%d,%d,%d,%.1f,%.1f,%lld,%d,%d]", first ? "" : ",", row.t, row.clip,
                (long long)r.frame, r.intent, r.background ? 1 : 0, int(r.outcome), r.waitMs, r.workMs,
                (long long)r.pictures, r.width, r.height);
        first = false;
    }
    out += "]};\n";
    return out;
}

void write(const std::string& text) {
    const char* home = std::getenv("HOME");
    const std::string dir = std::string(home ? home : "/tmp") + "/Library/Logs/MediaEngine";
    mkdir(dir.c_str(), 0755);
    const std::string path = dir + "/stats.js", temporary = path + ".tmp";
    FILE* file = std::fopen(temporary.c_str(), "w");
    if (!file) return;
    const bool ok = std::fwrite(text.data(), 1, text.size(), file) == text.size();
    if (std::fclose(file) == 0 && ok) std::rename(temporary.c_str(), path.c_str());  // never half a file
}

void flush(void*) {
    State& s = state();
    const FrameCacheUsage cache = frameCacheUsage();
    std::string text;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        s.flushPending = false;
        text = serialize(s, cache);
    }
    write(text);
}

// At most one write a second, and the last requests of a burst still reach the page.
void scheduleFlush(State& s) {
    if (s.flushPending) return;
    s.flushPending = true;
    dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), dispatch_get_global_queue(QOS_CLASS_UTILITY, 0),
                     nullptr, flush);
}

}  // namespace

void addClip(const void* clip, const std::string& name, const std::string& summary) {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    Totals& totals = s.clips[clip];
    if (totals.id == 0) totals.id = s.nextId++;
    totals.name = name;
    totals.summary = summary;
    scheduleFlush(s);
}

void removeClip(const void* clip) {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    auto it = s.clips.find(clip);
    if (it == s.clips.end()) return;
    s.closed.push_back(std::move(it->second));
    if (s.closed.size() > kKeptClosedClips) s.closed.erase(s.closed.begin());
    s.clips.erase(it);
    scheduleFlush(s);
}

void record(const Request& request) {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    int id = 0;
    if (auto it = s.clips.find(request.clip); it != s.clips.end()) {
        Totals& t = it->second;
        id = t.id;
        ++t.requests;
        t.hostCache += request.outcome == Outcome::HostCache;
        t.decoded += request.outcome == Outcome::Decoded;
        t.failed += request.outcome == Outcome::Failed;
        t.pictures += request.pictures;
        t.waitMs += request.waitMs;
        t.workMs += request.workMs;
    }
    s.rows.push_back({since(s.start), id, request});
    if (s.rows.size() > kKeptRequests) s.rows.pop_front();
    scheduleFlush(s);
}

}  // namespace me::stats
