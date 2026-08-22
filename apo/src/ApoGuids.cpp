// =============================================================================
// ApoGuids.cpp
// -----------------------------------------------------------------------------
// Única unidad de compilación que DEFINE los GUIDs (el resto solo los declara).
// <initguid.h> activa INITGUID, que hace que DEFINE_GUID genere el símbolo real
// en vez de un extern. Incluirlo en más de un .cpp daría símbolos duplicados.
// =============================================================================
#include <windows.h>
#include <initguid.h>

#include "ApoGuids.h"
