// Copyright (c) 2015-16 Tom Deakin, Simon McIntosh-Smith,
// University of Bristol HPC
//
// For full license terms please see the LICENSE file distributed with this
// source code


#include "HIPStream.h"
#include "hip/hip_runtime.h"

#define TBSIZE 1024


void check_error(void)
{
  hipError_t err = hipGetLastError();
  if (err != hipSuccess)
  {
    std::cerr << "Error: " << hipGetErrorString(err) << std::endl;
    exit(err);
  }
}

// It is best practice to include __device__ and constexpr even though in BabelStream it only needs to be __host__ const
__host__ __device__ constexpr size_t ceil_div(size_t a, size_t b) { return (a + b - 1)/b; }

template <class T>
HIPStream<T>::HIPStream(const intptr_t ARRAY_SIZE, const int device_index)
{
  // Set device
  int count;
  hipGetDeviceCount(&count);
  check_error();
  if (device_index >= count)
    throw std::runtime_error("Invalid device index");
  hipSetDevice(device_index);
  check_error();

  // Print out device information
  std::cout << "Using HIP device " << getDeviceName(device_index) << std::endl;
  std::cout << "Driver: " << getDeviceDriver(device_index) << std::endl;
#if defined(MANAGED)
    std::cout << "Memory: MANAGED" << std::endl;
#elif defined(PAGEFAULT)
    std::cout << "Memory: PAGEFAULT" << std::endl;
#else
    std::cout << "Memory: DEFAULT" << std::endl;
#endif

  hipDeviceProp_t props;
  hipGetDeviceProperties(&props, device_index);
  check_error();

  array_size = ARRAY_SIZE;
  dot_num_blocks = props.multiProcessorCount * 4;

  size_t array_bytes = sizeof(T);
  array_bytes *= ARRAY_SIZE;

  // Allocate the host array for partial sums for dot kernels using hipHostMalloc.
  // This creates an array on the host which is visible to the device. However, it requires
  // synchronization (e.g. hipDeviceSynchronize) for the result to be available on the host
  // after it has been passed through to a kernel.
  hipHostMalloc(&sums, sizeof(T) * dot_num_blocks, hipHostMallocNonCoherent);
  check_error();

  // Check buffers fit on the device
  if (props.totalGlobalMem < std::size_t{3}*array_bytes)
    throw std::runtime_error("Device does not have enough memory for all 3 buffers");

  // Create device buffers
#if defined(MANAGED)
  hipMallocManaged(&d_a, array_bytes);
  check_error();
  hipMallocManaged(&d_b, array_bytes);
  check_error();
  hipMallocManaged(&d_c, array_bytes);
  check_error();
#elif defined(PAGEFAULT)
  d_a = (T*)malloc(array_bytes);
  d_b = (T*)malloc(array_bytes);
  d_c = (T*)malloc(array_bytes);
#else
  hipMalloc(&d_a, array_bytes);
  check_error();
  hipMalloc(&d_b, array_bytes);
  check_error();
  hipMalloc(&d_c, array_bytes);
  check_error();
#endif
}


template <class T>
HIPStream<T>::~HIPStream()
{
  hipHostFree(sums);
  check_error();

  hipFree(d_a);
  check_error();
  hipFree(d_b);
  check_error();
  hipFree(d_c);
  check_error();
}


template <typename T>
__global__ void init_kernel(T * a, T * b, T * c, T initA, T initB, T initC, size_t array_size)
{
  for (size_t i = (size_t)threadIdx.x + (size_t)blockDim.x * blockIdx.x; i < array_size; i += (size_t)gridDim.x * blockDim.x) {
    a[i] = initA;
    b[i] = initB;
    c[i] = initC;
  }
}

template <class T>
void HIPStream<T>::init_arrays(T initA, T initB, T initC)
{
  size_t blocks = ceil_div(array_size, TBSIZE);
  init_kernel<T><<<dim3(blocks), dim3(TBSIZE), 0, 0>>>(d_a, d_b, d_c, initA, initB, initC, array_size);
  check_error();
  hipDeviceSynchronize();
  check_error();
}

template <class T>
void HIPStream<T>::read_arrays(std::vector<T>& a, std::vector<T>& b, std::vector<T>& c)
{

  // Copy device memory to host
#if defined(PAGEFAULT) || defined(MANAGED)
    hipDeviceSynchronize();
  for (intptr_t i = 0; i < array_size; i++)
  {
    a[i] = d_a[i];
    b[i] = d_b[i];
    c[i] = d_c[i];
  }
#else
  hipMemcpy(a.data(), d_a, a.size()*sizeof(T), hipMemcpyDeviceToHost);
  check_error();
  hipMemcpy(b.data(), d_b, b.size()*sizeof(T), hipMemcpyDeviceToHost);
  check_error();
  hipMemcpy(c.data(), d_c, c.size()*sizeof(T), hipMemcpyDeviceToHost);
  check_error();
#endif
}

template <typename T>
__global__ void copy_kernel(const T * a, T * c, size_t array_size)
{
  for (size_t i = (size_t)threadIdx.x + (size_t)blockDim.x * blockIdx.x; i < array_size; i += (size_t)gridDim.x * blockDim.x) {
    c[i] = a[i];
  }
}

