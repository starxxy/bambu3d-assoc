#include "bs3d/formats.h"
#include "bs3d/winutil.h"

#include <vector>

namespace bs3d {

namespace {

// 仅收录 Bambu Studio 实际能打开的格式（经其程序内置对话框字符串核实）：
//   模型导入: 3mf / step / stl / obj / amf / fbx    工程: 3mf    G-code: gcode / g / gco
// 行序与文案逐字对齐设计稿 ui_draft_glassmorphism.html 的 .list
//（设计稿是唯一真相；名称/说明/类型标签三列都以它为准）。
const FormatDef kFormats[] = {
    {L"3mf",  L"Bambu Studio 项目", L"三维制造格式（可选内嵌封面）", L"切片输出", Kind::EmbeddedImage, true,  true},
    {L"stl",  L"立体光刻",          L"网格 · 无材质",             L"网格",     Kind::Mesh,          false, true},
    {L"obj",  L"Wavefront 模型",    L"网格 · 支持法线",           L"网格",     Kind::Mesh,          false, true},
    {L"step", L"CAD 模型",          L"CAD 内核随安装包自带",      L"CAD 参数化", Kind::Cad,         false, true},
    {L"fbx",  L"Autodesk FBX",      L"二进制 / 文本双格式",       L"网格",     Kind::Mesh,          false, true},
    {L"amf",  L"增材制造格式",      L"XML · 索引约定自动识别",    L"网格",     Kind::Mesh,          true,  true},
    {L"gcode",L"切片输出",          L"仅显示首层预览",            L"切片输出", Kind::Toolpath,      false, true},
    // 别名（与上表同解析器，排在主用格式之后）
    {L"stp",  L"CAD 模型",          L"CAD 内核随安装包自带",      L"CAD 参数化", Kind::Cad,         false, true},
    {L"g",    L"切片输出",          L"仅显示首层预览",            L"切片输出", Kind::Toolpath,      false, true},
    {L"gco",  L"切片输出",          L"仅显示首层预览",            L"切片输出", Kind::Toolpath,      false, true},
};

std::vector<const FormatDef*>* s_all = nullptr;

const wchar_t* kLegacy[] = {
    L"ply", L"3ds", L"off", L"dae", L"x3d", L"wrl",
    L"iges", L"igs", L"brep", L"bgcode",
};

}  // namespace

const std::vector<const FormatDef*>& allFormats() {
    if (!s_all) {
        s_all = new std::vector<const FormatDef*>();
        for (size_t i = 0; i < std::end(kFormats) - std::begin(kFormats); ++i) {
            s_all->push_back(&kFormats[i]);
        }
    }
    return *s_all;
}

const std::vector<std::wstring>& legacyExts() {
    static std::vector<std::wstring>* s = nullptr;
    if (!s) {
        s = new std::vector<std::wstring>();
        for (size_t i = 0; i < std::end(kLegacy) - std::begin(kLegacy); ++i) {
            s->push_back(kLegacy[i]);
        }
    }
    return *s;
}

std::wstring extOf(const std::wstring& path) {
    size_t dot = path.find_last_of(L'.');
    size_t slash = path.find_last_of(L"\\/");
    if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash)) return L"";
    return lowerW(path.substr(dot));
}

std::wstring baseName(const std::wstring& path) {
    size_t s = path.find_last_of(L"\\/");
    return s == std::wstring::npos ? path : path.substr(s + 1);
}

const FormatDef* formatOfExt(const std::wstring& extIn) {
    std::wstring e = lowerW(extIn);
    if (!e.empty() && e[0] == L'.') e.erase(0, 1);
    if (e.empty()) return nullptr;
    for (const FormatDef* f : allFormats()) {
        if (lowerW(f->ext) == e) return f;
    }
    return nullptr;
}

const FormatDef* formatOfPath(const std::wstring& path) { return formatOfExt(extOf(path)); }

}  // namespace bs3d
