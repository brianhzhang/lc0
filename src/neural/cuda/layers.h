/*
  This file is part of Leela Chess Zero.
  Copyright (C) 2018-2019 The LCZero Authors

  Leela Chess is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Leela Chess is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with Leela Chess.  If not, see <http://www.gnu.org/licenses/>.

  Additional permission under GNU GPL version 3 section 7

  If you modify this Program, or any covered work, by linking or
  combining it with NVIDIA Corporation's libraries from the NVIDIA CUDA
  Toolkit and the NVIDIA CUDA Deep Neural Network library (or a
  modified version of those libraries), containing parts covered by the
  terms of the respective license agreement, the licensors of this
  Program grant you additional permission to convey the resulting work.
*/
#pragma once

#include <cstddef>
#include <cublas_v2.h>

#ifdef USE_CUDNN
#include <cudnn.h>
#else
typedef void* cudnnHandle_t;
#endif

namespace lczero {
namespace cudnn_backend {

// The Layer objects only hold memory for weights, biases, etc
// memory for input and output tensors is provided by caller of Eval.

template <typename DataType>
class BaseLayer {
 public:
  int GetC() const { return C; }
  int GetH() const { return H; }
  int GetW() const { return W; }

  BaseLayer(int c, int h, int w, BaseLayer* ip);
  BaseLayer(int c, int h, int w, BaseLayer* ip, bool nhwc);
  virtual ~BaseLayer() = default;
  size_t GetOutputSize(int N) const { return sizeof(DataType) * N * C * H * W; }

  // Input2 is optional (skip connection).
  virtual void Eval(int N, DataType* output, const DataType* input,
                    const DataType* input2, void* scratch, size_t scratch_size,
                    cudnnHandle_t cudnn, cublasHandle_t cublas) = 0;

  DataType* cache_;

 protected:
  BaseLayer* input_;

  int C;  // Output tensor dimensions.
  int H;
  int W;

  bool nhwc_;   // tensor layout
};

#ifdef USE_CUDNN
template <typename DataType>
class ConvLayer : public BaseLayer<DataType> {
  using BaseLayer<DataType>::C;
  using BaseLayer<DataType>::H;
  using BaseLayer<DataType>::W;
  using BaseLayer<DataType>::GetC;
  using BaseLayer<DataType>::GetH;
  using BaseLayer<DataType>::GetW;
  using BaseLayer<DataType>::nhwc_;

 public:
  ConvLayer(BaseLayer<DataType>* ip, int C, int H, int W, int size, int Cin,
            bool relu = false, bool bias = false);
  
  ConvLayer(bool nhwc, int C, int H, int W, int size, int Cin,
            bool relu = false, bool bias = false);

  ~ConvLayer();
  void LoadWeights(float* pfilter, float* pBias, void* scratch);
  void Eval(int N, DataType* output, const DataType* input,
            const DataType* input2, void* scratch, size_t scratch_size,
            cudnnHandle_t cudnn, cublasHandle_t cublas) override;

 private:
  const int c_input_;
  const int filter_size_;
  const bool use_relu_;
  const bool use_bias_;

  DataType* biases = nullptr;
  DataType* weights = nullptr;

  cudnnFilterDescriptor_t filter_desc_;
  cudnnConvolutionDescriptor_t conv_desc_;
  cudnnConvolutionFwdAlgo_t conv_algo_;

  cudnnTensorDescriptor_t bias_desc_;
  cudnnTensorDescriptor_t in_tensor_desc_;
  cudnnTensorDescriptor_t out_tensor_desc_;
  cudnnActivationDescriptor_t activation_;

  void init();
};

template <typename DataType>
class SoftMaxLayer : public BaseLayer<DataType> {
  using BaseLayer<DataType>::GetC;
  using BaseLayer<DataType>::GetH;
  using BaseLayer<DataType>::GetW;
  using BaseLayer<DataType>::nhwc_;

