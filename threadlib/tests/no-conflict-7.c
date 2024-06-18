#include <stdio.h>

int main(void) {
    volatile int a[100];
    volatile int b[10000];
    volatile int c[10000];

    for(int i = 0; i < 100; i++) {
        a[i] = i;
        if(i % 2 == 0){
            for(int j = 0; j < 100; j++) {
                int index = i * 100 + j;
                b[index] = index;
            }
        } else {
            for(int j = 0; j < 100; j++) {
                int index = i * 100 + j;
                c[index] = index;
            }
        }
    } 
    
  for (int i = 0; i < 1000; i++) { 
    for (int j = 0; j < 1000; j++) { 
      int sum = 0; 
        for (int k = 0; k < 1000; k++) { 
          sum += (i + k) * (k + j); 
        } 
      b[j] = sum; 
    }
  }
    printf("a: %d\nb: %d\nc: %d\n", a[43], b[483], c[353]);
    return 0;
}
