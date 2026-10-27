#include "pc_launcher_home.h"
#include "pc_ui_theme.h"
#include "pc_ui_actions.h"
#include "pc_mods.h"
#include "pc_ui_internal.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#ifndef DEFJAM_VERSION
#define DEFJAM_VERSION "dev"
#endif

// One row (or grid) of utility buttons; `first`..`last` index into the labels below.
static void ui_home_utilities(UiHomeResult &r, const char *focus, int first, int last, int columns,
                              const char *table, float button_height = 44)
{
    static const char *labels[] = {"Advanced", "Mods", "Open Logs", "Open Folder", "Update / Repair"};
    // Advanced reads as "All settings" on the Fight Night Home; its target name stays "Advanced".
    static const char *captions[] = {"All settings", "Mods", "Open Logs", "Open Folder", "Update / Repair"};
    float s = ImGui::GetStyle().FontScaleMain;
    if (!ImGui::BeginTable(table, columns, ImGuiTableFlags_SizingStretchSame))
        return;
    for (int i = first; i <= last; i++)
    {
        ImGui::TableNextColumn();
        char id[48];
        snprintf(id, sizeof id, "%s##%s", captions[i], labels[i]);
        bool hit = ImGui::Button(id, ImVec2(-1, button_height * s));
        ui_target(labels[i]);
        if (focus && !strcmp(focus, labels[i]))
            ImGui::SetKeyboardFocusHere(-1);
        if (i == 0 && hit)
            r.route = 1;
        if (i == 1 && hit)
            r.route = 3;
        if (i == 2 && hit)
            pcui::open_path(pcui::folder_path(pcui::Folder::Logs));
        if (i == 3 && hit)
            ImGui::OpenPopup("folders");
        if (i == 3 && ImGui::BeginPopup("folders"))
        {
            const char *names[] = {"Data", "Saves", "Mods", "Installation", "Settings file"};
            pcui::Folder kinds[] = {pcui::Folder::Data, pcui::Folder::Saves, pcui::Folder::Mods,
                                    pcui::Folder::Install, pcui::Folder::Settings};
            for (int j = 0; j < 5; j++)
                if (ImGui::MenuItem(names[j]))
                    pcui::open_path(pcui::folder_path(kinds[j]), j == 4);
            ImGui::EndPopup();
        }
        if (i == 4 && hit)
            r.route = 4;
    }
    ImGui::EndTable();
}

static void ui_home_controls_row(UiHomeResult &r, const char *focus)
{
    if (ImGui::Button("Configure controls"))
        r.route = 2;
    ui_target("Configure");
    if (focus && !strcmp(focus, "Configure"))
        ImGui::SetKeyboardFocusHere(-1);
}

// Fight Night poster panel: stripes, kicker, two-line headline, PLAY and Online.
static void ui_home_poster(UiHomeResult &r, float w, float h)
{
    float s = ImGui::GetStyle().FontScaleMain, pad = 40 * s;
    ImGui::BeginChild("hero", ImVec2(w, h), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground);
    ImVec2 a = ImGui::GetCursorScreenPos();
    pcui::stripes(a, ImVec2(w, h), s);

    // Headline sized to the panel: two tightly set lines of display type above the button row.
    // Lines are placed explicitly; the display font's own line height spaces them too far apart.
    float buttons = 72 * s, kicker = 22 * s, leading = .9f;
    float title = std::min({132 * s, (w - 2 * pad) / 3.4f, (h - 2 * pad - buttons - kicker * 1.4f - 40 * s) / (2 * leading + .25f)});
    float block = kicker * 1.4f + title * (leading + 1.1f) + 28 * s + buttons;
    float y = a.y + h - pad - block;
    ImGui::SetCursorScreenPos(ImVec2(a.x + pad, y));
    ImGui::PushStyleColor(ImGuiCol_Text, pcui::fight_red());
    pcui::heading("TONIGHT IN NEW YORK", kicker);
    ImGui::PopStyleColor();
    y += kicker * 1.4f;
    ImGui::SetCursorScreenPos(ImVec2(a.x + pad, y));
    pcui::heading("FIGHT", title);
    y += title * leading;
    ImGui::SetCursorScreenPos(ImVec2(a.x + pad, y));
    pcui::heading("FOR NY", title);
    y += title * 1.1f + 28 * s;

    ImGui::SetCursorScreenPos(ImVec2(a.x + pad, y));
    ImGui::BeginDisabled(pcui::mod_busy() || pcui::mod_recovery_needed());
    r.play = pcui::accent_button("PLAY", ImVec2(std::min(240 * s, w * .4f), buttons), 40, pcui::fight_red());
    ui_target("Play");
    ImGui::EndDisabled();
    if (ImGui::IsWindowAppearing())
        ImGui::SetItemDefaultFocus();
    ImGui::SameLine(0, 16 * s);
    // Online play is planned; keep its place in the layout without a dead control.
    ImGui::BeginDisabled(true);
    pcui::accent_button("ONLINE MATCH", ImVec2(std::min(220 * s, w * .35f), buttons), 22, pcui::fight_red(), true);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Online play is coming in a future update.");
    ImGui::EndChild();
}

UiHomeResult ui_launcher_home(float width, float height, const char *focus)
{
    UiHomeResult r;
    float s = ImGui::GetStyle().FontScaleMain, gap = 20 * s;
    bool split = width >= 850 * s;
    if (split)
    {
        float body = std::max(height, 410 * s);
        // Wide enough for ui_home_fields' two-column grid: it needs 390 px of content width,
        // after this child's padding and a possible scrollbar.
        float right = std::clamp(width * .44f, 470 * s, 520 * s);
        ui_home_poster(r, width - right - gap, body);
        ImGui::SameLine(0, gap);
        ImGui::BeginChild("quick settings", ImVec2(0, body),
                          ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
        pcui::heading("CORNER SETUP", 25 * s);
        ImGui::Spacing();
        ui_home_fields();
        ImGui::Separator();
        ui_home_devices();
        ui_home_controls_row(r, focus);
        ui_home_utilities(r, focus, 0, 1, 2, "main utilities");
        ui_home_utilities(r, focus, 2, 4, 3, "file utilities", 36);
        ImGui::EndChild();
        return r;
    }

    // Narrow windows: the launcher footer carries PLAY (pc_ui.cpp), so Home lists settings only.
    pcui::heading("FIGHT FOR NY", 32 * s);
    ImGui::Spacing();
    ImGui::BeginChild("quick settings", ImVec2(0, 0),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
    pcui::heading("CORNER SETUP", 25 * s);
    ImGui::Spacing();
    ui_home_fields();
    ImGui::Separator();
    ui_home_devices();
    ui_home_controls_row(r, focus);
    ImGui::EndChild();
    ImGui::Spacing();
    ui_home_utilities(r, focus, 0, 4, std::max(1, std::min(5, (int)(width / (145 * s)))), "utilities");
    return r;
}
