#include "../src/theme.h"

#include <cstdio>

int main() {
    std::printf("SystemPrefersDark : %d\n", theme::SystemPrefersDark() ? 1 : 0);

    theme::Init(true);
    std::printf("after Init(true)  : IsDark=%d WindowColor=%06lX FieldColor=%06lX\n",
                theme::IsDark() ? 1 : 0,
                (unsigned long)theme::WindowColor(),
                (unsigned long)theme::FieldColor());

    theme::Init(false);
    std::printf("after Init(false) : IsDark=%d WindowColor=%06lX\n",
                theme::IsDark() ? 1 : 0,
                (unsigned long)theme::WindowColor());
    return 0;
}