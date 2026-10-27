#include "pc_launcher_art.h"
#include "pc_ui_actions.h"
#include "imgui_internal.h"
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#include "stb_image.h"

namespace
{
struct Entry
{
    bool loaded = false;
    LauncherImage image;
    ImTextureData *texture = nullptr;
};
ImGuiContext *g_owner = nullptr;
std::map<std::string, Entry> g_images;

bool read_file(const std::filesystem::path &path, std::vector<unsigned char> &bytes)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;
    bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return !bytes.empty();
}

ImTextureData *decode(const std::vector<unsigned char> &bytes)
{
    int w = 0, h = 0, channels = 0;
    unsigned char *rgba = stbi_load_from_memory(bytes.data(), (int)bytes.size(), &w, &h, &channels, 4);
    if (!rgba)
        return nullptr;
    // Keep textures within what every launcher backend accepts.
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096)
    {
        stbi_image_free(rgba);
        return nullptr;
    }
    auto *tex = IM_NEW(ImTextureData)();
    tex->Create(ImTextureFormat_RGBA32, w, h);
    memcpy(tex->GetPixels(), rgba, (size_t)w * h * 4);
    stbi_image_free(rgba);
    tex->UseColors = true;
    tex->SetStatus(ImTextureStatus_WantCreate);
    ImGui::RegisterUserTexture(tex);
    return tex;
}
} // namespace

const LauncherImage *launcher_image(const char *name)
{
    // Textures belong to one ImGui context (and its renderer); start over in a new one.
    if (g_owner != ImGui::GetCurrentContext())
    {
        g_owner = ImGui::GetCurrentContext();
        g_images.clear();
    }
    Entry &e = g_images[name];
    if (!e.loaded)
    {
        e.loaded = true;
        std::filesystem::path dir = std::filesystem::u8path(pcui::folder_path(pcui::Folder::Data)) / "launcher";
        for (const char *ext : {".png", ".jpg", ".jpeg"})
        {
            std::vector<unsigned char> bytes;
            if (!read_file(dir / (std::string(name) + ext), bytes))
                continue;
            if ((e.texture = decode(bytes)))
            {
                e.image.texture = e.texture->GetTexRef();
                e.image.width = (float)e.texture->Width;
                e.image.height = (float)e.texture->Height;
            }
            break;
        }
    }
    return e.texture ? &e.image : nullptr;
}
