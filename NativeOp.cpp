
#include <assert.h>
#include <iostream>
#include <fstream>
#include <limits>
#include <sstream>
#include <string.h>
#include <vector>

#define ARRAY_LEN(x) (sizeof(x) / sizeof(x[0]))


#define assert_cmp(a, cmp, b) \
    if(!((a) cmp (b))) { \
        std::cerr << "Assertion failed: " << a << " " << #cmp << " " << b << std::endl; \
        assert((a) cmp (b)); \
    }



#ifndef TENSORFLOW
#define TENSORFLOW 0
#endif

/*
Reference: https://en.wikipedia.org/wiki/Row-_and_column-major_order
Memory layout:
* Row-major order, C contiguous
* Column-major, Fortran contiguous

Numpy (Ndarray) and Theano (and CudaNdarray) can support any memory layout (via custom strides),
    although row-major (C-contiguous) is the standard,
    and you get it via theano.extra_ops.CpuContiguous() or numpy.ascontiguousarray().
TensorFlow (Tensor) is always row-major, although it uses Eigen under the hood,
    which supports both row-major and column-major.
The BLAS functions expect the inputs in column-major and return in column-major.
*/

#if TENSORFLOW
// https://www.tensorflow.org/api_docs/cc/class/tensorflow/tensor
// https://github.com/tensorflow/tensorflow/blob/master/tensorflow/core/framework/tensor.h
// https://github.com/tensorflow/tensorflow/blob/master/tensorflow/core/framework/op_kernel.h
// https://eigen.tuxfamily.org/dox-devel/unsupported/Tensor_8h_source.html
#define Ndarray tensorflow::Tensor
#define Ndarray_DEV_DATA(x) ((float*) (x)->tensor_data().data())
#define Ndarray_DEV_DATA_int32(x) ((int32_t*) (x)->tensor_data().data())
#define Ndarray_DEV_DATA_int32_scalar(x) (x)->scalar<int32>()()
#define Ndarray_HOST_DIMS(x) DimsAccessor(x)
#define Ndarray_DIMS Ndarray_HOST_DIMS
#define Ndarray_NDIM(x) (x)->dims()
#define Ndarray_dtype_size(x) tensorflow::DataTypeSize((x)->dtype())
typedef long long Ndarray_DIM_Type;
#define Ndarray_SIZE(x) (x)->NumElements()

struct DimsAccessor {
    const Ndarray* tensor_;
    DimsAccessor(const Ndarray* tensor) : tensor_(tensor) {}
    Ndarray_DIM_Type operator[](const int i) {
        return tensor_->dim_size(i);
    }
};
typedef DimsAccessor Ndarray_DIMS_Type;

// return in elements
static inline size_t Ndarray_STRIDE(const Ndarray* x, int dim) {
    int ndim = x->dims();
    if(dim + 1 >= ndim)
        return 1;
    return x->dim_size(dim + 1) * Ndarray_STRIDE(x, dim + 1);
}

// uninitialized
static Ndarray* Ndarray_NewDims(int nd, Ndarray_DIMS_Type dims) {
    // TODO...
    assert("not implemented" && 0);
    return NULL;
}

Ndarray* Ndarray_Copy(const Ndarray* self) {
    // https://github.com/tensorflow/tensorflow/blob/master/tensorflow/core/kernels/dense_update_ops.cc
    // copy(context->eigen_device<Device>(), lhs->flat<T>(), rhs.flat<T>()) ....
    // TODO...
    assert("not implemented" && 0);
    return NULL;
}

// BLAS:
// https://github.com/tensorflow/tensorflow/blob/master/tensorflow/contrib/rnn/kernels/blas_gemm.cc
// https://github.com/tensorflow/tensorflow/blob/master/tensorflow/core/kernels/matmul_op.cc

// https://github.com/tensorflow/tensorflow/issues/6602
// TODO: Fixed now, check if it works, maybe we can remove this workaround.
#define TF_issue_6602_workaround 1

#if TF_issue_6602_workaround

#if GOOGLE_CUDA && !CUDA
// GOOGLE_CUDA && !CUDA: Make this only for the main namespace.
// Via: https://github.com/tensorflow/tensorflow/blob/master/tensorflow/contrib/rnn/kernels/blas_gemm.cc
namespace tensorflow {
namespace functor {
template <typename T>
struct TensorCuBlasGemm {
  void operator()(OpKernelContext* ctx, bool transa, bool transb, uint64 m,
                  uint64 n, uint64 k, T alpha, const T* a, int lda, const T* b,
                  int ldb, T beta, T* c, int ldc);
};
}
}
#endif  // GOOGLE_CUDA && !CUDA

