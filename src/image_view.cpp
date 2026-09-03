// Copyright 2023 ljrrjl. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <sstream>
#include <utility>
#include <memory>
#include <string>
#include <thread>
#include <mutex>

#include "ftxui/dom/requirement.hpp"
#include "ftxui/screen/screen.hpp"
#include "ftxui/dom/elements.hpp"
#include "ftxui/dom/node.hpp"

#include "bounded_cache.hpp"

// tiv_lib is now a small, decoder-agnostic library: it only knows how to
// turn a 4x8 block of pixels -- fed to it one pixel at a time via a
// GetPixelFunction callback -- into a terminal character plus fg/bg
// color. It no longer knows anything about CImg, image loading, or the
// network, so that glue lives here now instead of inside tiv_lib.
#include "tiv_lib.h"

// We only ever use CImg as a decoder, never its own window/display code.
// Disabling it removes the X11/GDI dependency and, importantly, removes
// the "pop up a dialog box" fallback that CImg's warning/error path can
// otherwise take (cimg_verbosity==2) -- not something you want happening
// from a background thread underneath a full-screen terminal UI. Must be
// defined before CImg.h is included.
#ifndef cimg_display
#define cimg_display 0
#endif
#include "CImg.h"

namespace ftxui {

namespace {

using ftxui::Screen;

// ---- Small helpers that used to live inside the old CImg-coupled tiv_lib ----

// Scales `this` down (never up) so it fits inside `container`, preserving
// aspect ratio. Replaces the old tiv::size::fitted_within, which isn't
// part of the new decoupled tiv_lib (it's display-layout math, not
// pixel-to-character conversion, so it doesn't belong there anyway).
struct FitSize {
    unsigned int width;
    unsigned int height;

    FitSize(unsigned int w, unsigned int h) : width(w), height(h) {}

    FitSize fitted_within(const FitSize& container) const {
        double scale = std::min(container.width / static_cast<double>(width),
                                 container.height / static_cast<double>(height));
        return FitSize(static_cast<unsigned int>(width * scale),
                        static_cast<unsigned int>(height * scale));
    }
};

// Encodes a Unicode code point as UTF-8 and writes it to `os`. Replaces
// the old tiv::printCodepoint, which the new tiv_lib also dropped (it
// never touched CImg or pixels either -- just wasn't carried over).
void WriteUtf8Codepoint(std::ostream& os, int codepoint) {
    if (codepoint < 128) {
        os << static_cast<char>(codepoint);
    } else if (codepoint < 0x7ff) {
        os << static_cast<char>(0xc0 | (codepoint >> 6));
        os << static_cast<char>(0x80 | (codepoint & 0x3f));
    } else if (codepoint < 0xffff) {
        os << static_cast<char>(0xe0 | (codepoint >> 12));
        os << static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f));
        os << static_cast<char>(0x80 | (codepoint & 0x3f));
    } else if (codepoint < 0x10ffff) {
        os << static_cast<char>(0xf0 | (codepoint >> 18));
        os << static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f));
        os << static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f));
        os << static_cast<char>(0x80 | (codepoint & 0x3f));
    } else {
        os << "ERROR";
    }
}

// Loads a file/URL as RGB via CImg, expanding greyscale images to 3
// channels. Replaces the old tiv::load_rgb_CImg.
cimg_library::CImg<unsigned char> LoadImageRGB(const char* const filename) {
    // CImg prints warnings/errors straight to stdout/stderr from inside
    // CImgException's constructor whenever cimg::exception_mode() != 0
    // (the POSIX default is 1, i.e. "console"). That happens *before*
    // our caller's try/catch ever runs, so wrapping this call in
    // try/catch alone does not stop the message from reaching the
    // terminal. Setting the mode to 0 ("quiet") makes CImg only throw,
    // never print. This is process-wide state, so setting it once would
    // be enough, but it's cheap and idempotent, and doing it here
    // guarantees it's set before any CImg loading code -- including on
    // this background thread -- runs.
    cimg_library::cimg::exception_mode(0);

    cimg_library::CImg<unsigned char> image(filename);
    if (image.spectrum() == 1) {
        // Greyscale: copy into all 3 channels.
        cimg_library::CImg<unsigned char> rgb_image(
            image.width(), image.height(), image.depth(), 3);
        for (unsigned int chn = 0; chn < 3; chn++) {
            rgb_image.draw_image(0, 0, 0, chn, image);
        }
        return rgb_image;
    }
    return image;
}

