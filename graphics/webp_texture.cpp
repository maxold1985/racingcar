#include "webp_texture.hpp"
#include <webp/decode.h>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>
#include <limits>

Texture2D LoadWebPTexture(const char* path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        std::fprintf(stderr, "Cannot open WebP texture: %s\n", path);
        return Texture2D{};
    }
    const std::vector<unsigned char> bytes(
        (std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    int width=0, height=0;
    if (bytes.empty() || !WebPGetInfo(bytes.data(),bytes.size(),&width,&height) ||
        width<=0 || height<=0 || width>16384 || height>16384) {
        std::fprintf(stderr, "Invalid WebP image: %s\n", path);
        return Texture2D{};
    }
    uint8_t* pixels=WebPDecodeRGBA(bytes.data(),bytes.size(),&width,&height);
    if (!pixels) {
        std::fprintf(stderr, "Failed to decode WebP: %s\n", path);
        return Texture2D{};
    }
    Image image{};
    image.data=pixels;
    image.width=width;
    image.height=height;
    image.mipmaps=1;
    image.format=PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
    Texture2D texture=LoadTextureFromImage(image);
    WebPFree(pixels);
    return texture;
}