#else  // TF_issue_6602_workaround

// http://stackoverflow.com/questions/41428756/own-tensorflow-op-with-cublassgemm
#if GOOGLE_CUDA
// or tensorflow/include/tensorflow/core/util/stream_executor_util.h ?
template <typename T>
perftools::gputools::DeviceMemory<T> AsDeviceMemory(const T* cuda_memory) {
  perftools::gputools::DeviceMemoryBase wrapped(const_cast<T*>(cuda_memory));
  perftools::gputools::DeviceMemory<T> typed(wrapped);
  return typed;
}

static perftools::gputools::blas::Transpose int get_transpose(char t) {
    switch(t) {
    case 'T':
        return perftools::gputools::blas::Transpose::kTranspose;
    case 'C':
        return perftools::gputools::blas::Transpose::kConjugateTranspose;
    case 'N':
        return perftools::gputools::blas::Transpose::kNoTranspose;
    default:
        assert("invalid transpose option" || 0);
    }
}
#endif  // GOOGLE_CUDA
#endif  // TF_issue_6602_workaround

template<typename T>
static void tf_cuda_sgemm(
        OpKernelContext* context,
        char transa, char transb,
        int m, int n, int k,
        const T* alpha_, const T* a, int lda,
        const T* b, int ldb, const T* beta_,
        T* c,
        int ldc) {
    T alpha = *alpha_;
    T beta = *beta_;
// https://github.com/tensorflow/tensorflow/blob/master/tensorflow/contrib/rnn/kernels/blas_gemm.cc
#if GOOGLE_CUDA
#if TF_issue_6602_workaround
    functor::TensorCuBlasGemm<T>() (
        context,
        transa != 'N', transb != 'N',
        m, n, k,
        alpha, a, lda, b, ldb, beta, c, ldc
    );

#else  // TF_issue_6602_workaround
    auto a_ptr = AsDeviceMemory(a);
    auto b_ptr = AsDeviceMemory(b);
    auto c_ptr = AsDeviceMemory(c);

    cudaStream_t cuda_stream = context->eigen_gpu_device().stream();

    // cublasCreate, http://docs.nvidia.com/cuda/cublas/#cublascreate

    auto dev_ctx = context->op_device_context();
    auto* dev_stream = dev_ctx->stream();
    OP_REQUIRES(context, dev_stream, errors::Internal("No GPU stream available."));

    bool blas_launch_status =
        dev_stream
             ->ThenBlasGemm(get_transpose(transa), get_transpose(transb),
                            m, n, k, alpha, a_ptr,
                            lda, b_ptr, ldb, beta, &c_ptr, ldc)
             .ok();
    OP_REQUIRES(context, blas_launch_status, errors::Aborted("CuBlasGemm failed!"));
#endif  // TF_issue_6602_workaround
#else  // GOOGLE_CUDA
    context->SetStatus(errors::InvalidArgument("CuBlasGemm needs CUDA."));
#endif  // GOOGLE_CUDA
}

#if CUDA
#if !GOOGLE_CUDA
#error "GOOGLE_CUDA not defined"
#endif


#define Ndarray_sgemm( \
	transpose_A, transpose_B, \
	m, n, k, alpha, A, lda, B, ldb, beta, C, ldc) \
    tf_cuda_sgemm<float>(context, transpose_A, transpose_B, m, n, k, alpha, A, lda, B, ldb, beta, C, ldc);

#else  // CUDA
/*
    // matrices are in column-major form
	int sgemm_(char *transa, char *transb,
		integer *m, integer *n, integer *k,
		real *alpha, real *a, integer *lda,
		real *b, integer *ldb, real *beta,
		real *c, integer *ldc);
*/
#define Ndarray_sgemm(\
	transpose_A, transpose_B, \
	m, n, k, alpha, A, lda, B, ldb, beta, C, ldc) \
	{ \
		char transa = transpose_A, transb = transpose_B; \
		int m_ = m, n_ = n, k_ = k, lda_ = lda, ldb_ = ldb, ldc_ = ldc; \
		sgemm_(&transa, &transb, \
			&m_, &n_, &k_, alpha, A, &lda_, B, &ldb_, beta, C, &ldc_); \
	}
