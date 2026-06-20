#pragma once

#include "SceneTypes.h"

#include <string>

namespace rb
{
struct SceneImportResult
{
    bool succeeded = false;
    std::string diagnostics;
    ImportedScene scene;
};

class SceneImporter
{
public:
    SceneImportResult ImportScene(const std::wstring& path);
};
}