// findCharData's `flags` only matters here for one bit: FLAG_TELETEXT
// (32) opts into 3x2 teletext block characters, which we don't want. Any
// value without that bit set behaves identically for our purposes; the
// old code passed the CImg-coupled tiv_lib's FLAG_24BIT (8) for the same
// effect, but that flag (and the whole idea of a "color mode" flag) no
// longer exists now that tiv_lib doesn't know about color modes at all
// -- we always build truecolor ftxui::Color values directly below.
constexpr int kCharSelectionFlags = 0;

// ---- Hashing helper for the composite cache keys below ----

template <typename T>
void hash_combine(std::size_t& seed, const T& v) {
    seed ^= std::hash<T>{}(v) + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
}

// Monotonically increasing, process-wide version counter. Every time an
// image finishes (re)loading it gets the next value here -- never 0,
// never reused, even if its cache entry is later evicted and the same
// URL is loaded again from scratch. This is what lets resized_cache_ and
// char_cache_ below use a plain integer instead of the URL string in
// their keys: a stale tile from an older load of the same URL can never
// collide with a fresh one, because the version number is never repeated
// for the lifetime of the process.
inline std::atomic<uint64_t> g_next_version{1};

// One entry per distinct image URL: the decoded original pixels plus the
// version number that was current when this image was (re)loaded.
struct ImageEntry {
    cimg_library::CImg<unsigned char> img;
    uint64_t version = 0;
};

// Cache key for a resized copy of an image at a specific target size.
// `url_hash` is the *full* (untruncated) std::hash<std::string> of the
// URL, so two different URLs colliding here is practically impossible
// for any realistic number of images loaded in a process lifetime.
struct ResizeKey {
    uint64_t url_hash;
    unsigned int width;
    unsigned int height;
    uint64_t version;

    bool operator==(const ResizeKey& other) const {
        return url_hash == other.url_hash && width == other.width &&
               height == other.height && version == other.version;
    }
};
struct ResizeKeyHash {
    std::size_t operator()(const ResizeKey& k) const {
        std::size_t seed = std::hash<uint64_t>{}(k.url_hash);
        hash_combine(seed, k.width);
        hash_combine(seed, k.height);
        hash_combine(seed, k.version);
        return seed;
    }
};

struct CharKey {
    uint64_t url_hash;
    uint16_t x;
    uint16_t y;
    uint64_t version;
    uint16_t width;
    uint16_t height;

    bool operator==(const CharKey& other) const {
        return url_hash == other.url_hash &&
               x == other.x &&
               y == other.y &&
               version == other.version &&
               width == other.width &&
               height == other.height;
    }
};
struct CharKeyHash {
    size_t operator()(const CharKey& k) const {
        std::size_t seed = std::hash<uint64_t>{}(k.url_hash);
        hash_combine(seed, k.x);
        hash_combine(seed, k.y);
        hash_combine(seed, k.version);
        hash_combine(seed, k.width);
        hash_combine(seed, k.height);
        return seed;
    }
};

class ImageView: public Node {
public:
    // All three caches are bounded, so all three are what actually caps
    // this app's steady-state memory use for images:
    //  - image_cache_ holds full-resolution decoded originals. These are
    //    the big ones (a single decoded 4K photo is tens of MB), so this
    //    cap is deliberately the smallest of the three. Lower it further
    //    if your images tend to be large and/or memory is tight.
    //  - resized_cache_ holds the (much smaller) copies scaled down to
    //    whatever terminal-cell size they were last drawn at.
    //  - char_cache_ holds one small struct per 4x8-pixel terminal cell,
    //    so its cap can be far higher for the same memory budget.
    //
    // These are just reasonable-ish starting points, sized for "a handful
    // of images on screen at once". If your app can have dozens of
    // distinct thumbnails visible simultaneously (a grid, several
    // carousels, a queue view, and a playback bar, say), raise these via
    // setImageCacheMaxSize() / setImageResizeCacheMaxSize() /
    // setImageCharCacheMaxSize() at startup -- a cache smaller than your
    // actual working set won't crash you (see the find()-not-at() note in
    // Render() below) but it will mean images keep getting evicted and
    // re-decoded/re-resized instead of staying cached.
    inline static BoundedCache<std::string, ImageEntry> image_cache_{100};
    inline static BoundedCache<ResizeKey, cimg_library::CImg<unsigned char>, ResizeKeyHash> resized_cache_{200};
    inline static BoundedCache<CharKey, CharData, CharKeyHash> char_cache_{15000};

