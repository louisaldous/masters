#include <stdio.h>

int main(void) {
    volatile int a = 0;
    volatile int b = 0;
    
    for(int i = 0; i < 600; i++) {
        a += i;
        for(int i = 0; i < 600; i++) {
            b += i;
        }
    }

    
    
    printf("Final a: %d\nFinal b: %d\n", a, b);
    return 0;
}

