#include "stdio.h"

int main(){
  volatile float array2[1000];  
  volatile int array[1000];
  for(int i = 0; i < 1000; i++){
    array2[i] = ((float)i) * 1.5f;
    array[i] = i * 2;
  }
  
  printf("%d\n", array[437]);
  printf("%.2f\n", array2[748]);
  printf("Done!\n");
  return 0;  
}
