#pragma once
#include "raylib.h"
// Decode a WebP file with libwebp and upload RGBA pixels to raylib.
Texture2D LoadWebPTexture(const char* path);
