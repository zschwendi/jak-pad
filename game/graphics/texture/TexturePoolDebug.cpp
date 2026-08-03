#include "game/graphics/texture/TexturePool.h"

#include <regex>

#include "fmt/format.h"
#include "third-party/imgui/imgui.h"

void TexturePool::draw_debug_window() {
  int id = 0;
  int total_vram_bytes = 0;
  int total_textures = 0;
  int total_displayed_textures = 0;
  int total_uploaded_textures = 0;
  ImGui::InputText("texture search", m_regex_input, sizeof(m_regex_input));
  bool use_regex = m_regex_input[0];
  std::regex regex(use_regex ? m_regex_input : ".*");

  for (size_t i = 0; i < m_textures.size(); i++) {
    auto& record = m_textures[i];
    total_textures++;
    if (record.source) {
      if (!use_regex || std::regex_search(get_debug_texture_name(record.source->tex_id), regex)) {
        ImGui::PushID(id++);
        draw_debug_for_tex(get_debug_texture_name(record.source->tex_id), record.source, i);
        ImGui::PopID();
        total_displayed_textures++;
      }
      if (!record.source->gpu_textures.empty()) {
        total_vram_bytes +=
            record.source->w * record.source->h * 4;  // todo, if we support other formats
      }

      total_uploaded_textures++;
    }
  }

  // todo mt4hh
  ImGui::Text("Total Textures: %d Uploaded: %d Shown: %d VRAM: %.3f MB", total_textures,
              total_uploaded_textures, total_displayed_textures,
              (float)total_vram_bytes / (1024 * 1024));
}

void TexturePool::draw_debug_for_tex(const std::string& name, GpuTexture* tex, u32 slot) {
  if (tex->is_placeholder) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.8, 0.3, 0.3, 1.0));
  } else if (tex->gpu_textures.size() == 1) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.3, 0.8, 0.3, 1.0));
  } else {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.8, 0.8, 0.3, 1.0));
  }
  if (ImGui::TreeNode(fmt::format("{}) {}", slot, name).c_str())) {
    ImGui::Text("P: %s sz: %d x %d", get_debug_texture_name(tex->tex_id).c_str(), tex->w, tex->h);
    if (!tex->is_placeholder) {
      ImGui::Image((ImTextureID)(intptr_t)tex->gpu_textures.at(0).gl, ImVec2(tex->w, tex->h));
    } else {
      ImGui::Text("PLACEHOLDER");
    }

    ImGui::TreePop();
    ImGui::Separator();
  }
  ImGui::PopStyleColor();
}
