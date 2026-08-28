// test_echo.c — 直接链接 ExampleDll 验证三个函数
#include <stdio.h>

__declspec(dllimport) double add(double a, double b);
__declspec(dllimport) int multiply(int a, int b);
__declspec(dllimport) const char *echo(const char *text);

int main(void)
{
    printf("add(1, 1)       = %.1f\n", add(1.0, 1.0));
    printf("add(2.5, 3.5)   = %.1f\n", add(2.5, 3.5));
    printf("multiply(3, 4)  = %d\n", multiply(3, 4));
    printf("echo(hello DSH) = %s\n", echo("hello DSH"));
    printf("echo(empty)     = [%s]\n", echo(""));
    return 0;
}