#endif  // CUDA

// See Context struct below.
#define CONTEXT_ARGS    context

#else  // TENSORFLOW

// See Context struct below.
#define CONTEXT_ARGS

#endif  // TENSORFLOW



#if CUDA

#define elem_atomic_add(x, v) atomicAdd(x, v)

#if TENSORFLOW
// Ndarray and friends already declared above, they are same for CUDA and non-CUDA
#define CUDA_CUR_STREAM  (context->eigen_gpu_device().stream())

#else  // TENSORFLOW, thus Theano here
#define CUDA_CUR_STREAM  (0)  // default stream

// Defined here: https://github.com/Theano/Theano/blob/master/theano/sandbox/cuda/cuda_ndarray.cuh
// See also: https://github.com/Theano/Theano/blob/master/theano/sandbox/cuda/cuda_ndarray.cu
#define Ndarray CudaNdarray
#define Ndarray_DEV_DATA CudaNdarray_DEV_DATA
#define Ndarray_DEV_DATA_int32(x) ((int32_t*) (Ndarray_DEV_DATA(x)))
#define Ndarray_DEV_DATA_int32_scalar(x) Ndarray_DEV_DATA_int32(x)[0]
#define Ndarray_HOST_DIMS CudaNdarray_HOST_DIMS
#define Ndarray_DIMS Ndarray_HOST_DIMS
#define Ndarray_STRIDE(x, i) (CudaNdarray_HOST_STRIDES(x)[i])  // return in elements. CudaNdarray stores like that
#define Ndarray_NDIM(x) (x->nd)
#define Ndarray_DIM_Type int
typedef Ndarray_DIM_Type const* Ndarray_DIMS_Type;
#define Ndarray_dtype_size(x) sizeof(float)
#define Ndarray_SIZE CudaNdarray_SIZE
// PyObject *CudaNdarray_NewDims(int nd, const inttype * dims), uninitialized
#define Ndarray_NewDims CudaNdarray_NewDims
// PyObject * CudaNdarray_Copy(const CudaNdarray * self);
#define Ndarray_Copy CudaNdarray_Copy

/*
    // via: http://docs.nvidia.com/cuda/cublas/
    // matrices are in column-major form
    cublasStatus_t cublasSgemm(cublasHandle_t handle,
        cublasOperation_t transa, cublasOperation_t transb,
        int m, int n, int k,
        const float *alpha, const float *A, int lda,
        const float *B, int ldb, const float *beta,
        float *C, int ldc);
*/
#define _cublasTranspose(t) \
	((t == 'T') ? CUBLAS_OP_T : \
	(t == 'C') ? CUBLAS_OP_C : \
	(t == 'N') ? CUBLAS_OP_N : cublasOperation_t('E'))
#define Ndarray_sgemm( \
	transpose_A, transpose_B, \
	m, n, k, alpha, A, lda, B, ldb, beta, C, ldc) \
	(_cudaHandleError(cublasSgemm(handle, \
	_cublasTranspose(transpose_A), \
	_cublasTranspose(transpose_B), \
	m, n, k, alpha, A, lda, B, ldb, beta, C, ldc), \
	__FILE__, __LINE__ ))

#endif

#define Ndarray_memcpy(y, x, size) (cudaMemcpyAsync(y, x, size, cudaMemcpyDeviceToDevice, CUDA_CUR_STREAM))
#define Ndarray_memset(s, c, size) (cudaMemsetAsync(s, c, size, CUDA_CUR_STREAM))

#define DIM_GRID 128
#define DIM_BLOCK 512

#define DEF_KERNEL __global__
// <<<DimGrid,DimBlock,ShmemSize|0,Stream|0>>>. http://docs.nvidia.com/cuda/cuda-c-programming-guide/#execution-configuration
#define start_dev_kernel(kernel, args) \
	(kernel<<<DIM_GRID,DIM_BLOCK,0,CUDA_CUR_STREAM>>>  args);