template <class T>
void HIPStream<T>::copy()
{
  size_t blocks = ceil_div(array_size, TBSIZE);
  copy_kernel<T><<<dim3(blocks), dim3(TBSIZE), 0, 0>>>(d_a, d_c, array_size);
  check_error();
  hipDeviceSynchronize();
  check_error();
}

template <typename T>
__global__ void mul_kernel(T * b, const T * c, size_t array_size)
{
  const T scalar = startScalar;
  for (size_t i = (size_t)threadIdx.x + (size_t)blockDim.x * blockIdx.x; i < array_size; i += (size_t)gridDim.x * blockDim.x) {
    b[i] = scalar * c[i];
  }
}

template <class T>
void HIPStream<T>::mul()
{
  size_t blocks = ceil_div(array_size, TBSIZE);
  mul_kernel<T><<<dim3(blocks), dim3(TBSIZE), 0, 0>>>(d_b, d_c, array_size);
  check_error();
  hipDeviceSynchronize();
  check_error();
}

template <typename T>
__global__ void add_kernel(const T * a, const T * b, T * c, size_t array_size)
{
  for (size_t i = (size_t)threadIdx.x + (size_t)blockDim.x * blockIdx.x; i < array_size; i += (size_t)gridDim.x * blockDim.x) {
    c[i] = a[i] + b[i];
  }
}

template <class T>
void HIPStream<T>::add()
{
  size_t blocks = ceil_div(array_size, TBSIZE);
  add_kernel<T><<<dim3(blocks), dim3(TBSIZE), 0, 0>>>(d_a, d_b, d_c, array_size);
  check_error();
  hipDeviceSynchronize();
  check_error();
}

template <typename T>
__global__ void triad_kernel(T * a, const T * b, const T * c, size_t array_size)
{
  const T scalar = startScalar;
  for (size_t i = (size_t)threadIdx.x + (size_t)blockDim.x * blockIdx.x; i < array_size; i += (size_t)gridDim.x * blockDim.x) {
    a[i] = b[i] + scalar * c[i];
  }
}

template <class T>
void HIPStream<T>::triad()
{
  size_t blocks = ceil_div(array_size, TBSIZE);
  triad_kernel<T><<<dim3(blocks), dim3(TBSIZE), 0, 0>>>(d_a, d_b, d_c, array_size);
  check_error();
  hipDeviceSynchronize();
  check_error();
}

template <typename T>
__global__ void nstream_kernel(T * a, const T * b, const T * c, size_t array_size)
{
  const T scalar = startScalar;
  for (size_t i = (size_t)threadIdx.x + (size_t)blockDim.x * blockIdx.x; i < array_size; i += (size_t)gridDim.x * blockDim.x) {
    a[i] += b[i] + scalar * c[i];
  }
}

template <class T>
void HIPStream<T>::nstream()
{
  size_t blocks = ceil_div(array_size, TBSIZE);
  nstream_kernel<T><<<dim3(blocks), dim3(TBSIZE), 0, 0>>>(d_a, d_b, d_c, array_size);
  check_error();
  hipDeviceSynchronize();
  check_error();
}

template <typename T>
__global__ void dot_kernel(const T * a, const T * b, T * sum, size_t array_size)
{
  __shared__ T tb_sum[TBSIZE];

  const size_t local_i = threadIdx.x;
  size_t i = blockDim.x * blockIdx.x + local_i;

  tb_sum[local_i] = {};
  for (; i < array_size; i += blockDim.x*gridDim.x)
    tb_sum[local_i] += a[i] * b[i];

  for (size_t offset = blockDim.x / 2; offset > 0; offset /= 2)
  {
    __syncthreads();
    if (local_i < offset)
    {
      tb_sum[local_i] += tb_sum[local_i+offset];
    }
  }

  if (local_i == 0)
    sum[blockIdx.x] = tb_sum[local_i];
}

template <class T>
T HIPStream<T>::dot()
{
  dot_kernel<T><<<dim3(dot_num_blocks), dim3(TBSIZE), 0, 0>>>(d_a, d_b, sums, array_size);
  check_error();
  hipDeviceSynchronize();
  check_error();

  T sum{};
  for (intptr_t i = 0; i < dot_num_blocks; i++)
    sum += sums[i];

  return sum;
}

void listDevices(void)
{
  // Get number of devices
  int count;
  hipGetDeviceCount(&count);
  check_error();

  // Print device names
  if (count == 0)
  {
    std::cerr << "No devices found." << std::endl;
  }
  else
  {
    std::cout << std::endl;
    std::cout << "Devices:" << std::endl;
    for (int i = 0; i < count; i++)
    {
      std::cout << i << ": " << getDeviceName(i) << std::endl;
    }
    std::cout << std::endl;
  }
}


std::string getDeviceName(const int device)
{
  hipDeviceProp_t props;
  hipGetDeviceProperties(&props, device);
  check_error();
  return std::string(props.name);
}


std::string getDeviceDriver(const int device)
{
  hipSetDevice(device);
  check_error();
  int driver;
  hipDriverGetVersion(&driver);
  check_error();
  return std::to_string(driver);
}

template class HIPStream<float>;
template class HIPStream<double>;
