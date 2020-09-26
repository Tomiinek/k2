/**
 * @brief
 * array_ops_inl
 *
 * @note
 * Don't include this file directly; it is included by array_ops.h.
 * It contains implementation code.
 *
 * @copyright
 * Copyright (c)  2020  Xiaomi Corporation (authors: Daniel Povey
 *                                                   Haowen Qiu)
 *                      Fangjun Kuang (csukuangfj@gmail.com)
 * @copyright
 * See LICENSE for clarification regarding multiple authors
 */

#ifndef K2_CSRC_ARRAY_OPS_INL_H_
#define K2_CSRC_ARRAY_OPS_INL_H_

#ifndef IS_IN_K2_CSRC_ARRAY_OPS_H_
#error "this file is supposed to be included only by array_ops.h"
#endif

#include <algorithm>
#include <cassert>
#include <cub/cub.cuh>  // NOLINT
#include <type_traits>
#include <utility>
#include <vector>

#include "k2/csrc/utils.h"

namespace k2 {
namespace internal {
// Will be used in ExclusiveSumDeref to call ExclusiveSum (which calls
// cub::DeviceScan::ExclusiveSum internally).
template <typename T>
struct PtrPtr {
  T **data;

  explicit PtrPtr(T **data) : data(data) {}

  // operator[] and operator+ are required by cub::DeviceScan::ExclusiveSum
  __host__ __device__ T operator[](int32_t i) { return *(data[i]); }
  __host__ __device__ PtrPtr operator+(int32_t n) const {
    PtrPtr tmp(*this);
    tmp.data += n;
    return tmp;
  }
};

// TODO(haowen): manage/load block config with some classes? then we can get
// different configuration depending on num_elements and data type.
// block size for matrix transpose.
static constexpr int32_t kTransTileDim = 32;
static constexpr int32_t kTransBlockRows = 8;

template <typename T>
__global__ void TransposeKernel(int32_t rows, int32_t cols,
                                int32_t input_elem_stride0,
                                int32_t output_elem_stride0, const T *input,
                                T *output) {
  // TODO(haowen): here we need to handle different type of T to avoid bank
  // conflicts, the size of cache now is fine for type size with 32bit (e.g.
  // int32_t or float).
  __shared__ T cache[kTransTileDim][kTransTileDim + 1];

  // input index, in a coalesced manner.
  int32_t x = threadIdx.x + blockIdx.x * kTransTileDim;
  int32_t y = threadIdx.y + blockIdx.y * kTransTileDim;

  for (int32_t i = 0; i < kTransTileDim; i += kTransBlockRows) {
    if (x < cols && (y + i) < rows) {
      cache[threadIdx.y + i][threadIdx.x] =
          input[(y + i) * input_elem_stride0 + x];
    }
  }

  __syncthreads();

  // output index, in a coalesced manner
  x = threadIdx.x + blockIdx.y * kTransTileDim;
  y = threadIdx.y + blockIdx.x * kTransTileDim;
  for (int32_t i = 0; i < kTransTileDim; i += kTransBlockRows) {
    if (x < rows && (y + i) < cols) {
      output[(y + i) * output_elem_stride0 + x] =
          cache[threadIdx.x][threadIdx.y + i];
    }
  }
}

// will be called in ExclusiveSum(Array2 &src, Array2 *dest, int32_t axis)
// to compute exclusive sum for each row
template <typename T>
void ExclusiveSumPerRow(const Array2<T> &src, Array2<T> *dest) {
  int32_t rows = dest->Dim0();
  // note there may be dest->Dim1() == src.Dim1() + 1
  int32_t cols = dest->Dim1();
  ContextPtr ctx = src.Context();
  ConstArray2Accessor<T> src_acc = src.Accessor();
  Array2Accessor<T> dest_acc = dest->Accessor();
  // TODO(haowen): parallelized it in case dest_minor_dim is large
  for (int32_t i = 0; i != rows; ++i) {
    ExclusiveSum(ctx, cols, src_acc.Row(i), dest_acc.Row(i));
  }
}

}  // namespace internal
}  // namespace k2

namespace std {
// vaule_type is required by cub::DeviceScan::ExclusiveSum
template <typename T>
struct iterator_traits<k2::internal::PtrPtr<T>> {
  typedef T value_type;
};
}  // namespace std

