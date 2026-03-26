# An FTXUI Component for Displaying Images

## Preview

![](./doc/example.png)

> [!NOTE]
> This project is based on the original [ftxui-image-view](https://github.com/ljrrjl/ftxui-image-view), with modifications focused on improving performance for TUIs that use images in interactive interfaces.
> Below is an overview of the main features and improvements.

### Async Image Loading

Images are now loaded asynchronously in a separate thread instead of blocking the UI thread.

Be aware that minor "hitches" or "stutters" may occur the first time an image is rendered (not computed) on the UI thread. This happens because the cell cache must be updated, and FTXUI does not handle large screen updates in a single frame efficiently. These stutters can range from ~10ms to 30ms and are more noticeable with large images (e.g., 2K wallpapers).

Since loading happens in a separate thread, the first rendered frame will display a black image. The UI must be manually refreshed for the actual image to appear. You can use `setOnLoadedImageCallback()` for this purpose. See the example in `./example/main.cpp`.

### Caching System

Several layers of caching have been introduced to improve performance:

- **Image cache**  
  Raw images are loaded once per URI instead of being reloaded every frame. They are stored in memory and reused across renders.

- **Size cache**  
  Images are cached per computed size. Previously, resizing occurred every frame; now it only happens when necessary (e.g., when the terminal size changes or the UI updates).

- **Character cache**  
  The expensive `tiv::findCharData` method was previously called every frame, recomputing each screen cell covered by the image. Now, the resulting Unicode characters are cached, significantly reducing rendering cost.

- **Other improvements**  
   Pointer use to prevent images from being copied at each frame, etc.

### Versioning & Invalidation

Each image maintains an internal version counter. This allows caches to be invalidated automatically once an image has been asynchronously updated.

## Dependencies

- [TerminalImageViewer](https://github.com/stefanhaustein/TerminalImageViewer.git): A powerful library used for terminal image rendering. Some parts were originally modified by [ljrrjl](https://github.com/ljrrjl) to integrate with FTXUI (see `./libs`).

For most users, installing `imagemagick` is sufficient:

**Ubuntu:**
```bash
sudo apt install imagemagick
```

**Arch:**
```bash
sudo pacman -S imagemagick
```

## API

A complete usage example is available in `./example/main.cpp`.

Before rendering images, you should register a callback to refresh the UI once an image finishes loading:

```c++
ftxui::setOnLoadedImageCallback([]() {
    /* Trigger a UI refresh when the image is ready */
    screen->postEvent(ftxui::Event::Custom); 
});

Element image_view(std::string_view path) {
    return std::make_shared<ImageView>(path);
}
```
## Building & Running the Example

To try the example locally, clone the repository and build it using CMake:

```bash
git clone https://github.com/orrnithogalum/ftxui-image-view.git
cd ftxui-image-view

cmake -B build -S . -DCMAKE_BUILD_TYPE=Debug 
cmake --build build
```

Once the build completes, run the example with a directory of images:

```bash
# In the ftxui-image-view directory
./build/example/ftxui-imageview-example ./imgs/
```
