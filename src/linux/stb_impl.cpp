// stb_impl.cpp (Linux build only): the two stb libraries' code, compiled once here (the menu's
// fonts and its logo; public domain / MIT, see the end of each header).
#define STB_TRUETYPE_IMPLEMENTATION
#include "third_party/stb_truetype.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#include "third_party/stb_image.h"
