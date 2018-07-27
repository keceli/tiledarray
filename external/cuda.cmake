find_package(CUDA)


if(CUDA_FOUND)
  set (TILEDARRAY_HAS_CUDA 1)
else(CUDA_FOUND)
  set (TILEDARRAY_HAS_CUDA 0)
endif(CUDA_FOUND)


if(CUDA_FOUND)
# TODO test CUDA
set(CUDA_LIBRARIES PUBLIC ${CUDA_LIBRARIES})
set(CUDA_CUBLAS_LIBRARIES PUBLIC ${CUDA_CUBLAS_LIBRARIES})
include_directories(${CUDA_INCLUDE_DIRS})
endif(CUDA_FOUND)