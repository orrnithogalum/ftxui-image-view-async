#include <ftxui/component/event.hpp>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>
#include <vector>
#include <random>
#include "ftxui/component/screen_interactive.hpp"
#include "ftxui/component/component.hpp"
#include "image_view.hpp"

namespace fs = std::filesystem;

namespace {

// Case-insensitive "does `s` start with `prefix`?" check.
bool StartsWithCaseInsensitive(const std::string& s, const std::string& prefix) {
    if (s.size() < prefix.size()) return false;
    return std::equal(prefix.begin(), prefix.end(), s.begin(), [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) ==
               std::tolower(static_cast<unsigned char>(b));
    });
}

bool IsUrl(const std::string& s) {
    return StartsWithCaseInsensitive(s, "http://") ||
           StartsWithCaseInsensitive(s, "https://");
}

// Common web/photo image extensions. Whether a given one actually decodes
// depends on how the underlying image library was built (e.g. CImg needs
// libpng/libjpeg compiled in, or falls back to an external `convert`/`gm`
// if available) -- this list just controls which files we *offer* to
// image_view, not what it can actually decode.
bool HasImageExtension(const fs::path& path) {
    static const std::vector<std::string> kExtensions = {
        ".png", ".jpg", ".jpeg", ".bmp", ".gif", ".tga",
    };
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return std::find(kExtensions.begin(), kExtensions.end(), ext) != kExtensions.end();
}

// Expands one command-line argument into zero or more image references:
//   - an http(s) URL is taken as-is (no local filesystem check makes sense
//     for it, and we don't want to make a network request just to
//     validate it at startup)
//   - a local directory is scanned (non-recursively) for files with a
//     recognized image extension
//   - a local file is taken as-is, regardless of extension -- if you
//     named it explicitly, we trust you
//   - anything else is reported and skipped, rather than silently
//     dropped or crashing the scan
void CollectPictures(const std::string& arg, std::vector<std::string>& out) {
    if (IsUrl(arg)) {
        out.push_back(arg);
        return;
    }

    std::error_code ec;
    if (fs::is_directory(arg, ec)) {
        for (const auto& entry : fs::directory_iterator(arg, ec)) {
            if (entry.is_regular_file() && HasImageExtension(entry.path())) {
                out.push_back(entry.path().native());
            }
        }
        if (ec) {
            std::cerr << "Warning: couldn't fully read directory '" << arg
                       << "': " << ec.message() << std::endl;
        }
        return;
    }

    if (fs::is_regular_file(arg, ec)) {
        out.push_back(arg);
        return;
    }

    std::cerr << "Warning: '" << arg
               << "' is not an http(s) URL, a file, or a directory -- skipping."
               << std::endl;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0]
                   << " <picture-dir | image-file | image-url> [more...]"
                   << std::endl;
        std::cerr << "  Mix and match: local folders, individual local "
                     "images, and http(s) image URLs are all accepted, in "
                     "any combination."
                   << std::endl;
        return 1;
    }

    std::vector<std::string> pictures;
    for (int i = 1; i < argc; ++i) {
        CollectPictures(argv[i], pictures);
    }

    if (pictures.empty()) {
        std::cerr << "No pictures found across the given arguments -- nothing to show."
                   << std::endl;
        return 1;
    }

    auto cell = [](const std::string& path) { return ftxui::image_view(path); };
    int displayIndex{0};
    auto catDisplay = ftxui::Renderer([&] {
        return cell(pictures[displayIndex]);
    });
    auto selectButton = ftxui::Button("Click", [&] {
        std::random_device seed;
        std::default_random_engine en(seed());
        std::uniform_int_distribution<> dist(0, static_cast<int>(pictures.size()) - 1);
        displayIndex = dist(en);
    });
    auto mainPannelContainer = ftxui::Container::Vertical({
        catDisplay,
        selectButton
    });
    auto screen = ftxui::ScreenInteractive::FitComponent();
    ftxui::setOnImageLoadedCallback([&]() {
        screen.PostEvent(ftxui::Event::Custom);
    });
    auto mainPannelRender = ftxui::Renderer(mainPannelContainer, [&] {
        return ftxui::vbox({
            catDisplay->Render() | ftxui::size(ftxui::WIDTH, ftxui::LESS_THAN, 60),
            ftxui::separator(),
            selectButton->Render() | ftxui::size(ftxui::HEIGHT, ftxui::GREATER_THAN, 10)
        });
    });
    screen.Loop(mainPannelRender);
}
