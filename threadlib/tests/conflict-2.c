#include <stdio.h>

int main(void) {
    volatile int a = 0;
    for(int i = 0; i < 126; i++) {
        a += i;
    }
    printf("%d\n", a);
    return 0;
}

