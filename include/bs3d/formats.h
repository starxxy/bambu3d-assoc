#pragma once

#include "bs3d.h"

namespace bs3d {

enum class Kind : int {
    Mesh = 0,        // polygon mesh, software rendered
    Cad = 1,         // parametric CAD (STEP / IGES / BREP)
    Toolpath = 2,    // gcode style layer paths, drawn top down
    EmbeddedImage = 3,  // container that always carries an image
    Unknown = 99,
};

// One row of the supported format table. `ext` is lower case without a dot.
struct FormatDef {
    const wchar_t* ext;
    const wchar_t* displayName;  // 列表里的短名称（v1.2.2 起改成口语化短名）
    const wchar_t* desc;         // 列表里的第二行小灰字
    const wchar_t* group;        // "网格" / "CAD 参数化" / "切片输出"
    Kind kind;
    bool mayEmbedImage;     // parser may find a preview image inside the file
    bool thumbnailable;     // thumbnail support implemented for this extension
};

const FormatDef* formatOfPath(const std::wstring& path);
const FormatDef* formatOfExt(const std::wstring& ext);
const std::vector<const FormatDef*>& allFormats();

// Extensions handled by older versions (1.0.0) but no longer in the table,
// kept so "restore all" can clean up their leftover associations. Dot-less.
const std::vector<std::wstring>& legacyExts();

std::wstring extOf(const std::wstring& path);   // ".3mf", "" if none
std::wstring baseName(const std::wstring& path);

}  // namespace bs3d
