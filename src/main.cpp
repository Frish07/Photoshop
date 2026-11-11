#include "app.hpp"

#include <shellapi.h>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 1;
    int exitCode = 0;
    {
        pf::App app(instance);
        if (!app.create(show)) {
            MessageBoxW(nullptr, L"Unable to create the editor window.", L"PixelForge", MB_ICONERROR | MB_OK);
            CoUninitialize();
            return 1;
        }
        int argc{};
        auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        try {
            if (argc <= 1) app.openDemo();
            for (int i = 1; i < argc; ++i) app.openPath(argv[i]);
        } catch (const std::exception& e) {
            MessageBoxA(nullptr, e.what(), "PixelForge", MB_ICONWARNING | MB_OK);
        }
        LocalFree(argv);
        MSG msg{};
        BOOL status{};
        while ((status = GetMessageW(&msg, nullptr, 0, 0)) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        exitCode = status == 0 ? int(msg.wParam) : 1;
    }
    CoUninitialize();
    return exitCode;
}