    // URLs currently being loaded by a background thread. Entries are
    // erased as soon as a load finishes (see the loader thread below)
    // rather than left behind set to `false`, so this map's size tracks
    // the number of loads in flight right now, not the number of
    // distinct URLs ever requested over the process's lifetime.
    inline static std::unordered_map<std::string, bool> inflight_;
    inline static std::mutex mutex_;

    // Caps how many background loader threads can be running at once.
    // Each one is a real OS thread and, for network URLs (when CImg isn't
    // built with libcurl), a curl/wget *subprocess* -- spawning dozens of
    // these at the same instant (e.g. an app opening on a screen with 40+
    // uncached thumbnails) is wasteful at best, and at worst can hit
    // OS-level fd/process limits or lean on rare concurrency edge cases in
    // CImg's own network-loading code. Requests beyond the cap simply wait:
    // ComputeRequirement() re-checks every frame, and on_loaded_ already
    // triggers a redraw whenever any load finishes, so a queued image
    // starts as soon as a slot frees up.
    inline static std::atomic<int> inflight_count_{0};
    inline static int max_concurrent_loads_ = 6;

    inline static cimg_library::CImg<unsigned char> black_img = cimg_library::CImg<unsigned char>(1, 1, 1, 3, 0);
    inline static std::function<void()> on_loaded_;

    uint64_t url_hash_;

    explicit ImageView(std::string_view url) : url_(url) {
        url_hash_ = std::hash<std::string>{}(url_);
    }

    void ComputeRequirement() override {
        const cimg_library::CImg<unsigned char>* img;

        {
            std::lock_guard<std::mutex> lock(mutex_);

            auto it = image_cache_.find(url_);
            if (it != image_cache_.end()) {
                img = &it->second.img;
            } else {
                // Insert a black placeholder immediately so layout has
                // something to measure while the real image loads.
                auto [it2, _] = image_cache_.emplace(
                    url_, ImageEntry{black_img, g_next_version.fetch_add(1, std::memory_order_relaxed)});
                img = &it2->second.img;

                // If not already loading, and we have room for another
                // concurrent load, start one. If we're at the cap, we
                // simply don't start a thread this frame -- ComputeRequirement()
                // runs again next frame (redraws are triggered whenever any
                // in-flight load finishes), so this naturally retries until
                // a slot opens up, without needing a real queue.
                if (inflight_.find(url_) == inflight_.end() &&
                    inflight_count_.load(std::memory_order_relaxed) < max_concurrent_loads_) {
                    inflight_[url_] = true;
                    inflight_count_.fetch_add(1, std::memory_order_relaxed);

                    std::string url_copy = url_;

                    std::thread([url_copy]() {
                        cimg_library::CImg<unsigned char> loaded;
                        try {
                            loaded = LoadImageRGB(url_copy.c_str());
                        } catch (...) {
                            loaded = black_img;
                        }

                        std::lock_guard<std::mutex> lock(mutex_);

                        image_cache_.insert_or_assign(
                            url_copy,
                            ImageEntry{std::move(loaded),
                                       g_next_version.fetch_add(1, std::memory_order_relaxed)});

                        inflight_.erase(url_copy);
                        inflight_count_.fetch_sub(1, std::memory_order_relaxed);

                        if (on_loaded_) {
                            on_loaded_();
                        }
                    }).detach();
                }
            }

            requirement_.min_x = img->width() / 4;
            requirement_.min_y = img->height() / 8;
            // Note: `img` is only valid while `lock` is held (it points
            // into a cache entry that another thread could otherwise
            // evict/overwrite), so we're careful to finish using it
            // before the lock_guard above goes out of scope.
        }
    }