static const char *_cudaGetErrorEnum(cublasStatus_t error) {
	switch (error) {
	case CUBLAS_STATUS_SUCCESS:
		return "CUBLAS_STATUS_SUCCESS";

	case CUBLAS_STATUS_NOT_INITIALIZED:
		return "CUBLAS_STATUS_NOT_INITIALIZED";

	case CUBLAS_STATUS_ALLOC_FAILED:
		return "CUBLAS_STATUS_ALLOC_FAILED";

	case CUBLAS_STATUS_INVALID_VALUE:
		return "CUBLAS_STATUS_INVALID_VALUE";

	case CUBLAS_STATUS_ARCH_MISMATCH:
		return "CUBLAS_STATUS_ARCH_MISMATCH";

	case CUBLAS_STATUS_MAPPING_ERROR:
		return "CUBLAS_STATUS_MAPPING_ERROR";

	case CUBLAS_STATUS_EXECUTION_FAILED:
		return "CUBLAS_STATUS_EXECUTION_FAILED";

	case CUBLAS_STATUS_INTERNAL_ERROR:
		return "CUBLAS_STATUS_INTERNAL_ERROR";
	}

	return "<unknown>";
}

static void _cudaHandleError(cudaError_t err, const char *file, int line) {
	if (err != cudaSuccess) {
		printf("NativeOp: CUDA runtime error: '%s' in %s at line %d\n", cudaGetErrorString(err), file, line);
		exit(EXIT_FAILURE);
	}
}

static void _cudaHandleError(cublasStatus_t status, const char *file, int line) {
	if (status != CUBLAS_STATUS_SUCCESS) {
		printf("NativeOp: cuBLAS runtime error: '%s' in %s at line %d\n", _cudaGetErrorEnum(status), file, line);
		exit(EXIT_FAILURE);
	}
}

#define HANDLE_ERROR(status) (_cudaHandleError( status, __FILE__, __LINE__ ))
#define HANDLE_LAST_ERROR()  (HANDLE_ERROR(cudaGetLastError()))

#else   // not CUDA

#define elem_atomic_add(x, v) (*x += v)  // ignore atomic for now...

#if !TENSORFLOW
// Numpy, see: http://docs.scipy.org/doc/numpy/reference/c-api.array.html
// And: http://deeplearning.net/software/theano/extending/extending_theano_c.html
#define Ndarray PyArrayObject
#define Ndarray_DEV_DATA(x) ((float*) PyArray_DATA(x))
#define Ndarray_DEV_DATA_int32(x) ((int32_t*) (Ndarray_DEV_DATA(x)))
#define Ndarray_DEV_DATA_int32_scalar(x) Ndarray_DEV_DATA_int32(x)[0]
#define Ndarray_HOST_DIMS PyArray_DIMS
#define Ndarray_STRIDE(x, i) (PyArray_STRIDE(x, i) / sizeof(float))  // return in elements. Numpy stores in bytes
#define Ndarray_DIMS Ndarray_HOST_DIMS
#define Ndarray_NDIM PyArray_NDIM
#define Ndarray_DIM_Type npy_intp
typedef Ndarray_DIM_Type const* Ndarray_DIMS_Type;
#define Ndarray_dtype_size(x) sizeof(float)
#define Ndarray_SIZE PyArray_SIZE
#define Ndarray_NewDims(nd, dims) (PyArray_SimpleNew(nd, dims, NPY_FLOAT32))
#define Ndarray_Copy(x) (PyArray_FromArray(x, NULL, NPY_ARRAY_OUT_ARRAY | NPY_ARRAY_ENSURECOPY))
/*
    // matrices are in column-major form
	int sgemm_(char *transa, char *transb,
		integer *m, integer *n, integer *k,
		real *alpha, real *a, integer *lda,
		real *b, integer *ldb, real *beta,
		real *c, integer *ldc);

	Cast to (float*) because we might have the C-style declaration incorrectly in the C++ scope.
*/
#define Ndarray_sgemm(\
	transpose_A, transpose_B, \
	m, n, k, alpha, A, lda, B, ldb, beta, C, ldc) \
	{ \
		char transa = transpose_A, transb = transpose_B; \
		int m_ = m, n_ = n, k_ = k, lda_ = lda, ldb_ = ldb, ldc_ = ldc; \
		sgemm_(&transa, &transb, \
			&m_, &n_, &k_, alpha, (float*) A, &lda_, (float*) B, &ldb_, beta, C, &ldc_); \
	}
#endif

#define Ndarray_memcpy(y, x, size) (memcpy(y, x, size))
#define Ndarray_memset(s, c, size) (memset(s, c, size))

#define DEF_KERNEL
#define start_dev_kernel(kernel, args) \
	{ for(_KernelLoop loop; !loop.finished(); loop.next()) { kernel args; } }

