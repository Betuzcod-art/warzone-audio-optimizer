// =============================================================================
// ApoGuids.h
// -----------------------------------------------------------------------------
// Identificadores COM del APO. Son públicos y permanentes: quedan escritos en
// el registro de Windows al instalar, así que NO deben cambiar entre versiones
// salvo que se cambie también el instalador -- si cambian, Windows seguiría
// apuntando al CLSID viejo y el APO no cargaría.
//
// (Microsoft recomienda generar un CLSID nuevo cuando cambia el contrato de la
// interfaz, no en cada release.)
// =============================================================================
#pragma once

#include <guiddef.h>

// {E76EE61C-E30D-4C52-994D-F54422E9A2C9} -- efecto de modo (MFX)
DEFINE_GUID(CLSID_WarzoneApoMfx,
    0xe76ee61c, 0xe30d, 0x4c52, 0x99, 0x4d, 0xf5, 0x44, 0x22, 0xe9, 0xa2, 0xc9);

#define WARZONE_APO_MFX_CLSID_STRING L"{E76EE61C-E30D-4C52-994D-F54422E9A2C9}"
