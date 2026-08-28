// ------------------------------------------------------------------
// example_dll.c
// ------------------------------------------------------------------
// ExampleDll: 供 DSH Hub 客户端 DllJsonCaller 通过命名管道调用的示例 DLL。
// 导出 add / multiply / echo 三个函数，与 AttachedPlugin 中的
// dll_add / dll_multiply / dll_echo 工具一一对应。
// ------------------------------------------------------------------

#include <string.h>

// double add(double a, double b)
__declspec(dllexport) double add(double a, double b)
{
    return a + b;
}

// int multiply(int a, int b)
__declspec(dllexport) int multiply(int a, int b)
{
    return a * b;
}

// const char* echo(const char* text)
static char echoBuffer[4096];

__declspec(dllexport) const char *echo(const char *text)
{
    if (!text) {
        echoBuffer[0] = '\0';
        return echoBuffer;
    }
    strncpy(echoBuffer, text, sizeof(echoBuffer) - 1);
    echoBuffer[sizeof(echoBuffer) - 1] = '\0';
    return echoBuffer;
}