struct _int3 {
    int x, y, z;
};

struct _uint3 {
    unsigned int x, y, z;
};

template<typename T>
static void resetVec3(T& v) {
    v.x = v.y = v.z = 0;
}

static _uint3 _threadIdx;
static _uint3 _blockIdx;
static _int3 _blockDim;
static _int3 _gridDim;
// We need those as macros to not infer with the CUDA versions if CUDA was also included.
#define threadIdx _threadIdx
#define blockIdx _blockIdx
#define blockDim _blockDim
#define gridDim _gridDim

struct _KernelLoop {
	_KernelLoop() {
		// When we can choose whatever we want here, this loops becomes trivial,
		// there will only be one iteration.
		resetVec3(gridDim); gridDim.x = 1;
		resetVec3(blockDim); blockDim.x = 1;
		resetVec3(threadIdx);
		resetVec3(blockIdx);
	}
	bool finished() {
		// TODO: Also block idx but doesn't matter with the constants above.
		// TODO: Also y/z but doesn't matter with the constants above.
		return threadIdx.x >= blockDim.x;
	}
	void next() {
		// TODO: Also blockIdx and y/z, but doesn't matter with the constants above.
		threadIdx.x++;
	}
};

#endif


Ndarray* Ndarray_uninitialized_like(Ndarray* a) {
	Ndarray_DIMS_Type dim = Ndarray_HOST_DIMS(a);
#if TENSORFLOW
	Ndarray* res = (Ndarray*) Ndarray_NewDims(Ndarray_NDIM(a), dim);
#else
	Ndarray* res = (Ndarray*) Ndarray_NewDims(Ndarray_NDIM(a), const_cast<Ndarray_DIM_Type*>(dim));
#endif
	return res;
}

long Ndarray_get_n_total_elements(Ndarray* a) {
	long c = 1;
	for(long i = 0; i < Ndarray_NDIM(a); ++i)
		c *= Ndarray_DIMS(a)[i];
	return c;
}

//if nd is 2 then assume a weight matrix and just return beginning of data
//else nd should be 3 and we pick the x part
float* data_ptr(Ndarray* a, int x) {
	assert(Ndarray_NDIM(a) == 2 || Ndarray_NDIM(a) == 3);
	if(Ndarray_NDIM(a) == 2)
		return Ndarray_DEV_DATA(a);
	else {
		Ndarray_DIMS_Type dims = Ndarray_HOST_DIMS(a);
		return Ndarray_DEV_DATA(a) + x * dims[1] * dims[2];
	}
}

const float* data_ptr(const Ndarray* a, int x) {
	return data_ptr((Ndarray*) a, x);
}

void lastTwoDims(const Ndarray* a, int out[2]) {
	Ndarray_DIMS_Type dims = Ndarray_HOST_DIMS((Ndarray*) a);
	assert(Ndarray_NDIM(a) >= 2);
	out[0] = dims[Ndarray_NDIM(a) - 2];
	out[1] = dims[Ndarray_NDIM(a) - 1];
}

int lastTwoDimsStride(const Ndarray * a) {
	int dims[2];
	lastTwoDims(a, dims);
	return dims[0] * dims[1];
}

