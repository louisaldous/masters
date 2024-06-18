#include <stdio.h>

int main(void) {
    volatile int a[100];
    volatile int b[10000];
    
    for(int i = 0; i < 100; i++) {
        a[i] = i;
        for(int j = 0; j < 100; j++) {
            int index = i * 100 + j;
            if(index % 2 == 0) {
                index /= 2;
            }
            b[index] = j;
        }
    }
 
    printf("A: %d Final b: %d\n", a[81], b[529]);
    return 0;
}

