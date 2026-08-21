#include "gui.h"

#include <locale.h>
#include <stdlib.h>

int main() {
    setlocale(LC_ALL, "");
    setenv("LANG", "en_US.UTF-8", 1);
    setenv("LC_ALL", "en_US.UTF-8", 1);
    return run_gui();
}