struct Context  {
    /*
    E.g. TensorFlow requires that we know about the context in some subroutines.
    This helper class/struct is there to capture the context and make it accessible to any potential subroutines.
    */
#if TENSORFLOW
    OpKernelContext* context;
    Context(OpKernelContext* ctx_) : context(ctx_) {}
#else
    Context() {}
#endif

/*
Note: There is also this hacky way to get the cudaStream_t:
  const cudaStream_t cu_stream = CHECK_NOTNULL(
      reinterpret_cast<const cudaStream_t>(context->op_device_context()
                                                ->stream()
                                                ->implementation()
                                                ->CudaStreamMemberHack()));

*/


void _Ndarray_set_zero(Ndarray* a) {
	long size = Ndarray_get_n_total_elements(a) * Ndarray_dtype_size(a);
	Ndarray_memset(Ndarray_DEV_DATA(a), 0, size);
}
#define Ndarray_set_zero Context(CONTEXT_ARGS)._Ndarray_set_zero


#if TENSORFLOW
void* _malloc(size_t num_bytes) {
    //auto dev = context->eigen_device<EigenDev>();
    //auto* stream = context->op_device_context()->stream();
    Allocator* allocator =
        context->device()->GetAllocator(AllocatorAttributes());
    void* ptr = (void*)allocator->Allocate<uint8_t>(num_bytes);
    if(!ptr)
        context->CtxFailure(
            errors::InvalidArgument("NativeOp: cannot allocate ", num_bytes, " bytes on ", allocator->Name()));
    return ptr;
}
void _free(void* ptr) {
    Allocator* allocator =
        context->device()->GetAllocator(AllocatorAttributes());
    allocator->DeallocateRaw(ptr);
}
#define device_malloc Context(CONTEXT_ARGS)._malloc
#define device_free Context(CONTEXT_ARGS)._free

#if CUDA
cublasHandle_t _handle() {
    assert("not available" && 0);
    return NULL;
}
#define handle Context(CONTEXT_ARGS)._handle()
#endif
#endif


//C[x] += A[x]*B[x]
//(if not 4-dimensional, then indexing [x] is ignored (e.g. for weight matrices))

void _affine_y_x(
        int x_A, Ndarray* A, int x_B, Ndarray* B,
	    int x_C, /*out*/Ndarray* C, bool transpose_A = false, bool transpose_B = false, float beta = 1.0) {
	const float* data_A = data_ptr(A, x_A);
	const float* data_B = data_ptr(B, x_B);
	float* data_C = data_ptr(C, x_C);
	// expect row-major (C-contiguous), and dims represent (columns, rows)
	int A_dim[2], B_dim[2], C_dim[2];
	lastTwoDims(A, A_dim);
	lastTwoDims(B, B_dim);
	lastTwoDims(C, C_dim);

    int ldC = C_dim[1];
	int ldB = B_dim[1];
	int ldA = A_dim[1];
	char transA = transpose_A ? 'T' : 'N';
	char transB = transpose_B ? 'T' : 'N';
	if (transpose_A)
		std::swap(A_dim[0], A_dim[1]);
	if (transpose_B)
		std::swap(B_dim[0], B_dim[1]);
	// Note that A/B will be swapped around in the sgemm call below.
    assert_cmp(A_dim[0], ==, C_dim[0]);
    assert_cmp(B_dim[1], ==, C_dim[1]);
    assert_cmp(A_dim[1], ==, B_dim[0]);
    int m = B_dim[1];
    int n = A_dim[0];
    int k = A_dim[1];

	const float alpha = 1;

    // https://www.ibm.com/support/knowledgecenter/en/SSFHY8_5.5.0/com.ibm.cluster.essl.v5r5.essl100.doc/am5gr_hsgemm.htm
    // https://www.math.utah.edu/software/lapack/lapack-blas/sgemm.html
	Ndarray_sgemm(
	    transB, transA, m, n, k,
	    &alpha, data_B, ldB, data_A, ldA, &beta, data_C, ldC);
}
#define affine_y_x Context(CONTEXT_ARGS)._affine_y_x

//C += A*B
//(if not 4-dimensional, then indexing [x] is ignored (e.g. for weight matrices))

void _affine_raw(
        float* A, int a0, int a1,
        float* B, int b0, int b1,
        /*out*/float* C, int c0, int c1,
	    bool transpose_A = false, bool transpose_B = false,
	    float beta = 1.0, float alpha = 1.0,
	    int ldA_factor = 1, int ldB_factor = 1) {
	const float* data_A = A;
	const float* data_B = B;
	float* data_C = C;
	int A_dim[2], B_dim[2], C_dim[2];
    A_dim[0] = a0; A_dim[1] = a1;
    B_dim[0] = b0; B_dim[1] = b1;
    C_dim[0] = c0; C_dim[1] = c1;

    int ldC = C_dim[1];
	int ldB = B_dim[1] * ldB_factor;
	int ldA = A_dim[1] * ldA_factor;
	char transA = transpose_A ? 'T' : 'N';
	char transB = transpose_B ? 'T' : 'N';
	if (transpose_A)
		std::swap(A_dim[0], A_dim[1]);
	if (transpose_B)
		std::swap(B_dim[0], B_dim[1]);
	// Note that A/B will be swapped around in the sgemm call below.
    assert_cmp(A_dim[0], ==, C_dim[0]);
    assert_cmp(B_dim[1], ==, C_dim[1]);
    assert_cmp(A_dim[1], ==, B_dim[0]);
    int m = B_dim[1];
    int n = A_dim[0];
    int k = A_dim[1];

	Ndarray_sgemm(
	    transB, transA, m, n, k,
	    &alpha, data_B, ldB, data_A, ldA, &beta, data_C, ldC);
}
#define affine_raw Context(CONTEXT_ARGS)._affine_raw


//offset is used for x time-shift between A and B
//if offset == 1, then we will calculate A[0..end-1] * B[1..end]
void _affine_global(
        Ndarray* A, Ndarray* B, /*out*/Ndarray* C,
        bool transpose_A = false, bool transpose_B = false, int offset = 0, float beta = 1.0) {
	float* data_C = Ndarray_DEV_DATA(C);
	int A_dim[2], B_dim[2];
	lastTwoDims(A, A_dim);
	lastTwoDims(B, B_dim);
	int shiftA = A_dim[1] * A_dim[0];
	int shiftB = B_dim[1] * B_dim[0];
	A_dim[0] = Ndarray_SIZE(A) / A_dim[1] - offset * A_dim[0];
	B_dim[0] = Ndarray_SIZE(B) / B_dim[1] - offset * A_dim[0];
	const float * data_A = Ndarray_DEV_DATA(A);
	const float * data_B = Ndarray_DEV_DATA(B) + offset * shiftB;

	int ldB = B_dim[1];
	int ldA = A_dim[1];
	char transA = transpose_A ? 'T' : 'N';
	char transB = transpose_B ? 'T' : 'N';
	if (transpose_A)
		std::swap(A_dim[0], A_dim[1]);
	if (transpose_B)
		std::swap(B_dim[0], B_dim[1]);

	const float alpha = 1;
	Ndarray_sgemm(transB, transA, B_dim[1], A_dim[0], A_dim[1], &alpha, data_B, ldB,
		data_A, ldA, &beta, data_C, B_dim[1]);
}
#define affine_global Context(CONTEXT_ARGS)._affine_global

};