 public:
  SoftMaxLayer(BaseLayer<DataType>* ip);
  ~SoftMaxLayer();
  void Eval(int N, DataType* output, const DataType* input,
            const DataType* input2, void* scratch, size_t scratch_size,
            cudnnHandle_t cudnn, cublasHandle_t cublas) override;

 private:
  cudnnTensorDescriptor_t out_tensor_desc_;
};
#endif

template <typename DataType>
class FCLayer : public BaseLayer<DataType> {
 using BaseLayer<DataType>::nhwc_;

 public:
  FCLayer(BaseLayer<DataType>* ip, int C, int H, int W, bool relu, bool bias,
          bool tanh = false, bool sigmoid = false);
  ~FCLayer();

  void LoadWeights(float* cpuWeight, float* cpuBias, void* scratch);
  void Eval(int N, DataType* output, const DataType* input,
            const DataType* input2, void* scratch, size_t scratch_size,
            cudnnHandle_t cudnn, cublasHandle_t cublas) override;

 private:
  const bool use_bias_;
  const bool use_relu_;
  const bool use_tanh_;
  const bool use_sigmoid_;
  DataType* weights_ = nullptr;
  DataType* biases_ = nullptr;
};

template <typename DataType>
class PolicyMapLayer: public BaseLayer<DataType> {
 using BaseLayer<DataType>::nhwc_;

 public:
  PolicyMapLayer(BaseLayer<DataType>* ip, int C, int H, int W, int usedSize);
  ~PolicyMapLayer();

  void LoadWeights(const short* cpuWeight, void* scratch);
  void Eval(int N, DataType* output, const DataType* input,
            const DataType* input2, void* scratch, size_t scratch_size,
            cudnnHandle_t cudnn, cublasHandle_t cublas) override;

 private:
  int used_size_; // Size of the input without padding (typically 73x64).
                  // This is over-written to contain size with padding 
                  // (typically 80x64) after CHW->HWC conversion for fp16.
  short* weights_ = nullptr;
};

// Fused SE layer:
// (optional bias add +) global avg -> FC1 -> FC2 -> global scale -> add skip
// connection -> RELU.
template <typename DataType>
class SELayer : public BaseLayer<DataType> {
 using BaseLayer<DataType>::C;
 using BaseLayer<DataType>::nhwc_;

 public:
  SELayer(BaseLayer<DataType>* ip, int numFc1Out,
          bool addPrevLayerBias = false);
  ~SELayer();

  void LoadWeights(float* w1, float* b1, float* w2, float* b2,
                   float* prevLayerBias, void* scratch);

  void Eval(int N, DataType* output, const DataType* input,
            const DataType* input2, void* scratch, size_t scratch_size,
            cudnnHandle_t cudnn, cublasHandle_t cublas) override;

 private:
  DataType* w1_ = nullptr;
  DataType* w1_t_ = nullptr;    // transposed copy used by fused SE kernel
  DataType* b1_ = nullptr;
  DataType* w2_ = nullptr;
  DataType* w2_t_ = nullptr;
  DataType* b2_ = nullptr;
  DataType* bPrev_ = nullptr;
  int numFc1Out_;
  bool addPrevLayerBias_;
};


// Multi-pass Winograd Conv fused with (optional) SE
template <typename DataType>
class FusedWinogradConvSELayer : public BaseLayer<DataType> {
  using BaseLayer<DataType>::C;
  using BaseLayer<DataType>::H;
  using BaseLayer<DataType>::W;
  using BaseLayer<DataType>::GetC;
  using BaseLayer<DataType>::GetH;
  using BaseLayer<DataType>::GetW;
  using BaseLayer<DataType>::nhwc_;

 public:
  FusedWinogradConvSELayer(BaseLayer<DataType>* ip, int C, int H, int W,
                         int Cin, bool relu, bool bias, bool skipAdd, bool se,
                           int se_k, bool use_gemm_ex);

