#include <ATen/ATen.h>
#include <ATen/NativeFunctions.h>
#include <ATen/AccumulateType.h>
#include <ATen/CPUApplyUtils.h>
#include <ATen/Parallel.h>
#include <ATen/Config.h>

#include <ATen/detail/CUDAHooksInterface.h>

#include <vector>

static const int MIOPEN_DIM_MAX = 4;

namespace at { namespace native {

namespace {
  void check_dims_match_num_input_features(const char* arg_name, int64_t expected, int64_t actual){
    AT_CHECK(actual == expected,
             arg_name, " should contain ", expected, " elements not ", actual);
  }

  static inline Tensor repeat_if_defined(const Tensor& t, int64_t repeat) {
    if (t.defined()) {
      return t.repeat(repeat);
    }
    return t;
  }
}

// TensorAccessor when it is defined to work around undefined...
template <typename scalar_t>
static TensorAccessor<scalar_t, 1> conditional_accessor_1d(const Tensor& t) {
  if (! t.defined()) {
    return TensorAccessor<scalar_t, 1>(nullptr, nullptr, nullptr);
  }
  return t.accessor<scalar_t, 1>();
}

template<typename T>
struct InvStd {
  T operator()(T var, double epsilon) const {
    T invstd = 0;
    if (var != static_cast<T>(0) || epsilon != static_cast<T>(0)) {
      invstd = static_cast<T>(1) / std::sqrt(var + epsilon);
    }
    return invstd;
  }
};

template<typename T>
struct Var {
  T operator()(T var, double epsilon) const {
    return var;
  }
};

/// A fast path for CPU inference when all tensors are contiguous.
/// This code achieves machine bandwidth peak without AVX support.
/// If this changes for future architectures, we can move it to the cpu/
/// directory.
template<typename scalar_t>
void batch_norm_cpu_inference_contiguous(Tensor& output, const Tensor& input,
    const Tensor& weight /* optional */, const Tensor& bias /* optional */,
    const Tensor& mean, const Tensor& variance, double eps) {
  int64_t n_batch = input.size(0);
  int64_t n_channel = input.size(1);
  int64_t image_size = input.numel() / n_batch / n_channel;

  scalar_t* output_data = output.data<scalar_t>();
  const scalar_t* input_data = input.data<scalar_t>();
  const scalar_t* weight_data = weight.defined() ? weight.data<scalar_t>() : nullptr;
  const scalar_t* bias_data = bias.defined() ? bias.data<scalar_t>() : nullptr;
  const scalar_t* mean_data = mean.data<scalar_t>();
  const scalar_t* var_data = variance.data<scalar_t>();

  /// Collect the linear and constant terms regarding the input.
  /// output(n, c, h, w)
  ///     = (input(n, c, h, w) - mean(c)) / sqrt(var(c) + eps) * weight(c)
  ///         + bias(c)
  ///     = input(n, c, h, w) * inv_var(c) * weight(c) +
  ///         - mean(c) * inv_var(c) * weight(c) + bias(c),
  /// where inv_var(c) = 1 / sqrt(var(c) + eps).
  /// So the linear term, alpha(c) = inv_var(c) * weight(c),
  ///   the constant term beta(c) = bias(c) - mean(c) * inv_var(c) * weight(c)
  /// Note that this is only a good idea if (input_size >> c), in degenerate
  /// cases where image_size == 1 && batch_size == 1, it is slow.
  Tensor alpha = at::empty_like(mean);
  Tensor beta = at::empty_like(mean);
  scalar_t* alpha_data = alpha.data<scalar_t>();
  scalar_t* beta_data = beta.data<scalar_t>();
  for (int64_t c = 0; c < n_channel; c++) {
    scalar_t inv_var = 1 / std::sqrt(var_data[c] + static_cast<scalar_t>(eps));
    scalar_t weight_v = weight_data ? weight_data[c] : 1;
    scalar_t bias_v = bias_data ? bias_data[c] : 0;
    alpha_data[c] = inv_var * weight_v;
    beta_data[c] = bias_v - mean_data[c] * inv_var * weight_v;
  }

  // Apply the linear terms to the input,
  // output(n, c, h, w) = input(n, c, h, w) * alpha(c) + beta(c)
  // No need to use parallel_for as this function is supposed to be
  // memory-limited.
  // Keep the loop struture simple to make sure compiler vetorization kicks in.
  if (image_size != 1) {
    for (int64_t n = 0; n < n_batch; ++n) {
      for (int64_t c = 0; c < n_channel; ++c) {
        for (int64_t i = 0; i < image_size; ++i) {
          // Keep all the offset calculation within the inner loop for
          // simplicity. Compilers are very good at hoisting the common part
          // outside.
          int64_t offset = n * n_channel * image_size + c * image_size + i;
          output_data[offset] = input_data[offset] * alpha_data[c] +
              beta_data[c];
        }
      }
    }
  } else {
    // image_size == 1
    for (int64_t n = 0; n < n_batch; ++n) {
      for (int64_t c = 0; c < n_channel; ++c) {
        int64_t offset = n * n_channel + c;
        output_data[offset] = input_data[offset] * alpha_data[c] + beta_data[c];
      }
    }
  }
}

template<typename scalar_t>
std::tuple<Tensor,Tensor,Tensor> batch_norm_cpu_transform_input_template(
    const Tensor& input, const Tensor& weight, const Tensor& bias,
    const Tensor& save_mean /* optional */, const Tensor& save_invstd /* optional */,
    const Tensor& running_mean /* optional */, const Tensor& running_var /* optional */,
    bool train, double eps) {

  Tensor output = at::empty_like(input);

  // Check if we should use the fast path.
  if (!train && input.is_contiguous()
      && (!weight.defined() || weight.is_contiguous())
      && (!bias.defined() || bias.is_contiguous())
      && running_mean.is_contiguous()
      && running_var.is_contiguous()) {
    batch_norm_cpu_inference_contiguous<scalar_t>(output, input, weight, bias,
      running_mean, running_var, eps);
    return std::make_tuple(output, save_mean, save_invstd);
  }
  int64_t n_input = input.size(1);

  auto save_mean_a = conditional_accessor_1d<scalar_t>(save_mean);
  auto save_invstd_a = conditional_accessor_1d<scalar_t>(save_invstd);

  auto running_mean_a = conditional_accessor_1d<scalar_t>(running_mean);
  auto running_var_a = conditional_accessor_1d<scalar_t>(running_var);

  parallel_for(0, n_input, 1, [&](int64_t b_begin, int64_t b_end) {
    for (int64_t f = b_begin; f < b_end; ++f) {
      Tensor in = input.select(1, f);
      Tensor out = output.select(1, f);

      scalar_t mean, invstd;
      if (train) {
        mean = save_mean_a[f];
        invstd = save_invstd_a[f];
      } else {
        mean = running_mean_a[f];
        invstd = 1 / std::sqrt(running_var_a[f] + eps);
      }

      // compute output
      scalar_t w = weight.defined() ? weight.data<scalar_t>()[f * weight.stride(0)] : 1;
      scalar_t b = bias.defined() ? bias.data<scalar_t>()[f * bias.stride(0)] : 0;

      CPU_tensor_apply2<scalar_t,scalar_t>(out, in, [&](scalar_t& o, const scalar_t& i) {
        o = ((i - mean) * invstd) * w + b;
      });
    }
  });
  return std::make_tuple(output, save_mean, save_invstd);
}

template<typename scalar_t, template<typename T> class VarTransform>
std::tuple<Tensor,Tensor> batch_norm_cpu_update_stats_template(
    const Tensor& input, const Tensor& running_mean, const Tensor& running_var,
    double momentum, double eps) {

  using accscalar_t = at::acc_type<scalar_t, false>;

  int64_t n_input = input.size(1);
  int64_t n = input.numel() / n_input;

  Tensor save_mean = at::empty({n_input}, input.options());
  Tensor save_var_transform = at::empty({n_input}, input.options());
  auto save_mean_a = save_mean.accessor<scalar_t, 1>();
  auto save_var_transform_a = save_var_transform.accessor<scalar_t, 1>();

  auto running_mean_a = conditional_accessor_1d<scalar_t>(running_mean);
  auto running_var_a = conditional_accessor_1d<scalar_t>(running_var);

  parallel_for(0, n_input, 1, [&](int64_t b_begin, int64_t b_end) {
    for (int64_t f = b_begin; f < b_end; ++f) {
      Tensor in = input.select(1, f);

      // compute mean per input
      accscalar_t sum = 0;
      CPU_tensor_apply1<scalar_t>(in, [&] (const scalar_t& i) {
          sum += i;
        });
      scalar_t mean = sum / n;
      save_mean_a[f] = mean;

      // compute variance per input
      accscalar_t var_sum = 0;
      CPU_tensor_apply1<scalar_t>(in, [&] (const scalar_t& i) {
        var_sum += (i - mean) * (i - mean);
      });
      save_var_transform_a[f] = VarTransform<accscalar_t>{}(var_sum / n, eps);

      // update running averages
      if (running_mean.defined()) {
        running_mean_a[f] = momentum * mean + (1 - momentum) * running_mean_a[f];
      }
      if (running_var.defined()) {
        accscalar_t unbiased_var = var_sum / (n - 1);
        running_var_a[f] = momentum * unbiased_var + (1 - momentum) * running_var_a[f];
      }
    }
  });
  return std::make_tuple(save_mean, save_var_transform);
}


template<typename scalar_t>
std::tuple<Tensor, Tensor, Tensor> batch_norm_backward_cpu_template(const Tensor& grad_out_, const Tensor& input, const Tensor& weight,
                                                                    const Tensor& running_mean, const Tensor& running_var, const Tensor& save_mean, const Tensor& save_invstd,
                                                                    bool train, double eps, std::array<bool,3> grad_input_mask) {

  using accscalar_t = at::acc_type<scalar_t, false>;

  Tensor grad_input;
  Tensor grad_weight;
  Tensor grad_bias;
  if (grad_input_mask[0]) {
    grad_input = at::empty_like(input);
  }
  if (grad_input_mask[1]) {
    grad_weight = at::empty_like(weight);
  }
  if (grad_input_mask[2]) {
    grad_bias = at::empty_like(weight);
  }

  auto weight_a = conditional_accessor_1d<scalar_t>(weight);
  auto grad_weight_a = conditional_accessor_1d<scalar_t>(grad_weight);
  auto grad_bias_a = conditional_accessor_1d<scalar_t>(grad_bias);

  int64_t n_input = input.size(1);
  int64_t n = input.numel() / n_input;

  auto save_mean_a = conditional_accessor_1d<scalar_t>(save_mean);
  auto save_invstd_a = conditional_accessor_1d<scalar_t>(save_invstd);

  auto running_mean_a = conditional_accessor_1d<scalar_t>(running_mean);
  auto running_var_a = conditional_accessor_1d<scalar_t>(running_var);


  parallel_for(0, n_input, 1, [&](int64_t b_begin, int64_t b_end) {
      for (int64_t f = b_begin; f < b_end; ++f) {
        Tensor in = input.select(1, f);
        Tensor grad_out = grad_out_.select(1, f);

        scalar_t w = weight.defined() ? weight_a[f] : 1;

        scalar_t mean, invstd;
        if (train) {
          mean = save_mean_a[f];
          invstd = save_invstd_a[f];
        } else {
          mean = running_mean_a[f];
          invstd = 1 / std::sqrt(running_var_a[f] + eps);
        }

        // sum over all gradOutput in feature plane
        accscalar_t sum = 0;
        CPU_tensor_apply1<scalar_t>(grad_out, [&](const scalar_t& g) {
            sum += g;
          });

        // dot product of the Q(X) and gradOuput
        accscalar_t dotp = 0;
        CPU_tensor_apply2<scalar_t,scalar_t>(in, grad_out, [&](const scalar_t& i, const scalar_t& go) {
            dotp += (i - mean) * go;
          });

        if (grad_input_mask[0]) {
          Tensor grad_in = grad_input.select(1, f);
          if (train) {
            // when in training mode
            // Q(X) = X - E[x] ; i.e. input centered to zero mean
            // Y = Q(X) / ??    ; i.e. BN output before weight and bias
            // dL/dX = (Q(dL/dY) - dot(Y, dL/dY) * Y) / ?? * w

            // projection of gradOutput on to output scaled by std
            scalar_t k = (scalar_t) dotp * invstd * invstd / n;

            CPU_tensor_apply2<scalar_t,scalar_t>(grad_in, in, [&](scalar_t& gi, const scalar_t& i) {
                gi = (i - mean)* k;
              });

            accscalar_t grad_mean = sum / n;
            CPU_tensor_apply2<scalar_t,scalar_t>(grad_in, grad_out, [&](scalar_t& gi, const scalar_t& go) {
            gi = (go - grad_mean - gi) * invstd * w;
              });
          } else {
            // when in evaluation mode
            // Q(X) = X - running_mean  ; i.e. input centered to zero mean
            // Y = Q(X) / running_std    ; i.e. BN output before weight and bias
            // dL/dX = w / running_std
            CPU_tensor_apply2<scalar_t,scalar_t>(grad_in, grad_out, [&](scalar_t& gi, const scalar_t& go) {
                gi = go * invstd * w;
              });
          }
        }
        if (grad_input_mask[1]) {
          grad_weight_a[f] = dotp * invstd;
        }

        if (grad_input_mask[2]) {
          grad_bias_a[f] = sum;
        }
      }
    });
  return std::make_tuple(grad_input, grad_weight, grad_bias);
}

// _batch_norm_impl_index(_backward) are used in the JIT be able to keep the run-time selection
// of backends, while enabling it to keep the information about the used backend, so that it can
// use its corresponding backward implementation.
// XXX: The indices of backends need to be kept synchronized between this function and its _backward.
std::tuple<Tensor, Tensor, Tensor, int64_t> _batch_norm_impl_index(
    const Tensor& input, const Tensor& weight /* optional */, const Tensor& bias /* optional */,
    const Tensor& running_mean /* optional */, const Tensor& running_var /* optional */,
    bool training, double momentum, double eps, bool cudnn_enabled) {
  auto num_features = input.sizes()[1];
  if (running_mean.defined()) {
    check_dims_match_num_input_features("running_mean", num_features, running_mean.numel());
  } else if (!training) {
    AT_ERROR("running_mean must be defined in evaluation mode");
  }
  if (running_var.defined()) {
    check_dims_match_num_input_features("running_var", num_features, running_var.numel());
  } else if (!training) {
    AT_ERROR("running_var must be defined in evaluation mode");
  }
  if (weight.defined()) {
    check_dims_match_num_input_features("weight", num_features, weight.numel());
  }
  if (bias.defined()) {
    check_dims_match_num_input_features("bias", num_features, bias.numel());
  }

  bool use_cudnn = false;
  use_cudnn = (input.is_cuda()
               && (input.scalar_type() != at::kHalf
                 || weight.scalar_type() == at::kFloat)
               && weight.defined() && bias.defined()
               && ((running_mean.defined() && running_var.defined())
                 || (!running_mean.defined() && !running_var.defined() && training))
               && input.size(0) <= 131070
               && detail::getCUDAHooks().compiledWithCuDNN()
               && cudnn_enabled && detail::getCUDAHooks().versionCuDNN() >= 5110L);

  if (use_cudnn && eps >= detail::getCUDAHooks().batchnormMinEpsilonCuDNN()) {
    return std::tuple_cat(
             at::cudnn_batch_norm(
               input.contiguous(), weight.contiguous(),
               bias.contiguous(),
               running_mean.defined() ? running_mean.contiguous() : running_mean,
               running_var.defined() ? running_var.contiguous() : running_var,
               training, momentum, eps),
             std::make_tuple(1));
  }

  bool use_miopen = (input.is_cuda()
               && input.dim() <= MIOPEN_DIM_MAX
               && input.scalar_type() != at::kDouble
               && (weight.scalar_type() != at::kHalf)
               && weight.defined() && bias.defined()
               && ((running_mean.defined() && running_var.defined())
                 || (!running_mean.defined() && !running_var.defined() && training))
               && detail::getCUDAHooks().compiledWithMIOpen()
               );

  if (use_miopen) {
    return std::tuple_cat(
             at::miopen_batch_norm(
               input.contiguous(), weight.contiguous(), bias.contiguous(),
               running_mean.defined() ? running_mean.contiguous() : running_mean,
               running_var.defined() ? running_var.contiguous() : running_var,
               training, momentum, eps),
             std::make_tuple(2));
  }

  return std::tuple_cat(
           at::native_batch_norm(
             input, weight, bias, running_mean, running_var, training, momentum, eps),
           std::make_tuple(0));
}

std::tuple<Tensor, Tensor, Tensor> _batch_norm_impl_index_backward(
    int64_t impl_index,
    const Tensor& input, const Tensor& grad_output, const Tensor& weight /* optional */,
    const Tensor& running_mean /* optional */, const Tensor& running_var /* optional */,
    const Tensor& save_mean /* optional */, const Tensor& save_var_transform /* optional */,
    bool train, double epsilon, std::array<bool, 3> output_mask) {
  if (impl_index == 0) {
    return at::native_batch_norm_backward(grad_output, input, weight, running_mean, running_var, save_mean, save_var_transform, train, epsilon, output_mask);
  } else if (impl_index == 1) {
    return at::cudnn_batch_norm_backward(input, grad_output, weight, running_mean, running_var, save_mean, save_var_transform, epsilon);
  } else if (impl_index == 2) {
    return at::miopen_batch_norm_backward(input, grad_output, weight, running_mean, running_var, save_mean, save_var_transform, epsilon);
  }
  AT_ASSERTM(false, "Unsupported impl_index in _batch_norm_impl_index_backward: ", impl_index);
}

Tensor batch_norm(
    const Tensor& input, const Tensor& weight /* optional */, const Tensor& bias /* optional */,
    const Tensor& running_mean /* optional */, const Tensor& running_var /* optional */,
    bool training, double momentum, double eps, bool cudnn_enabled) {
  return std::get<0>(at::_batch_norm_impl_index(input, weight, bias, running_mean, running_var,
                                                training, momentum, eps, cudnn_enabled));
}

Tensor instance_norm(
    const Tensor& input, const Tensor& weight /* optional */, const Tensor& bias /* optional */,
    const Tensor& running_mean /* optional */, const Tensor& running_var /* optional */,
    bool use_input_stats, double momentum, double eps, bool cudnn_enabled) {
  AT_CHECK(use_input_stats || (running_mean.defined() && running_var.defined()),
           "Expected running_mean and running_var to be defined when use_input_stats is false");
  std::vector<int64_t> shape = input.sizes().vec();
  int64_t b = input.size(0);
  int64_t c = input.size(1);
  shape[1] = b * c;
  shape[0] = 1;

  Tensor weight_ = repeat_if_defined(weight, b);
  Tensor bias_ = repeat_if_defined(bias, b);
  Tensor running_mean_ = repeat_if_defined(running_mean, b);
  Tensor running_var_ = repeat_if_defined(running_var, b);

  auto input_reshaped = input.contiguous().view(shape);
  auto out = at::batch_norm(input_reshaped, weight_, bias_, running_mean_, running_var_,
                            use_input_stats, momentum, eps, cudnn_enabled);

  // we alias running_mean and running_var because they are const but we want to modify their data
  if (running_mean.defined()) {
    at::alias(running_mean).copy_(running_mean_.view({ b, c }).mean(0, false));
  }
  if (running_var.defined()) {
    at::alias(running_var).copy_(running_var_.view({ b, c }).mean(0, false));
  }

  return out.view(input.sizes());
}

Tensor layer_norm(const Tensor& input, IntArrayRef normalized_shape,
    const Tensor& weight /* optional */, const Tensor& bias /* optional */,
    double eps, bool cudnn_enabled) {

    int64_t normalized_ndim = normalized_shape.size();

    AT_CHECK(normalized_ndim >= 1,
             "Expected normalized_shape to be at least 1-dimensional, i.e., ",
             "containing at least one element, but got normalized_shape=",
             normalized_shape);

    AT_CHECK(!weight.defined() || weight.sizes().equals(normalized_shape),
             "Expected weight to be of same shape as normalized_shape, but got ",
             "weight of shape ", weight.sizes(), " and normalized_shape=",
             normalized_shape);
    AT_CHECK(!bias.defined() || bias.sizes().equals(normalized_shape),
             "Expected bias to be of same shape as normalized_shape, but got ",
             "bias of shape ", bias.sizes(), " and normalized_shape=",
             normalized_shape);

    auto input_shape = input.sizes();
    auto input_ndim = input.dim();

    if (input_ndim < normalized_ndim ||
        !input_shape.slice(input_ndim - normalized_ndim).equals(normalized_shape)) {
      std::stringstream ss;
      ss << "Given normalized_shape=" << normalized_shape
         << ", expected input with shape [*";
      for (auto size : normalized_shape) {
        ss << ", " << size;
      }
      ss << "], but got input of size" << input_shape;
      AT_ERROR(ss.str());
    }

    int64_t n = 1;
    for (int64_t i = 0; i < input_ndim - normalized_ndim; i++) {
      n *= input_shape[i];
    }

    // Apply layer norm
    auto input_reshaped = input.contiguous().view({1, n, -1});

    auto out = at::batch_norm(input_reshaped, {}, {}, {}, {}, true, 0, eps,
                              cudnn_enabled);
    out = out.view(input_shape);

    if (weight.defined() && bias.defined()) {
      return bias.addcmul(out, weight, 1);
    } else if (weight.defined()) {
      return out.mul(weight);
    } else if (bias.defined()) {
      return out.add(bias);
    } else {
      return out;
    }
}

Tensor group_norm(const Tensor& input, int64_t num_groups,
    const Tensor& weight /* optional */, const Tensor& bias /* optional */,
    double eps, bool cudnn_enabled) {

    auto input_shape = input.sizes();
    int64_t b = input.size(0);
    int64_t c = input.size(1);

    AT_CHECK(c % num_groups == 0,
             "Expected number of channels in input to be divisible by ",
             "num_groups, but got input of shape ", input.sizes(), " and "
             "num_groups=", num_groups);

    AT_CHECK(!weight.defined() || (weight.dim() == 1 && weight.numel() == c),
             "Expected weight to be a vector of size equal to the number of ",
             "channels in input, but got weight of shape ", weight.sizes(),
             " and input of shape ", input.sizes());
    AT_CHECK(!bias.defined() || (bias.dim() == 1 && bias.numel() == c),
             "Expected bias to be a vector of size equal to the number of ",
             "channels in input, but got bias of shape ", weight.sizes(),
             " and input of shape ", input.sizes());

    // Apply group norm
    auto input_reshaped = input.contiguous().view({1, b * num_groups, -1});

    auto out = at::batch_norm(input_reshaped, {}, {}, {}, {}, true, 0, eps,
                              cudnn_enabled);
    out = out.view(input_shape);

    if (!weight.defined() && !bias.defined()) {
      return out;
    }

    std::vector<int64_t> affine_param_shape(input.dim(), 1);
    affine_param_shape[1] = c;

    if (weight.defined() && bias.defined()) {
      return bias.view(affine_param_shape).addcmul(out, weight.view(affine_param_shape), 1);
    } else if (weight.defined()) {
      return out.mul(weight.view(affine_param_shape));
    } else {
      return out.add(bias.view(affine_param_shape));
    }
}

std::tuple<Tensor, Tensor> batch_norm_update_stats_cpu(
        const Tensor& self, const Tensor& running_mean, const Tensor& running_var, double momentum) {
  return AT_DISPATCH_FLOATING_TYPES(self.scalar_type(), "batch_norm_update_stats_cpu", [&] {
      return batch_norm_cpu_update_stats_template<scalar_t, Var>(self, running_mean, running_var, momentum, 0);
    });
}

std::tuple<Tensor, Tensor, Tensor> batch_norm_cpu(const Tensor& self, const Tensor& weight, const Tensor& bias,
                                                  const Tensor& running_mean, const Tensor& running_var,
                                                  bool train, double momentum, double eps) {
  checkBackend("batch_norm_cpu", {self, weight, bias, running_mean, running_var}, Backend::CPU);

  return AT_DISPATCH_FLOATING_TYPES(self.scalar_type(), "batch_norm", [&] {
      if (!train) {
        return batch_norm_cpu_transform_input_template<scalar_t>(self, weight, bias, {}, {}, running_mean, running_var, train, eps);
      } else {
        auto save_stats = batch_norm_cpu_update_stats_template<scalar_t, InvStd>(self, running_mean, running_var, momentum, eps);
        return batch_norm_cpu_transform_input_template<scalar_t>(self, weight, bias, std::get<0>(save_stats), std::get<1>(save_stats), running_mean, running_var, train, eps);
      }
    });
}

std::tuple<Tensor, Tensor, Tensor> batch_norm_backward_cpu(const Tensor& grad_out, const Tensor& self, const Tensor& weight,
                                                           const Tensor& running_mean, const Tensor& running_var, const Tensor& save_mean, const Tensor& save_invstd,
                                                           bool train, double eps, std::array<bool,3> grad_input_mask) {
  return AT_DISPATCH_FLOATING_TYPES(self.scalar_type(), "batch_norm_backward_cpu", [&] {
      return batch_norm_backward_cpu_template<scalar_t>(grad_out, self, weight, running_mean, running_var, save_mean, save_invstd, train, eps, grad_input_mask);
    });
}

}} // at::native
