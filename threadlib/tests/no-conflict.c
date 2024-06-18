#include "stdio.h"

int main(){
  volatile int array[1000];
  
  for(int i = 0; i < 1000; i++){
    array[i] = i * 2;
  }
  
  printf("%d\n", array[437]);
  printf("Done!\n");
  return 0;  
}
