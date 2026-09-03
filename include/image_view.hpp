#pragma once

#include "ftxui/dom/node.hpp"
#include <cstddef>

#include <cstdint>

namespace ftxui {

Element image_view(std::string_view url);
void setOnImageLoadedCallback(std::function<void()> cb);

// Tune the bounded caches backing image_view. Sizes are entry counts, not
// bytes. Safe to call at any time (they take the same internal lock as
// everything else), but calling once near startup, before your first
// render, is the typical usage.
//
//   image_cache_  -- full-resolution decoded originals (the big ones)
//   resized_cache_ -- copies scaled down to a drawn terminal-cell size
//   char_cache_   -- one small struct per 4x8-pixel terminal cell
//
// If your app can have many distinct thumbnails on screen at once (a
// grid, several carousels, a queue view, a playback bar, etc. all
// showing images simultaneously), raise these -- a too-small cache
// won't crash the app, but it will mean images keep getting evicted
// and re-decoded/re-resized instead of staying cached.
void setImageCacheMaxSize(size_t max_entries);
void setImageResizeCacheMaxSize(size_t max_entries);
void setImageCharCacheMaxSize(size_t max_entries);

// Caps how many images can be decoding on background threads at once.
// Each in-flight load is a real OS thread (and, for network URLs, often
// a curl/wget subprocess), so this bounds resource use when a burst of
// many distinct images needs loading at the same time -- e.g. an app
// opening on a screen with 40+ uncached thumbnails. Requests beyond the
// cap simply wait their turn and start as soon as a slot frees up.
void setMaxConcurrentImageLoads(int max_concurrent);

}
