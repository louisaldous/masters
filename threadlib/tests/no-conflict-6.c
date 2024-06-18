#include <stdio.h>

int main(void) {
    volatile int a[100];
    volatile int b[10000];
    volatile int d[100];

    for(int i = 0; i < 10000; i++){
       b[i] = i; 
    }
    
    for(int i = 0; i < 100; i++) {
        a[i] = i;
        int c = i * 2;
        for(int j = 0; j < 100; j++) {
            int index = c * 50 + j;

            b[index] = j;
        }
        d[i] = c * 2;
    } 
    
    printf("a: %d\nb: %d\nd: %d\n", a[43], b[383], d[79]);
    return 0;
}
