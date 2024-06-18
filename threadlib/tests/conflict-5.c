#include <stdio.h>

int main(void) {
    volatile int a[600];
    volatile int b = 0;
    
    for(int i = 0; i < 600; i++) {
        a[i] = i;
        for(int j = 0; j < 600; j++) {
            b += j;
        }
    }
 
    printf("A: %d Final b: %d\n", a[381], b);
    return 0;
}