namespace k2 {
template <typename T>
void Transpose(ContextPtr &c, const Array2<T> &src, Array2<T> *dest) {
  K2_CHECK(c->IsCompatible(*src.Context()));
  K2_CHECK(c->IsCompatible(*dest->Context()));
  int32_t rows = src.Dim0();
  int32_t cols = src.Dim1();
  // TODO(haowen): limit the number of elements?
  K2_CHECK_EQ(rows, dest->Dim1());
  K2_CHECK_EQ(cols, dest->Dim0());
  int32_t src_elem_stride0 = src.ElemStride0();
  int32_t dest_elem_stride0 = dest->ElemStride0();
  const T *src_data = src.Data();
  T *dest_data = dest->Data();
  DeviceType d = c->GetDeviceType();
  if (d == kCpu) {
    for (int32_t i = 0; i < cols; ++i) {
      for (int32_t j = 0; j < rows; ++j) {
        dest_data[i * dest_elem_stride0 + j] =
            src_data[j * src_elem_stride0 + i];
      }
    }
  } else {
    K2_CHECK_EQ(d, kCuda);
    dim3 block_size(internal::kTransTileDim, internal::kTransBlockRows, 1);
    dim3 grid_size(NumBlocks(cols, internal::kTransTileDim),
                   NumBlocks(rows, internal::kTransTileDim));
    internal::TransposeKernel<<<grid_size, block_size, 0, c->GetCudaStream()>>>(
        rows, cols, src_elem_stride0, dest_elem_stride0, src_data, dest_data);
    auto ret = cudaDeviceSynchronize();
    K2_CHECK_CUDA_ERROR(ret);
  }
}

template <typename T>
void ExclusiveSumDeref(Array1<T *> &src, Array1<T> *dest) {
  K2_CHECK(IsCompatible(src, *dest));
  int32_t src_dim = src.Dim();
  int32_t dest_dim = dest->Dim();
  K2_CHECK(dest_dim == src_dim || dest_dim == src_dim + 1);
  if (dest_dim == src_dim + 1) {
    const RegionPtr &region = src.GetRegion();
    int32_t byte_offset = src.ByteOffset();
    K2_CHECK_GE(region->num_bytes - byte_offset, dest_dim * src.ElementSize());
  }
  internal::PtrPtr<T> src_data = internal::PtrPtr<T>(src.Data());
  ExclusiveSum(src.Context(), dest_dim, src_data, dest->Data());
}

template <typename T>
void ExclusiveSum(Array2<T> &src, Array2<T> *dest, int32_t axis) {
  K2_CHECK(axis == 0 || axis == 1);
  K2_CHECK(IsCompatible(src, *dest));
  int32_t src_major_dim = src.Dim0();  // the axis will be summed
  int32_t src_minor_dim = src.Dim1();
  int32_t dest_major_dim = dest->Dim0();
  int32_t dest_minor_dim = dest->Dim1();
  if (axis == 1) {
    std::swap(src_major_dim, src_minor_dim);
    std::swap(dest_major_dim, dest_minor_dim);
  }
  K2_CHECK_EQ(dest_minor_dim, src_minor_dim);
  K2_CHECK(dest_major_dim == src_major_dim ||
           dest_major_dim == src_major_dim + 1);
  if (dest_major_dim == src_major_dim + 1) {
    const RegionPtr &region = src.GetRegion();
    int32_t byte_offset = src.ByteOffset();
    K2_CHECK_GE(region->num_bytes - byte_offset,
                (src_major_dim * src_minor_dim + 1) * src.ElementSize());
  }

  if (axis == 1) {
    internal::ExclusiveSumPerRow(src, dest);
  } else {
    ContextPtr ctx = src.Context();
    int32_t elem_size = src.ElementSize();
    // note here we always allocate an extra element for src_trans
    RegionPtr src_trans_region =
        NewRegion(ctx, (src_major_dim * src_minor_dim + 1) * elem_size);
    Array2<T> src_trans(src_minor_dim, src_major_dim, src_major_dim, 0,
                        src_trans_region);
    Transpose(ctx, src, &src_trans);

    RegionPtr dest_trans_region =
        NewRegion(ctx, dest_major_dim * dest_minor_dim * elem_size);
    Array2<T> dest_trans(dest_minor_dim, dest_major_dim, dest_major_dim, 0,
                         dest_trans_region);
    internal::ExclusiveSumPerRow(src_trans, &dest_trans);
    Transpose(ctx, dest_trans, dest);
  }
}

// CAUTION: if you fix bugs in this code, please also fix the same bugs in
// Splice() in array_ops.cu, since it was modified from this code.
template <typename T>
Array1<T> Append(int32_t num_arrays, const Array1<T> **src) {
  K2_CHECK_GT(num_arrays, 0);
  ContextPtr c = src[0]->Context();

  std::vector<int32_t> row_splits_vec(num_arrays + 1);
  int32_t sum = 0, max_dim = 0;
  row_splits_vec[0] = sum;
  for (int32_t i = 0; i < num_arrays; i++) {
    int32_t dim = src[i]->Dim();
    if (dim > max_dim) max_dim = dim;
    sum += dim;
    row_splits_vec[i + 1] = sum;
  }
  int32_t ans_size = sum;

  Array1<T> ans(c, ans_size);
  T *ans_data = ans.Data();

  if (c->GetDeviceType() == kCpu) {
    // a simple loop is faster, although the other branches should still work on
    // CPU.
    for (int32_t i = 0; i < num_arrays; i++) {
      int32_t offset = row_splits_vec[i], this_dim = src[i]->Dim();
      const int32_t *this_src_data = src[i]->Data();
      memcpy(static_cast<void *>(ans_data),
             static_cast<const void *>(this_src_data), sizeof(T) * this_dim);
      ans_data += this_dim;
    }
  } else {
    K2_CHECK_EQ(c->GetDeviceType(), kCuda);
    Array1<int32_t> row_splits(c, row_splits_vec);
    const int32_t *row_splits_data = row_splits.Data();
    std::vector<T *> src_ptrs_vec(num_arrays);
    for (int32_t i = 0; i < num_arrays; i++) src_ptrs_vec[i] = src[i]->Data();
    Array1<T> src_ptrs(c, src_ptrs_vec);
    auto src_ptrs_data = src_ptrs.Data();
    int32_t avg_input_size = ans_size / num_arrays;
    if (max_dim < 2 * avg_input_size + 512) {
      // here, 2 is a heuristic factor. We're saying, "if the max length of any
      // of the source arrays is not too much larger than the average length of
      // the source arrays."  The `+ 512` is an additional heuristic factor, as
      // we care less about launching too many GPU threads if the number of
      // elements being processed is small. What we're saying is that the
      // arrays' sizes are fairly balanced, so we launch with a simple
      // rectangular kernel.
      auto lambda_set_data = [=] __host__ __device__(int32_t i,
                                                     int32_t j) -> void {
        // TODO(haowen): change to use operator[]
        int32_t row_start = row_splits.Data()[i],
                row_end = row_splits.Data()[i + 1];
        const T *src_ptr = src_ptrs_data[i];
        if (j < row_end - row_start) {
          ans_data[row_start + j] = src_ptr[j];
        }
      };
      Eval2(c, num_arrays, max_dim, lambda_set_data);
    } else {
      int32_t block_dim = 256;
      while (block_dim * 4 < avg_input_size && block_dim < 8192) block_dim *= 2;

      // `index_map` will map from 'new index' to 'old index', with 0 <=
      // old_index < num_arrays... we handle each source array with multiple
      // blocks.
      //  The elements of `index_map` will be of the form:
      //    old_index + (block_of_this_array << 32).
      // where `old_index` is an index into `src` and `block_of_this_array`
      // tells us which block it is, as in 0, 1, 2, 3...
      // there won't be very many blocks, so it's not a problem to enumerate
      // them on CPU.
      std::vector<uint64_t> index_map;
      index_map.reserve((2 * ans_size) / block_dim);
      for (int32_t i = 0; i < num_arrays; i++) {
        int32_t this_array_size = src[i]->Dim();
        int32_t this_num_blocks = (this_array_size + block_dim - 1) / block_dim;
        for (int32_t j = 0; j < this_num_blocks; j++) {
          index_map.push_back((((uint64_t)j) << 32) + (uint64_t)i);
        }
      }
      Array1<uint64_t> index_map_gpu(c, index_map);
      const uint64_t *index_map_data = index_map_gpu.Data();

      auto lambda_set_data_blocks = [=] __host__ __device__(int32_t i,
                                                            int32_t j) {
        uint64_t index = index_map_data[i];
        uint32_t orig_i = (uint32_t)index,
                 block_index = (uint32_t)(index >> 32);
        int32_t row_start = row_splits.Data()[orig_i],
                row_end = row_splits.Data()[orig_i + 1],
                orig_j = (block_index * block_dim) + j;
        const T *src_ptr = src_ptrs_data[orig_i];
        if (orig_j < row_end - row_start) {
          ans_data[row_start + orig_j] = src_ptr[orig_j];
        }
      };
      Eval2(c, index_map_gpu.Dim(), block_dim, lambda_set_data_blocks);
    }
  }
}

template <typename T>
Array1<T> Append(int32_t src_size, Array1<T> *src) {
  K2_CHECK_GT(src_size, 0);
  std::vector<Array1<T> *> srcs(src_size);
  for (int32_t i = 0; i < src_size; i++) srcs[i] = src + i;
  // TODO(haowen): add below interfaces.
  // return Append(&(srcs[0]));
  K2_LOG(FATAL) << "Not Implemented";
  return src[0];
}

template <typename T>
void MaxPerSublist(Ragged<T> &src, T default_value, Array1<T> *max_values) {
  K2_CHECK_EQ(src.NumAxes(), 2);
  K2_CHECK_EQ(src.shape.Dim0(), max_values->Dim());
  K2_CHECK(IsCompatible(src.shape, *max_values));

  ContextPtr c = src.Context();
  int32_t num_rows = src.shape.Dim0();
  const int32_t *row_splits = src.shape.RowSplits(1).Data();
  const T *values_data = src.values.Data();
  T *output_data = max_values->Data();

  if (c->GetDeviceType() == kCpu) {
    int32_t j = row_splits[0];
    for (int32_t i = 0; i < num_rows; i++) {
      T max_val = default_value;
      int32_t row_end = row_splits[i + 1];
      for (; j < row_end; j++) {
        T elem = values_data[j];
        max_val = (elem > max_val ? elem : max_val);
      }
      output_data[i] = max_val;
    }
  } else {
    K2_CHECK(c->GetDeviceType() == kCuda);

    // This code is based on the example here:
    // https://nvlabs.github.io/cub/structcub_1_1_device_segmented_reduce.html
    MaxOp<T> max_op;
    void *d_temp_storage = NULL;
    std::size_t temp_storage_bytes = 0;

    // the first time is to determine temporary device storage requirements
    K2_CUDA_SAFE_CALL(cub::DeviceSegmentedReduce::Reduce(
        d_temp_storage, temp_storage_bytes, values_data, output_data, num_rows,
        row_splits, row_splits + 1, max_op, default_value, c->GetCudaStream()));
    void *deleter_context;
    d_temp_storage = c->Allocate(temp_storage_bytes, &deleter_context);
    K2_CUDA_SAFE_CALL(cub::DeviceSegmentedReduce::Reduce(
        d_temp_storage, temp_storage_bytes, values_data, output_data, num_rows,
        row_splits, row_splits + 1, max_op, default_value, c->GetCudaStream()));
    c->Deallocate(d_temp_storage, deleter_context);
  }
}

template <typename T>
void And(Array1<T> &src, T default_value, Array1<T> *dest) {
  // TODO(haowen): implement
  K2_LOG(FATAL) << "Not implemented";
}

template <typename T>
Array1<T> RandUniformArray1(ContextPtr &c, int32_t dim, T min_value,
                            T max_value) {
  Array1<T> temp(GetCpuContext(), dim);
  T *data = temp.Data();
  K2_CHECK(max_value >= min_value);
  if (max_value == min_value) {
    for (int32_t i = 0; i < dim; ++i) data[i] = 0;
  } else if (std::is_floating_point<T>::value ||
             std::abs(min_value) > RAND_MAX || std::abs(max_value) > RAND_MAX) {
    for (int32_t i = 0; i < dim; i++)
      data[i] =
          min_value + (rand() * (max_value - min_value) / RAND_MAX);  // NOLINT
  } else {
    for (int32_t i = 0; i < dim; ++i)
      data[i] =
          min_value +
          (rand() % static_cast<int32_t>(max_value + 1 - min_value));  // NOLINT
  }
  return temp.To(c);
}

template <typename T>
Array1<T> Range(ContextPtr &c, int32_t dim, T first_value, T inc /*=1*/) {
  K2_CHECK(dim >= 0);
  DeviceType d = c->GetDeviceType();
  Array1<T> ans = Array1<T>(c, dim);
  T *ans_data = ans.Data();
  if (d == kCpu) {
    for (int32_t i = 0; i < dim; i++) ans_data[i] = first_value + i * inc;
  } else {
    auto lambda_set_values = [=] __host__ __device__(int32_t i) -> void {
      ans_data[i] = first_value + i * inc;
    };
    Eval(c, dim, lambda_set_values);
  }
  return ans;
}

template <typename T>
Array2<T> ToContiguous(const Array2<T> &src) {
  int32_t dim0 = src.Dim0();
  int32_t dim1 = src.Dim1();
  int32_t elem_stride0 = src.ElemStride0();
  if (dim1 == elem_stride0) return src;
  Array2<T> ans(src.Context(), src.Dim0(), src.Dim1());
  T *out = ans.Data();
  const T *in = src.Data();
  auto lambda_copy_elems = [=] __host__ __device__(int32_t i,
                                                   int32_t j) -> void {
    out[i * dim1 + j] = in[i * elem_stride0 + j];
  };
  Eval2(src.Context(), dim0, dim1, lambda_copy_elems);
  return ans;
}

}  // namespace k2

#endif  // K2_CSRC_ARRAY_OPS_INL_H_