#if TENSORFLOW
#if !CUDA  // only do in main namespace
//typedef Eigen::ThreadPoolDevice CPUDevice;
//typedef Eigen::GpuDevice GPUDevice;
#endif

#if CUDA
#undef EigenDev
#define EigenDev Eigen::GpuDevice
#else
#define EigenDev Eigen::ThreadPoolDevice
#endif

#endif

#if TENSORFLOW
//void cudaMemcpy ... -> Ndarray_memcpy?

void make_copy(OpKernelContext* context, tensorflow::Tensor* tgt_tensor, const tensorflow::Tensor* src_tensor) {
    // also check https://github.com/tensorflow/tensorflow/blob/master/tensorflow/core/kernels/debug_ops.h, CopyOp
    // also: https://github.com/tensorflow/tensorflow/blob/master/tensorflow/core/kernels/dense_update_ops.cc
    //   https://github.com/tensorflow/tensorflow/blob/master/tensorflow/core/kernels/assign_op.h
    // also see Ndarray_Copy above
    OP_REQUIRES(context, tgt_tensor, errors::InvalidArgument("tgt_tensor not set"));
    OP_REQUIRES(context, src_tensor, errors::InvalidArgument("src_tensor not set"));
    if(!tgt_tensor || !src_tensor) return;
    OP_REQUIRES(context, Ndarray_SIZE(tgt_tensor) == Ndarray_SIZE(src_tensor),
        errors::InvalidArgument("shape sizes do not match, got shapes ",
                                src_tensor->shape().DebugString(), tgt_tensor->shape().DebugString()));
    //Ndarray_memcpy(Ndarray_DEV_DATA(tgt_tensor), Ndarray_DEV_DATA(src_tensor), Ndarray_SIZE(src_tensor) * sizeof(float));
    auto dev = context->eigen_device<EigenDev>();
    assert(tgt_tensor->dtype() == DT_FLOAT);  // not implemented otherwise yet...
    tgt_tensor->flat<float>().device(dev) = src_tensor->flat<float>();
}