  ~FusedWinogradConvSELayer();
  void LoadWeights(float* pfilter, float* pBias, void* scratch);
  void LoadSEWeights(float* w1, float* b1, float* w2, float* b2, void *scratch);
  void Eval(int N, DataType* output, const DataType* input,
            const DataType* input2,
            void* scratch, size_t scratch_size,
            cudnnHandle_t cudnn, cublasHandle_t cublas) override;

 private:
  const int c_input_;
  const bool use_relu_;
  const bool use_bias_;
  const bool skip_add_;
  const bool has_se_;
  const int se_k_;
  const bool use_gemm_ex_;

  DataType* biases_ = nullptr;
  DataType* transformed_weights_ = nullptr;  // After winograd transform.

  // Weights and Biases for (optional) SE.
  DataType* w1_;
  DataType* w2_;
  DataType* b1_;
  DataType* b2_;

 void cublasRowMajorMatrixMul(const DataType* A, const DataType* B,
                               DataType* Out, int M, int N, int K,
                               int batchSize, cublasHandle_t cublas);
};

template <typename DataType>
class Conv1Layer : public BaseLayer<DataType> {
  using BaseLayer<DataType>::C;
  using BaseLayer<DataType>::H;
  using BaseLayer<DataType>::W;
  using BaseLayer<DataType>::GetC;
  using BaseLayer<DataType>::GetH;
  using BaseLayer<DataType>::GetW;
  using BaseLayer<DataType>::nhwc_;

 public:
  Conv1Layer(BaseLayer<DataType>* ip, int C, int H, int W,
                         int Cin, bool relu, bool bias, bool use_gemm_ex);

  ~Conv1Layer();
  void LoadWeights(float* pfilter, float* pBias, void* scratch);
  void Eval(int N, DataType* output, const DataType* input,
            const DataType* input2,
            void* scratch, size_t scratch_size,
            cudnnHandle_t cudnn, cublasHandle_t cublas) override;

 private:
  const int c_input_;
  const bool use_relu_;
  const bool use_bias_;
  const bool use_gemm_ex_;

  DataType* biases_ = nullptr;
  DataType* weights_ = nullptr;

 void cublasRowMajorMatrixMul(const DataType* A, const DataType* B,
                               DataType* Out, int M, int N, int K,
                               int batchSize, cublasHandle_t cublas);
};

// Multi-pass Winograd Conv fused with (optional) SE
template <typename DataType>
class ResidualBlock : public BaseLayer<DataType> {
  using BaseLayer<DataType>::C;
  using BaseLayer<DataType>::H;
  using BaseLayer<DataType>::W;
  using BaseLayer<DataType>::GetC;
  using BaseLayer<DataType>::GetH;
  using BaseLayer<DataType>::GetW;

 public:
  ResidualBlock(BaseLayer<DataType>* ip, int C, bool se, int se_k, bool use_gemm_ex, bool first, bool last);

  ~ResidualBlock();
  void LoadWeights0(float* pfilter, float* pBias, void* scratch);
  void LoadWeights1(float* pfilter, float* pBias, void* scratch);
  void LoadSEWeights(float* w1, float* b1, float* w2, float* b2, void* scratch);

  void Eval(int N, DataType* output, const DataType* input,
            const DataType* input2, void* scratch, size_t scratch_size,
            cudnnHandle_t cudnn, cublasHandle_t cublas) override;

 private:
  const bool has_se_;
  const int se_k_;
  const bool use_gemm_ex_;
  const int c_input_;
  const bool first_block_;
  const bool last_block_;

  DataType* biases0_ = nullptr;
  DataType* biases1_ = nullptr;
  DataType* transformed_weights0_ = nullptr;  // After winograd transform.
  DataType* transformed_weights1_ = nullptr;  // After winograd transform.

  // Weights and Biases for (optional) SE.
  DataType* w1_;
  DataType* w2_;
  DataType* b1_;
  DataType* b2_;

  void cublasRowMajorMatrixMul(const DataType* A, const DataType* B,
                               DataType* Out, int M, int N, int K,
                               int batchSize, cublasHandle_t cublas);
};

}  // namespace cudnn_backend
}  // namespace lczero
