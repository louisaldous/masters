#include<stdlib.h>
#include<stdio.h>

#define SIZE 100

long **mallocMatrix(long size){
  long **mat = (long**) malloc(size * sizeof(long*));

  for(int i = 0; i < size; i++){
    mat[i] = (long*) malloc(size * sizeof(long*));
  }

  return mat;
}

long **generateLargeMatrix(int size){
  long **mat = mallocMatrix(size); 

  printf("Entered\n");
  for(int i = 0; i < size; i++){
    for(int j = 0; j < size; j++){
      mat[i][j] = i * j;
    }
  }
  printf("Exited\n");

  return mat;
}

void freeMatrix(long **mat, int size){
  for(int i = 0; i < size; i++){
    free(mat[i]);
  }
  free(mat);
}

int main(){
  
  srand(42);

  long **A = generateLargeMatrix(SIZE);
  long **B = generateLargeMatrix(SIZE);

  long **C = mallocMatrix(SIZE); 

  // Multiply the two matrices 
  for (int i = 0; i < SIZE; i++) { 
    for (int j = 0; j < SIZE; j++) { 
      long sum = 0; 
      for (int k = 0; k < SIZE; k++) { 
        sum += A[i][k] * B[k][j]; 
      } 
      
      printf("i: %d\tj: %d\t sum: %ld\n", i, j, sum);

      C[i][j] = sum; 
    }
  }
  printf("done\n");
  
  int randomI = rand() % SIZE;
  int randomJ = rand() % SIZE;
  printf("Index: [%d][%d] Value: %ld\n", randomI, randomJ, C[randomI][randomJ]); 
  
  freeMatrix(C, SIZE);
  freeMatrix(B, SIZE);
  freeMatrix(A, SIZE);
  
  return 0;
}