template<typename T>
void check_inf_or_nan_cpu(tensorflow::Tensor* v, const std::string& name) {
    // copied from CheckNumericsOp CPU kernel
    auto in = v->flat<T>();
    static const int kInfBit = 0x01;
    static const int kNaNBit = 0x02;
    const T* data = in.data();
    const int64 size = in.size();
    // Check to see if any element of the tensor is NaN or Inf.
    int fp_props =
        std::accumulate(data, data + size, 0, [](const int& x, const T& y) {
          int result = x;
          if (Eigen::numext::isinf(y)) {
            result |= kInfBit;
          } else if (Eigen::numext::isnan(y)) {
            result |= kNaNBit;
          }
          return result;
        });
    string status;
    if ((fp_props & kInfBit) && (fp_props & kNaNBit)) {
      status = "Inf and NaN";
    } else {
      if (fp_props & kInfBit) {
        status = "Inf";
      }
      if (fp_props & kNaNBit) {
        status = "NaN";
      }
    }
    if (!status.empty())
      printf("WARNING: %s: Tensor had %s values!\n", name.c_str(), status.c_str());
}

void _fwrite_uint64(FILE* f, uint64_t v) {
    fwrite(&v, sizeof(uint64_t), 1, f);
}

void _fwrite_str_pascal(FILE* f, const std::string& s) {
    _ns::_fwrite_uint64(f, s.size());
    fwrite(s.data(), s.size(), 1, f);
}

void dump_to_file(tensorflow::Tensor* v, const std::string& name) {
    FILE* f = fopen(name.c_str(), "w");
    _ns::_fwrite_str_pascal(f, "NativeOp_dump");  // header
    _ns::_fwrite_str_pascal(f, tensorflow::DataTypeString(v->dtype()));
    _ns::_fwrite_uint64(f, tensorflow::DataTypeSize(v->dtype()));
    _ns::_fwrite_uint64(f, (uint64_t) v->dims());
    for(int i = 0; i < v->dims(); ++i)
        _ns::_fwrite_uint64(f, (uint64_t) v->dim_size(i));
    tensorflow::StringPiece data = v->tensor_data();
    _ns::_fwrite_uint64(f, (uint64_t) data.size());
    fwrite(data.data(), data.size(), 1, f);
    fclose(f);
}

void debug_print(OpKernelContext* context, tensorflow::Tensor* v, const std::string& name, int64 max_entries=100) {
    // https://github.com/tensorflow/tensorflow/blob/master/tensorflow/core/kernels/debug_ops.h
    std::string full_name = context->op_kernel().name() + ":" + name;
    tensorflow::Tensor cpy(v->dtype(), v->shape());
    if(context->op_device_context()) {  // GPU
        Notification done_copy;
        context->op_device_context()->CopyDeviceTensorToCPU(
            v, name, static_cast<Device*>(context->device()), &cpy,
            [&done_copy](const Status& s) { done_copy.Notify(); });
        done_copy.WaitForNotification();
    }
    else {
        cpy.UnsafeCopyFromInternal(*v, v->dtype(), v->shape());
    }
    printf("%s: %s\n", full_name.c_str(), cpy.DebugString().c_str());
    if(max_entries > 0)
        printf("%s: %s\n", full_name.c_str(), cpy.SummarizeValue(max_entries).c_str());
    if(cpy.dtype() == DT_FLOAT)
        check_inf_or_nan_cpu<float>(&cpy, full_name);
    std::string filename = full_name + ".dump";
    filename = tensorflow::str_util::StringReplace(filename, ":", "_", true);
    filename = tensorflow::str_util::StringReplace(filename, "/", "__", true);
    dump_to_file(&cpy, filename);
}

void debug_print_shape(OpKernelContext* context, tensorflow::Tensor* tensor, const std::string& name) {
    printf("%s info:\n", name.c_str());
    printf("  initialized: %i\n", tensor->IsInitialized());
    printf("  dtype: %s (size %i)\n", DataTypeString(tensor->dtype()).c_str(), DataTypeSize(tensor->dtype()));
    printf("  shape: %s\n", tensor->shape().DebugString().c_str());
    #define _dump_type_dims(NDIM) \
      if(DataTypeString(tensor->dtype()) == "float" && tensor->dims() == NDIM) { \
        const auto& eigen_tensor = tensor->tensor<float, NDIM>(); \
        printf("  eigen rank: %li\n", eigen_tensor.rank()); \
        for(int d = 0; d < eigen_tensor.rank(); ++d) \
          printf("  eigen dim %i: %li\n", d, eigen_tensor.dimension(d)); \
        printf("  eigen data: %p\n", eigen_tensor.data()); \
      }
    _dump_type_dims(1);
    _dump_type_dims(2);
    _dump_type_dims(3);
    printf("  data: %p\n", Ndarray_DEV_DATA(tensor));
}

#endif
