// Copyright 2023 ljrrjl. All rights reserved.
// Use of this source code is governed by the MIT license that can be found in
// the LICENSE file.

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

#include "tiv_lib.h"

namespace ftxui {

namespace {

using ftxui::Screen;

using ResizeKey = std::tuple<int, int, int, uint16_t>;
struct ResizeKeyHash {
    std::size_t operator()(const ResizeKey& k) const {
        auto const& [url, w, h, version] = k;
        std::size_t h1 = std::hash<int>{}(url);
        std::size_t h2 = std::hash<int>{}(w);
        std::size_t h3 = std::hash<int>{}(h);
        std::size_t h4 = std::hash<uint16_t>{}(version);
        return h1 ^ (h2 << 1) ^ (h3 << 2) ^ (h4 << 3);
    }
};

struct CharKey {
    uint32_t url;
    uint16_t x;
    uint16_t y;
    uint16_t version;
    uint16_t width;
    uint16_t height;

    bool operator==(const CharKey& other) const {
        return url == other.url &&
               x == other.x &&
               y == other.y &&
               version == other.version &&
               width == other.width &&
               height == other.height;
    }
};
struct CharKeyHash {
    size_t operator()(const CharKey& k) const {
        size_t h1 = std::hash<uint32_t>{}(k.url);
        size_t h2 = std::hash<uint16_t>{}(k.x);
        size_t h3 = std::hash<uint16_t>{}(k.y);
        size_t h4 = std::hash<uint16_t>{}(k.version);
        size_t h5 = std::hash<uint16_t>{}(k.width);
        size_t h6 = std::hash<uint16_t>{}(k.height);

        return h1 ^ (h2 << 1) ^ (h3 << 2) ^ (h4 << 3)
                   ^ (h5 << 4) ^ (h6 << 5);
    }
};

class ImageView: public Node {
public:
    inline static std::unordered_map<ResizeKey, cimg_library::CImg<unsigned char>, ResizeKeyHash> resized_cache_;
    inline static std::unordered_map<std::string, cimg_library::CImg<unsigned char>> cimg_cache_;
    inline static std::unordered_map<CharKey, tiv::CharData, CharKeyHash> char_cache_;
    inline static std::unordered_map<std::string, int> version_;

    inline static std::unordered_map<std::string, bool> inflight_;
    inline static std::mutex mutex_;

    inline static cimg_library::CImg<unsigned char> black_img = cimg_library::CImg<unsigned char>(1, 1, 1, 3, 0);
    inline static std::function<void()> on_loaded_;

    uint32_t url_hash_;

    explicit ImageView(std::string_view url) : url_(url) {
        url_hash_ = std::hash<std::string>{}(url_);
    }

    void ComputeRequirement() override {
        {
            std::lock_guard<std::mutex> lock(mutex_);

            auto it = cimg_cache_.find(url_);
            if (it != cimg_cache_.end()) {
                img_ = &it->second;
            } else {
                // insert black image immediately
                auto [it2, _] = cimg_cache_.emplace(url_, black_img);
                img_ = &it2->second;

                // if not already loading → start async load
                if (!inflight_[url_]) {
                    inflight_[url_] = true;

                    std::string url_copy = url_;

                    std::thread([url_copy]() {
                        cimg_library::CImg<unsigned char> img;
                        try {
                            img = tiv::load_rgb_CImg(url_copy.c_str());
                        } catch (...) {
                            img = black_img;
                        }

                        std::lock_guard<std::mutex> lock(mutex_);

                        cimg_cache_[url_copy] = std::move(img);
                        version_[url_copy]++;

                        inflight_[url_copy] = false;

                        if(on_loaded_) {
                            on_loaded_();
                        }

                    }).detach();
                }
            }
        }

        requirement_.min_x = img_->width() / 4;
        requirement_.min_y = img_->height() / 8;
    }

    void Render(Screen& screen) override {
        auto origin_image_width = (box_.x_max - box_.x_min + 1) * 4;
        auto origin_image_height = (box_.y_max - box_.y_min + 1) * 8;

        const cimg_library::CImg<unsigned char>* original;
        uint16_t version;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            original = &cimg_cache_.at(url_);
            version = version_[url_];
        }

        auto container_size = tiv::size(origin_image_width, origin_image_height);
        auto new_size = tiv::size(original->_width, original->_height).fitted_within(&container_size);
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


                CharKey key{
                    url_hash_,
                    x,
                    y,
                    version,
                    static_cast<uint16_t>(new_size.width),
                    static_cast<uint16_t>(new_size.height)
                };
                const tiv::CharData* charData;

                {
                    std::lock_guard<std::mutex> lock(mutex_);

                    auto cache_it = char_cache_.find(key);
                    if (cache_it != char_cache_.end()) {
                        charData = &cache_it->second;
                    } else {
                        auto [it, _] = char_cache_.emplace(key, tiv::findCharData(get_pixel, x, y, tiv::FLAG_24BIT));
                        charData = &it->second;
                    }
                }

                std::stringstream output;

                ftxui::Color bgColor(charData->bgColor[0], charData->bgColor[1],charData->bgColor[2]);
                ftxui::Color fgColor(charData->fgColor[0], charData->fgColor[1],charData->fgColor[2]);

                tiv::printCodepoint(output, charData->codePoint);

                auto pixel = ftxui::Pixel();

                pixel.background_color = bgColor;
                pixel.foreground_color = fgColor;
                pixel.character = output.str();

                screen.PixelAt(screen_x++, screen_y) = pixel;
            }
            ++screen_y;
        }
    }

private:
    const cimg_library::CImg<unsigned char>* img_ = nullptr;

    int         width_ = 0;
    int         height_ = 0;

    std::string url_;
};

}  // namespace

void setOnImageLoadedCallback(std::function<void()> cb) {
    ImageView::on_loaded_ = std::move(cb);
}

Element image_view(std::string_view url) {
    return std::make_shared<ImageView>(url);
}

}  // namespace ftxui