    void Render(Screen& screen) override {
        // Everything below touches the shared caches (directly, or via
        // pointers into them), so we hold the lock for the whole
        // function rather than releasing and re-acquiring it partway
        // through. The extra critical-section time here is negligible:
        // it only ever contends with a background loader thread
        // committing a finished decode, never with other widgets'
        // Render() calls, since FTXUI drives layout and rendering from a
        // single thread.
        std::lock_guard<std::mutex> lock(mutex_);

        auto origin_image_width = (box_.x_max - box_.x_min + 1) * 4;
        auto origin_image_height = (box_.y_max - box_.y_min + 1) * 8;

        // NOTE: we deliberately don't use image_cache_.at(url_) here. This
        // widget's own ComputeRequirement() (earlier in this same frame)
        // guarantees url_ was in image_cache_ at that point -- but FTXUI
        // computes requirements for the *entire* tree before rendering any
        // of it, so if the number of distinct images on screen is close to
        // or over image_cache_'s capacity, a later widget's
        // ComputeRequirement() can evict this one's entry before we get
        // here. at() would throw std::out_of_range in that case, which
        // nothing catches on the way back up through FTXUI's render loop --
        // an uncaught exception there is a guaranteed std::terminate() /
        // SIGABRT, not a graceful failure. find() + a placeholder fallback
        // means a too-small cache costs you a flickered-back-to-black
        // thumbnail for a frame, not a crash. If you're seeing this happen
        // often, raise the caches with setImageCacheMaxSize() below rather
        // than relying on this fallback as the normal path.
        auto cache_it = image_cache_.find(url_);
        if (cache_it == image_cache_.end()) {
            cache_it = image_cache_.emplace(
                url_, ImageEntry{black_img, g_next_version.fetch_add(1, std::memory_order_relaxed)}).first;
        }
        auto& entry = cache_it->second;
        const cimg_library::CImg<unsigned char>* original = &entry.img;
        uint64_t version = entry.version;

        FitSize container_size(origin_image_width, origin_image_height);
        FitSize new_size = FitSize(original->_width, original->_height).fitted_within(container_size);
        ResizeKey key{url_hash_, new_size.width, new_size.height, version};

        auto it = resized_cache_.find(key);
        if (it != resized_cache_.end()) {
            original = &it->second;
        } else {
            auto img = original->get_resize(
                new_size.width, new_size.height, -100, -100, 5
            );

            it = resized_cache_.emplace(key, std::move(img)).first;
            original = &it->second;
        }

        auto get_pixel = [original](int row, int col) -> unsigned long {
            return (((unsigned long) (*original)(row, col, 0, 0)) << 16)
                | (((unsigned long) (*original)(row, col, 0, 1)) << 8)
                | (((unsigned long) (*original)(row, col, 0, 2)));
        };

        auto screen_y = box_.y_min;

        for (uint16_t y = 0; y <= original->height() - 8; y += 8) {
            auto screen_x = box_.x_min;

            for (uint16_t x = 0; x <= original->width() - 4; x += 4) {
                if(screen_x > box_.x_max)
                    break;

                CharKey char_key{
                    url_hash_,
                    x,
                    y,
                    version,
                    static_cast<uint16_t>(new_size.width),
                    static_cast<uint16_t>(new_size.height)
                };

                const CharData* charData;

                auto cache_it = char_cache_.find(char_key);
                if (cache_it != char_cache_.end()) {
                    charData = &cache_it->second;
                } else {
                    auto [new_it, _] = char_cache_.emplace(char_key, findCharData(get_pixel, x, y, kCharSelectionFlags));
                    charData = &new_it->second;
                }

                std::stringstream output;

                ftxui::Color bgColor(charData->bgColor[0], charData->bgColor[1], charData->bgColor[2]);
                ftxui::Color fgColor(charData->fgColor[0], charData->fgColor[1], charData->fgColor[2]);

                WriteUtf8Codepoint(output, charData->codePoint);

                auto cell = ftxui::Cell();

                cell.background_color = bgColor;
                cell.foreground_color = fgColor;
                cell.character = output.str();

                screen.PixelAt(screen_x++, screen_y) = cell;
            }
            ++screen_y;
        }
    }

private:
    std::string url_;
};

}  // namespace

void setOnImageLoadedCallback(std::function<void()> cb) {
    ImageView::on_loaded_ = std::move(cb);
}

void setImageCacheMaxSize(size_t max_entries) {
    std::lock_guard<std::mutex> lock(ImageView::mutex_);
    ImageView::image_cache_.set_max_size(max_entries);
}

void setImageResizeCacheMaxSize(size_t max_entries) {
    std::lock_guard<std::mutex> lock(ImageView::mutex_);
    ImageView::resized_cache_.set_max_size(max_entries);
}

void setImageCharCacheMaxSize(size_t max_entries) {
    std::lock_guard<std::mutex> lock(ImageView::mutex_);
    ImageView::char_cache_.set_max_size(max_entries);
}

void setMaxConcurrentImageLoads(int max_concurrent) {
    std::lock_guard<std::mutex> lock(ImageView::mutex_);
    ImageView::max_concurrent_loads_ = std::max(1, max_concurrent);
}

Element image_view(std::string_view url) {
    return std::make_shared<ImageView>(url);
}

}  // namespace ftxui
