#include "SmatchetImGuiRenderBackend.h"

TUniquePtr<ISmatchetImGuiRenderBackend> CreateSmatchetImGuiRenderBackend_WinDx12();
TUniquePtr<ISmatchetImGuiRenderBackend> CreateSmatchetImGuiRenderBackend_Platform();

TUniquePtr<ISmatchetImGuiRenderBackend> CreateSmatchetImGuiRenderBackend() {
#if PLATFORM_WINDOWS
    return CreateSmatchetImGuiRenderBackend_WinDx12();
#elif (defined(PLATFORM_PS5) && PLATFORM_PS5) || (defined(PLATFORM_XBOXONE) && PLATFORM_XBOXONE) || (defined(PLATFORM_XSX) && PLATFORM_XSX)
    return CreateSmatchetImGuiRenderBackend_Platform();
#else
    return nullptr;
#endif
}
