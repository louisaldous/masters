#include <stdio.h>

int main(void) {
    volatile int b = 0;
    
    for(int i = 0; i < 600; i++) {
        for(int i = 0; i < 600; i++) {
            b += i;
        }
    }
 
    printf("Final b: %d\n", b);
    return 0;
}

