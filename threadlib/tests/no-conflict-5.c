#include <stdio.h>

int main(void) {
    volatile int a[100];
    volatile int b[10000];
    
    for(int i = 0; i < 100; i++) {
        a[i] = i;
        for(int j = 0; j < 100; j++) {
            int index = i * 100 + j;
            b[index] = index;
        }
    } 
    
    printf("a: %d\nb: %d\n", a[43], b[383]);
    return 0;
}

