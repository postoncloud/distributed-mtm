#include <stdio.h>
#include <stdlib.h>

#include "kernels/dropout.h"

#define CUDA_CALL(x)                                  \
  do {                                                \
    if((x) != cudaSuccess) {                          \
      printf("Error at %s:%d\n", __FILE__, __LINE__); \
      exit(1);                                        \
    }                                                 \
  } while(0)

#define CURAND_CALL(x)                                \
  do {                                                \
    if((x) != CURAND_STATUS_SUCCESS) {                \
      printf("Error at %s:%d\n", __FILE__, __LINE__); \
      exit(1);                                        \
    }                                                 \
  } while(0)

namespace marian {

__global__ void gScale(float* data, int n, float p) {
  int index = threadIdx.x + blockIdx.x * blockDim.x;

  while(index < n) {
    data[index] = (data[index] < p) / p;
    index += gridDim.x * blockDim.x;
  }
}

void Dropout(Tensor tensor, float p, curandGenerator_t gen) {
  int n = tensor->size();
  CURAND_CALL(curandGenerateUniform(gen, tensor->data(), n));

  int numThreads = std::min(n, 512);
  int numBlocks = n / numThreads + (n % numThreads != 0);

  gScale<<<numBlocks, numThreads>>>(tensor->data(), n, 1.f - p);
}
}
