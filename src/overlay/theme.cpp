#include "overlay/theme.h"

#if CF_HAS_IMGUI

namespace cf::ov::theme {

static ImVec4 Hex(unsigned argb){
    float a = ((argb>>24) & 0xFF) / 255.f;
    float r = ((argb>>16) & 0xFF) / 255.f;
    float g = ((argb>> 8) & 0xFF) / 255.f;
    float b = ( argb      & 0xFF) / 255.f;
    return ImVec4(r, g, b, a);
}

void Apply(){
    auto& s = ImGui::GetStyle();

    // Geometry — soft but not round; one radius token.
    s.WindowRounding      = 10.f;
    s.ChildRounding       = 8.f;
    s.FrameRounding       = 6.f;
    s.PopupRounding       = 8.f;
    s.ScrollbarRounding   = 6.f;
    s.GrabRounding        = 6.f;
    s.TabRounding         = 6.f;

    s.WindowPadding       = ImVec2(14, 12);
    s.FramePadding        = ImVec2(10, 6);
    s.ItemSpacing         = ImVec2(10, 7);
    s.ItemInnerSpacing    = ImVec2(6, 4);
    s.IndentSpacing       = 14.f;
    s.ScrollbarSize       = 10.f;
    s.GrabMinSize         = 12.f;

    s.WindowBorderSize    = 0.f;
    s.ChildBorderSize     = 0.f;
    s.PopupBorderSize     = 0.f;
    s.FrameBorderSize     = 0.f;
    s.TabBorderSize       = 0.f;

    s.WindowTitleAlign    = ImVec2(0.0f, 0.5f);
    s.ButtonTextAlign     = ImVec2(0.5f, 0.5f);

    ImVec4* c = s.Colors;
    c[ImGuiCol_Text]                 = Hex(col::text);
    c[ImGuiCol_TextDisabled]         = Hex(col::textDim);
    c[ImGuiCol_WindowBg]             = Hex(col::bg);
    c[ImGuiCol_ChildBg]              = Hex(col::bg);
    c[ImGuiCol_PopupBg]              = Hex(col::bg2);
    c[ImGuiCol_Border]               = Hex(col::stroke);
    c[ImGuiCol_BorderShadow]         = ImVec4(0,0,0,0);
    c[ImGuiCol_FrameBg]              = Hex(col::surf);
    c[ImGuiCol_FrameBgHovered]       = Hex(col::surfHi);
    c[ImGuiCol_FrameBgActive]        = Hex(col::surfHi);
    c[ImGuiCol_TitleBg]              = Hex(col::bg2);
    c[ImGuiCol_TitleBgActive]        = Hex(col::bg2);
    c[ImGuiCol_TitleBgCollapsed]     = Hex(col::bg2);
    c[ImGuiCol_MenuBarBg]            = Hex(col::bg2);
    c[ImGuiCol_ScrollbarBg]          = Hex(col::bg);
    c[ImGuiCol_ScrollbarGrab]        = Hex(col::surf);
    c[ImGuiCol_ScrollbarGrabHovered] = Hex(col::surfHi);
    c[ImGuiCol_ScrollbarGrabActive]  = Hex(col::accent2);
    c[ImGuiCol_CheckMark]            = Hex(col::accent);
    c[ImGuiCol_SliderGrab]           = Hex(col::accent);
    c[ImGuiCol_SliderGrabActive]     = Hex(col::accent);
    c[ImGuiCol_Button]               = Hex(col::surf);
    c[ImGuiCol_ButtonHovered]        = Hex(col::surfHi);
    c[ImGuiCol_ButtonActive]         = Hex(col::accent2);
    c[ImGuiCol_Header]               = Hex(col::surf);
    c[ImGuiCol_HeaderHovered]        = Hex(col::surfHi);
    c[ImGuiCol_HeaderActive]         = Hex(col::accent2);
    c[ImGuiCol_Separator]            = Hex(col::stroke);
    c[ImGuiCol_SeparatorHovered]     = Hex(col::accent);
    c[ImGuiCol_SeparatorActive]      = Hex(col::accent);
    c[ImGuiCol_ResizeGrip]           = ImVec4(0,0,0,0);
    c[ImGuiCol_ResizeGripHovered]    = Hex(col::accent2);
    c[ImGuiCol_ResizeGripActive]     = Hex(col::accent);
    c[ImGuiCol_Tab]                  = Hex(col::surf);
    c[ImGuiCol_TabHovered]           = Hex(col::surfHi);
    c[ImGuiCol_TabActive]            = Hex(col::bg);
    c[ImGuiCol_TabUnfocused]         = Hex(col::surf);
    c[ImGuiCol_TabUnfocusedActive]   = Hex(col::bg);
    c[ImGuiCol_PlotLines]            = Hex(col::accent);
    c[ImGuiCol_PlotLinesHovered]     = Hex(col::accent2);
    c[ImGuiCol_PlotHistogram]        = Hex(col::accent);
    c[ImGuiCol_PlotHistogramHovered] = Hex(col::accent2);
    c[ImGuiCol_TextSelectedBg]       = Hex(col::accent2);
    c[ImGuiCol_NavHighlight]         = Hex(col::accent);
    c[ImGuiCol_NavWindowingHighlight]= Hex(col::accent);
    c[ImGuiCol_NavWindowingDimBg]    = ImVec4(0,0,0,0.5f);
    c[ImGuiCol_ModalWindowDimBg]     = ImVec4(0,0,0,0.55f);
}

} // namespace cf::ov::theme

#else

namespace cf::ov::theme { void Apply(){} }

#endif
