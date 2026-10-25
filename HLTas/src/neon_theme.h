// neon_theme.h — тема ImGui для HL // TAS.
// Две палитры: 0 = Neon (чёрный + циан/маджента), 1 = Mono (графит + белый акцент).
// Использование: после ImGui::CreateContext() вызови Neon::SetTheme(0 или 1).
// Смена темы на лету: просто вызови Neon::SetTheme(...) ещё раз.
#pragma once
#include "imgui.h"

namespace Neon {

    namespace Palette {
        // Цвета меняются в SetPalette(); Cyan — основной акцент, Magenta — вторичный/предупреждения.
        inline ImVec4 Bg, Sidebar, Card, Border, Cyan, CyanDim, Magenta, MagentaDim, Text, TextDim;
    }

    inline ImVec4 WithAlpha(ImVec4 c, float a) { c.w = a; return c; }

    inline void SetPalette(int variant)
    {
        using namespace Palette;
        if (variant == 1) {          // Mono
            Bg = ImVec4(0.050f, 0.050f, 0.056f, 0.97f);
            Sidebar = ImVec4(0.038f, 0.038f, 0.044f, 1.00f);
            Card = ImVec4(0.086f, 0.086f, 0.096f, 1.00f);
            Border = ImVec4(1.000f, 1.000f, 1.000f, 0.08f);
            Cyan = ImVec4(0.93f, 0.93f, 0.95f, 1.00f);
            CyanDim = ImVec4(0.52f, 0.52f, 0.58f, 1.00f);
            Magenta = ImVec4(0.98f, 0.42f, 0.42f, 1.00f);
            MagentaDim = ImVec4(0.60f, 0.26f, 0.26f, 1.00f);
            Text = ImVec4(0.90f, 0.90f, 0.92f, 1.00f);
            TextDim = ImVec4(0.48f, 0.48f, 0.53f, 1.00f);
        }
        else {                     // Neon
            Bg = ImVec4(0.040f, 0.040f, 0.056f, 0.97f);
            Sidebar = ImVec4(0.028f, 0.028f, 0.042f, 1.00f);
            Card = ImVec4(0.072f, 0.070f, 0.098f, 1.00f);
            Border = ImVec4(0.000f, 0.930f, 1.000f, 0.20f);
            Cyan = ImVec4(0.00f, 0.93f, 1.00f, 1.00f);
            CyanDim = ImVec4(0.00f, 0.55f, 0.62f, 1.00f);
            Magenta = ImVec4(1.00f, 0.15f, 0.85f, 1.00f);
            MagentaDim = ImVec4(0.60f, 0.08f, 0.50f, 1.00f);
            Text = ImVec4(0.88f, 0.95f, 1.00f, 1.00f);
            TextDim = ImVec4(0.45f, 0.52f, 0.60f, 1.00f);
        }
    }

    inline void Apply()
    {
        using namespace Palette;
        ImGuiStyle& s = ImGui::GetStyle();

        s.WindowRounding = 12.0f;
        s.ChildRounding = 10.0f;
        s.FrameRounding = 7.0f;
        s.PopupRounding = 8.0f;
        s.GrabRounding = 10.0f;
        s.TabRounding = 6.0f;
        s.ScrollbarRounding = 8.0f;
        s.ScrollbarSize = 8.0f;
        s.GrabMinSize = 14.0f;
        s.WindowBorderSize = 1.0f;
        s.ChildBorderSize = 1.0f;
        s.FrameBorderSize = 0.0f;
        s.PopupBorderSize = 1.0f;
        s.WindowPadding = ImVec2(12, 10);
        s.FramePadding = ImVec2(10, 6);
        s.ItemSpacing = ImVec2(10, 4);
        s.ItemInnerSpacing = ImVec2(8, 4);

        ImVec4* c = s.Colors;
        c[ImGuiCol_Text] = Text;
        c[ImGuiCol_TextDisabled] = TextDim;
        c[ImGuiCol_WindowBg] = Bg;
        c[ImGuiCol_ChildBg] = Card;
        c[ImGuiCol_PopupBg] = ImVec4(Card.x, Card.y, Card.z, 0.99f);
        c[ImGuiCol_Border] = Border;
        c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);

        ImVec4 frame = ImVec4(Card.x * 1.45f + 0.015f, Card.y * 1.45f + 0.015f, Card.z * 1.45f + 0.015f, 1.0f);
        c[ImGuiCol_FrameBg] = frame;
        c[ImGuiCol_FrameBgHovered] = WithAlpha(Cyan, 0.16f);
        c[ImGuiCol_FrameBgActive] = WithAlpha(Cyan, 0.26f);

        c[ImGuiCol_TitleBg] = Sidebar;
        c[ImGuiCol_TitleBgActive] = Sidebar;
        c[ImGuiCol_TitleBgCollapsed] = Sidebar;

        c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
        c[ImGuiCol_ScrollbarGrab] = WithAlpha(CyanDim, 0.55f);
        c[ImGuiCol_ScrollbarGrabHovered] = Cyan;
        c[ImGuiCol_ScrollbarGrabActive] = Magenta;

        c[ImGuiCol_CheckMark] = Cyan;
        c[ImGuiCol_SliderGrab] = Cyan;
        c[ImGuiCol_SliderGrabActive] = Cyan;

        c[ImGuiCol_Button] = frame;
        c[ImGuiCol_ButtonHovered] = WithAlpha(Cyan, 0.30f);
        c[ImGuiCol_ButtonActive] = WithAlpha(Cyan, 0.50f);

        c[ImGuiCol_Header] = WithAlpha(Cyan, 0.20f);
        c[ImGuiCol_HeaderHovered] = WithAlpha(Cyan, 0.30f);
        c[ImGuiCol_HeaderActive] = WithAlpha(Cyan, 0.40f);

        c[ImGuiCol_Separator] = Border;
        c[ImGuiCol_SeparatorHovered] = Cyan;
        c[ImGuiCol_SeparatorActive] = Cyan;

        c[ImGuiCol_ResizeGrip] = ImVec4(0, 0, 0, 0);
        c[ImGuiCol_ResizeGripHovered] = WithAlpha(Cyan, 0.4f);
        c[ImGuiCol_ResizeGripActive] = Cyan;

        c[ImGuiCol_PlotLines] = Cyan;
        c[ImGuiCol_PlotLinesHovered] = Magenta;
        c[ImGuiCol_PlotHistogram] = Magenta;
        c[ImGuiCol_PlotHistogramHovered] = Cyan;

        c[ImGuiCol_TextSelectedBg] = WithAlpha(Cyan, 0.30f);
    }

    inline void SetTheme(int variant)
    {
        SetPalette(variant);
        Apply();
    }

    // Цвет скорости для HUD: акцент на малой скорости -> вторичный цвет на большой
    inline ImVec4 SpeedColor(float speed, float maxSpeed = 1200.0f)
    {
        float t = speed / maxSpeed;
        if (t < 0.f) t = 0.f;
        if (t > 1.f) t = 1.f;
        return ImVec4(Palette::Cyan.x + (Palette::Magenta.x - Palette::Cyan.x) * t,
            Palette::Cyan.y + (Palette::Magenta.y - Palette::Cyan.y) * t,
            Palette::Cyan.z + (Palette::Magenta.z - Palette::Cyan.z) * t,
            1.0f);
    }

} // namespace Neon