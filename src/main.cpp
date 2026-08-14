#include "Core/Application.h"

#include <Windows.h>

#include <exception>
#include <fstream>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
    try
    {
        Shadow::Core::Application application(instance);
        return application.Run();
    }
    catch (const std::exception& exception)
    {
        // GUI 子系统没有控制台；同时写入工作目录，VS Code 直接运行失败时仍能看到原因。
        std::ofstream("ShadowEngine-error.log", std::ios::trunc) << exception.what() << '\n';
        MessageBoxA(nullptr, exception.what(), "Shadow Engine - Fatal Error", MB_OK | MB_ICONERROR);
        return EXIT_FAILURE;
    }
}
