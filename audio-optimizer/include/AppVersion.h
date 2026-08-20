// =============================================================================
// AppVersion.h
// -----------------------------------------------------------------------------
// Versión mostrada en la UI y comparada contra docs/version.json al buscar
// actualizaciones. Súbela a mano en cada release y crea el tag/Release en
// GitHub con el mismo número.
// =============================================================================
#pragma once

namespace audiopt {

constexpr wchar_t kAppVersion[] = L"1.0.0";

// Página de descarga / changelog público.
constexpr wchar_t kDownloadPageUrl[] =
    L"https://TU_USUARIO.github.io/TU_REPO/";

// Manifiesto de versión consultado por "Buscar actualizaciones".
constexpr wchar_t kVersionManifestUrl[] =
    L"https://TU_USUARIO.github.io/TU_REPO/version.json";

} // namespace audiopt
