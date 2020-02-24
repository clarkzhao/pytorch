#include <torch/nn/modules/rnn.h>

#include <torch/nn/init.h>
#include <torch/types.h>
#include <torch/utils.h>

#include <c10/util/Exception.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <regex>
#include <string>
#include <tuple>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace torch::nn::utils::rnn;

namespace torch {
namespace nn {

/// The function signature of `rnn_relu`, `rnn_tanh` and `gru`.
using RNNFunctionSignature = std::tuple<Tensor, Tensor>(
    /*input=*/const Tensor&,
    /*state=*/const Tensor&,
    /*params=*/TensorList,
    /*has_biases=*/bool,
    /*layers=*/int64_t,
    /*dropout=*/double,
    /*train=*/bool,
    /*bidirectional=*/bool,
    /*batch_first=*/bool);

std::unordered_map<std::string, RNNFunctionSignature*> _rnn_impls = {
  {"RNN_TANH", &torch::rnn_tanh},
  {"RNN_RELU", &torch::rnn_relu}
};

/// These must line up with the CUDNN mode codes:
/// https://docs.nvidia.com/deeplearning/sdk/cudnn-developer-guide/index.html#cudnnRNNMode_t
enum class CuDNNMode { RNN_RELU = 0, RNN_TANH = 1, LSTM = 2, GRU = 3 };

CuDNNMode get_cudnn_mode_for_rnn(detail::RNNOptionsBase::rnn_options_base_mode_t mode) {
  if (c10::get_if<enumtype::kRNN_RELU>(&mode)) {
    return CuDNNMode::RNN_RELU;
  } else if (c10::get_if<enumtype::kRNN_TANH>(&mode)) {
    return CuDNNMode::RNN_TANH;
  } else if (c10::get_if<enumtype::kLSTM>(&mode)) {
    return CuDNNMode::LSTM;
  } else if (c10::get_if<enumtype::kGRU>(&mode)) {
    return CuDNNMode::GRU;
  } else {
    TORCH_CHECK(false, "Unknown mode: ", torch::enumtype::get_enum_name(mode));
  }
}

RNNFunctionSignature* get_rnn_impl(detail::RNNOptionsBase::rnn_options_base_mode_t mode) {
  if (c10::get_if<enumtype::kRNN_TANH>(&mode)) {
    return _rnn_impls["RNN_TANH"];
  } else if (c10::get_if<enumtype::kRNN_RELU>(&mode)) {
    return _rnn_impls["RNN_RELU"];
  } else {
    TORCH_CHECK(false, "Unknown mode: ", torch::enumtype::get_enum_name(mode));
  }
}

Tensor apply_permutation(Tensor tensor, Tensor permutation, int64_t dim = 1) {
  return tensor.index_select(dim, permutation);
}

// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ RNNImplBase ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
namespace detail {
template <typename Derived>
RNNImplBase<Derived>::RNNImplBase(RNNOptionsBase options_)
  : options(std::move(options_)) {
  reset();
}

template <typename Derived>
void RNNImplBase<Derived>::reset() {
  const int64_t num_directions = options.bidirectional() ? 2 : 1;

  TORCH_CHECK(
    0 <= options.dropout() && options.dropout() <= 1,
    "dropout should be a number in range [0, 1] ",
    "representing the probability of an element being ",
    "zeroed");

  if (options.dropout() > 0 && options.num_layers() == 1) {
    TORCH_WARN(
      "dropout option adds dropout after all but last ",
      "recurrent layer, so non-zero dropout expects ",
      "num_layers greater than 1, but got dropout=", options.dropout(), " and ",
      "num_layers=", options.num_layers());
  }

  int64_t gate_size = 0;
  if (c10::get_if<enumtype::kLSTM>(&options.mode())) {
    gate_size = 4 * options.hidden_size();
  } else if (c10::get_if<enumtype::kGRU>(&options.mode())) {
    gate_size = 3 * options.hidden_size();
  } else if (c10::get_if<enumtype::kRNN_TANH>(&options.mode())) {
    gate_size = options.hidden_size();
  } else if (c10::get_if<enumtype::kRNN_RELU>(&options.mode())) {
    gate_size = options.hidden_size();
  } else {
    TORCH_CHECK(false, "Unrecognized RNN mode: " + torch::enumtype::get_enum_name(options.mode()));
  }

  _flat_weights_names = {};
  _all_weights = {};

  for (int64_t layer = 0; layer < options.num_layers(); layer++) {
    for (int64_t direction = 0; direction < num_directions; direction++) {
      int64_t layer_input_size = layer == 0 ? options.input_size() : options.hidden_size() * num_directions;

      auto w_ih = torch::empty({gate_size, layer_input_size});
      auto w_hh = torch::empty({gate_size, options.hidden_size()});
      auto b_ih = torch::empty({gate_size});
      // Second bias vector included for CuDNN compatibility. Only one
      // bias vector is needed in standard definition.
      auto b_hh = torch::empty({gate_size});
      std::vector<Tensor> layer_params = {w_ih, w_hh, b_ih, b_hh};

      std::string suffix = direction == 1 ? "_reverse" : "";
      std::vector<std::string> param_names = {"weight_ih_l{layer}{suffix}", "weight_hh_l{layer}{suffix}"};
      if (options.bias()) {
        param_names.emplace_back("bias_ih_l{layer}{suffix}");
        param_names.emplace_back("bias_hh_l{layer}{suffix}");
      }
      for (size_t i = 0; i < param_names.size(); i++) {
        std::string x = std::regex_replace(param_names[i], std::regex("\\{layer}"), layer);
        x = std::regex_replace(x, std::regex("\\{suffix}"), suffix);
        param_names[i] = x;
      }

      for (size_t i = 0; i < param_names.size(); i++) {
        auto name = param_names[i];
        auto param = layer_params[i];
        this->register_parameter(name, param);
      }
      _flat_weights_names.insert(_flat_weights_names.end(), param_names.begin(), param_names.end());
      _all_weights.emplace_back(param_names);
    }
  }

  _flat_weights = {};
  for (const auto& wn : _flat_weights_names) {
    auto named_parameters = this->named_parameters(/*recurse=*/false);
    if (named_parameters.contains(wn)) {
      _flat_weights.emplace_back(named_parameters[wn]);
    } else {
      _flat_weights.emplace_back(Tensor());
    }
  }

  this->flatten_parameters();
  this->reset_parameters(); 
}

template <typename Derived>
void RNNImplBase<Derived>::flatten_parameters() {
  // Resets parameter data pointer so that they can use faster code paths.
  //
  // Right now, this works only if the module is on the GPU and cuDNN is enabled.
  // Otherwise, it's a no-op.

  // Short-circuits if _flat_weights is only partially instantiated
  if (_flat_weights.size() != _flat_weights_names.size()) {
    return;
  }

  // Short-circuits if any tensor in self._flat_weights is not acceptable to cuDNN
  // or the tensors in _flat_weights are of different dtypes

  auto first_fw = _flat_weights[0];
  auto dtype = first_fw.dtype();
  for (const auto& fw : _flat_weights) {
    if (!(fw.dtype() == dtype) ||
        !fw.is_cuda() ||
        !torch::cudnn_is_acceptable(fw)) {
      return;
    }
  }

  // If any parameters alias, we fall back to the slower, copying code path. This is
  // a sufficient check, because overlapping parameter buffers that don't completely
  // alias would break the assumptions of the uniqueness check in
  // Module::named_parameters().
  std::unordered_set<void*> unique_data_ptrs;
  for (const auto& p : _flat_weights) {
    unique_data_ptrs.emplace(p.data_ptr());
  }
  if (unique_data_ptrs.size() != _flat_weights.size()) {
    return;
  }

  {
    torch::DeviceGuard device_guard(torch::device_of(first_fw));

    // Note: no_grad() is necessary since _cudnn_rnn_flatten_weight is
    // an inplace operation on self._flat_weights
    {
      torch::NoGradGuard no_grad;
      torch::_cudnn_rnn_flatten_weight(
            _flat_weights,
            options.with_bias() ? 4 : 2,
            options.input_size(),
            get_cudnn_mode_for_rnn(options.mode()),
            options.hidden_size(),
            options.num_layers(),
            options.batch_first(), 
            options.bidirectional());
    }
  }
}

template <typename Derived>
void RNNImplBase<Derived>::reset_flat_weights() {
  _flat_weights = {};
  for (const auto& wn : _flat_weights_names) {
    auto named_parameters = this->named_parameters(/*recurse=*/false);
    if (named_parameters.contains(wn)) {
      _flat_weights.emplace_back(named_parameters[wn]);
    } else {
      _flat_weights.emplace_back(Tensor());
    }
  }
}

template <typename Derived>
void RNNImplBase<Derived>::to(
    torch::Device device,
    torch::Dtype dtype,
    bool non_blocking) {
  nn::Module::to(device, dtype, non_blocking);
  reset_flat_weights();
  flatten_parameters();
}

template <typename Derived>
void RNNImplBase<Derived>::to(torch::Dtype dtype, bool non_blocking) {
  nn::Module::to(dtype, non_blocking);
  reset_flat_weights();
  flatten_parameters();
}

template <typename Derived>
void RNNImplBase<Derived>::to(torch::Device device, bool non_blocking) {
  nn::Module::to(device, non_blocking);
  reset_flat_weights();
  flatten_parameters();
}

template <typename Derived>
void RNNImplBase<Derived>::reset_parameters() {
  const double stdv = 1.0 / std::sqrt(options.hidden_size());
  for (auto& weight : this->parameters()) {
    init::uniform_(weight, -stdv, stdv);
  }
}

template <typename Derived>
void RNNImplBase<Derived>::check_input(Tensor input, Tensor batch_sizes) {
  int64_t expected_input_dim = batch_sizes.defined() ?  2 : 3;
  TORCH_CHECK(
    input.dim() == expected_input_dim,
    "input must have ", expected_input_dim, " dimensions, got ", input.dim());
  TORCH_CHECK(
    options.input_size() == input.size(-1),
    "input.size(-1) must be equal to input_size. Expected ", options.input_size(), " got ", input.size(-1));
}

template <typename Derived>
std::tuple<int64_t, int64_t, int64_t> RNNImplBase<Derived>::get_expected_hidden_size(
  Tensor input, Tensor batch_sizes) {
  int64_t mini_batch = 0;
  if (batch_sizes.defined()) {
    mini_batch = batch_sizes[0].item<int64_t>();
  } else {
    mini_batch = options.batch_first() ? input.size(0) : input.size(1);
  }    
  int64_t num_directions = options.bidirectional() ? 2 : 1;
  return std::make_tuple(options.num_layers() * num_directions, mini_batch, options.hidden_size());
}

template <typename Derived>
void RNNImplBase<Derived>::check_hidden_size(
    Tensor hx,
    std::tuple<int64_t, int64_t, int64_t> expected_hidden_size,
    std::string msg) {
  if (hx.size() != expected_hidden_size) {
    msg = std::regex_replace(msg, std::regex("\\{1}"), expected_hidden_size);
    msg = std::regex_replace(msg, std::regex("\\{2}"), hx.size());
    TORCH_CHECK(false, msg);
  }
}

template <typename Derived>
void RNNImplBase<Derived>::check_forward_args(Tensor input, Tensor hidden, Tensor batch_sizes) {
  this->check_input(input, batch_sizes)
  auto expected_hidden_size = this->get_expected_hidden_size(input, batch_sizes);

  this->check_hidden_size(hidden, expected_hidden_size);
}

template <typename Derived>
Tensor RNNImplBase<Derived>::permute_hidden(Tensor hx, Tensor permutation) {
  if (!permutation.defined()) {
    return hx;
  }
  return apply_permutation(hx, permutation);
}

template <typename Derived>
std::tuple<Tensor, Tensor> RNNImplBase<Derived>::forward_helper(
  Tensor input,
  Tensor batch_sizes,
  Tensor sorted_indices,
  int64_t max_batch_size,
  Tensor hx) {
  if (!hx.defined()) {
    int64_t num_directions = options.bidirectional() 2 : 1;
    hx = torch::zeros({options.num_layers() * num_directions,
                     max_batch_size, options.hidden_size()},
                     torch::dtype(input.dtype()).device(input.device()));
  } else {
    // Each batch of the hidden state should match the input sequence that
    // the user believes he/she is passing in.
    hx = this->permute_hidden(hx, sorted_indices);
  }    

  this->check_forward_args(input, hx, batch_sizes);
  std::function<RNNFunctionSignature> _impl = get_rnn_impl(options.mode());
  std::tuple<Tensor, Tensor> result;
  if (!batch_sizes.defined()) {
    result = _impl(input, hx, _flat_weights, options.bias(), options.num_layers(),
                     options.dropout(), is_training(), options.bidirectional(), options.batch_first());
  } else {
    result = _impl(input, batch_sizes, hx, _flat_weights, options.bias(),
                     options.num_layers(), options.dropout(), is_training(), options.bidirectional());
  }
  auto output = std::get<0>(result);
  auto hidden = std::get<1>(result);
}

template <typename Derived>
std::tuple<Tensor, Tensor> RNNImplBase<Derived>::forward(const Tensor& input, Tensor hx = {}) {
  auto batch_sizes = torch::Tensor();
  auto max_batch_size = options.batch_first() input.size(0) : input.size(1);
  auto sorted_indices = torch::Tensor();
  auto unsorted_indices = torch::Tensor();

  Tensor output, hidden;
  std::tie(output, hidden) = this->forward_helper(input, batch_sizes, sorted_indices, max_batch_size, hx);

  return std::make_tuple(output, this->permute_hidden(hidden, unsorted_indices));
}

template <typename Derived>
std::tuple<PackedSequence, Tensor> RNNImplBase<Derived>::forward(const PackedSequence& packed_input, Tensor hx = {}) {
  auto input = packed_input.data();
  auto batch_sizes = packed_input.batch_sizes();
  auto sorted_indices = packed_input.sorted_indices();
  auto unsorted_indices = packed_input.unsorted_indices();
  auto max_batch_size = batch_sizes[0].item<int64_t>();

  Tensor output, hidden;
  std::tie(output, hidden) = this->forward_helper(input, batch_sizes, sorted_indices, max_batch_size, hx);

  auto output_packed = PackedSequence(output, batch_sizes, sorted_indices, unsorted_indices);
  return std::make_tuple(output_packed, this->permute_hidden(hidden, unsorted_indices));
}

template <typename Derived>
void RNNImplBase<Derived>::pretty_print(std::ostream& stream) const {
  const std::string name = this->name();
  const std::string name_without_impl = name.substr(0, name.size() - 4);
  stream << name_without_impl << "(input_size=" << options.input_size()
         << ", hidden_size=" << options.hidden_size()
         << ", num_layers=" << options.num_layers()
         << ", bias=" << options.bias()
         << ", batch_first=" << options.batch_first()
         << ", dropout=" << options.dropout()
         << ", bidirectional=" << options.bidirectional()
         << ")";
}

template <typename Derived>
std::vector<Tensor> RNNImplBase<Derived>::all_weights() const {
  std::vector<Tensor> result = {};
  auto named_parameters = this->named_parameters(/*recurse=*/false);
  for (const auto& weights : _all_weights) {
    for (const auto& weight : weights) {
      result.emplace_back(named_parameters[weight]);
    }
  }
  return result;
}

template class RNNImplBase<LSTMImpl>;
template class RNNImplBase<GRUImpl>;
template class RNNImplBase<RNNImpl>;
} // namespace detail

// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ RNN ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

detail::RNNOptionsBase::rnn_options_base_mode_t compute_rnn_options_base_mode(
  RNNOptions::nonlinearity_t nonlinearity) {
  if (c10::get_if<enumtype::kTanh>(nonlinearity)) {
    return torch::kRNN_TANH;
  } else if (c10::get_if<enumtype::kReLU>(nonlinearity)) {
    return torch::kRNN_RELU;
  } else {
    TORCH_CHECK(false, "Unknown nonlinearity ", torch::enumtype::get_enum_name(nonlinearity));
  }
}

RNNImpl::RNNImpl(RNNOptions options_)
    : detail::RNNImplBase<RNNImpl>(
          detail::RNNOptionsBase(
            compute_rnn_options_base_mode(options.nonlinearity()),
            options_.input_size(),
            options_.hidden_size())
              .num_layers(options_.num_layers())
              .bias(options_.bias())
              .batch_first(options_.batch_first())
              .dropout(options_.dropout())
              .bidirectional(options_.bidirectional())),
      options(options_) {}
}

// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ LSTM ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

LSTMImpl::LSTMImpl(LSTMOptions options_)
    : detail::RNNImplBase<LSTMImpl>(
          detail::RNNOptionsBase(
            torch::kLSTM,
            options_.input_size(),
            options_.hidden_size())
              .num_layers(options_.num_layers())
              .bias(options_.bias())
              .batch_first(options_.batch_first())
              .dropout(options_.dropout())
              .bidirectional(options_.bidirectional())),
      options(options_) {}

void LSTMImpl::check_forward_args(Tensor input, std::tuple<Tensor, Tensor> hidden, Tensor batch_sizes) {
  this->check_input(input, batch_sizes);
  auto expected_hidden_size = this->get_expected_hidden_size(input, batch_sizes);

  this->check_hidden_size(std::get<0>(hidden), expected_hidden_size,
                          "Expected hidden[0] size {1}, got {2}");
  this->check_hidden_size(std::get<1>(hidden), expected_hidden_size,
                          "Expected hidden[1] size {1}, got {2}");
}

std::tuple<Tensor, Tensor> LSTMImpl::permute_hidden(std::tuple<Tensor, Tensor> hx, Tensor permutation) {
  if (!permutation.defined()) {
    return hx;
  }
  return std::make_tuple(
    apply_permutation(std::get<0>(hx), permutation),
    apply_permutation(std::get<1>(hx), permutation)
  );
}

std::tuple<Tensor, std::tuple<Tensor, Tensor>> LSTMImpl::forward_helper(
  Tensor input,
  Tensor batch_sizes,
  Tensor sorted_indices,
  int64_t max_batch_size,
  torch::optional<std::tuple<Tensor, Tensor>> hx_opt) {

  std::tuple<Tensor, Tensor> hx;
  if (!hx_opt.has_value()) {
    int64_t num_directions = options.bidirectional() 2 : 1;
    auto zeros = torch::zeros({options.num_layers() * num_directions,
                     max_batch_size, options.hidden_size()},
                     torch::dtype(input.dtype()).device(input.device()));
    hx = std::make_tuple(zeros, zeros);
  } else {
    hx = hx_opt.value();
    // Each batch of the hidden state should match the input sequence that
    // the user believes he/she is passing in.
    hx = this->permute_hidden(hx, sorted_indices);
  }    

  this->check_forward_args(input, hx, batch_sizes);
  std::tuple<Tensor, Tensor, Tensor> result;
  if (!batch_sizes.defined()) {
    result = torch::lstm(input, hx, _flat_weights, options.bias(), options.num_layers(),
                     options.dropout(), is_training(), options.bidirectional(), options.batch_first());
  } else {
    result = torch::lstm(input, batch_sizes, hx, _flat_weights, options.bias(),
                     options.num_layers(), options.dropout(), is_training(), options.bidirectional());
  }
  auto output = std::get<0>(result);
  auto hidden = std::make_tuple(std::get<1>(result), std::get<2>(result));

  return std::make_tuple(output, hidden);
}

std::tuple<Tensor, std::tuple<Tensor, Tensor>> LSTMImpl::forward(
  const Tensor& input, torch::optional<std::tuple<Tensor, Tensor>> hx_opt) {
  auto batch_sizes = torch::Tensor();
  auto max_batch_size = options.batch_first() ? input.size(0) : input.size(1);
  auto sorted_indices = torch::Tensor();
  auto unsorted_indices = torch::Tensor();

  Tensor output;
  std::tuple<Tensor, Tensor> hidden;
  std::tie(output, hidden) = this->forward_helper(input, batch_sizes, sorted_indices, max_batch_size, hx_opt);

  return std::make_tuple(output, this->permute_hidden(hidden, unsorted_indices));
}

std::tuple<PackedSequence, std::tuple<Tensor, Tensor>> LSTMImpl::forward(
  const PackedSequence& packed_input, torch::optional<std::tuple<Tensor, Tensor>> hx_opt) {
  auto input = packed_input.data();
  auto batch_sizes = packed_input.batch_sizes();
  auto sorted_indices = packed_input.sorted_indices();
  auto unsorted_indices = packed_input.unsorted_indices();
  auto max_batch_size = batch_sizes[0].item<int64_t>();

  Tensor output;
  std::tuple<Tensor, Tensor> hidden;
  std::tie(output, hidden) = this->forward_helper(input, batch_sizes, sorted_indices, max_batch_size, hx_opt);

  auto output_packed = PackedSequence(output, batch_sizes, sorted_indices, unsorted_indices);
  return std::make_tuple(output_packed, this->permute_hidden(hidden, unsorted_indices));
}

// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ GRU ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

GRUImpl::GRUImpl(GRUOptions options_)
    : detail::RNNImplBase<GRUImpl>(
          detail::RNNOptionsBase(
            torch::kGRU,
            options_.input_size(),
            options_.hidden_size())
              .num_layers(options_.num_layers())
              .bias(options_.bias())
              .batch_first(options_.batch_first())
              .dropout(options_.dropout())
              .bidirectional(options_.bidirectional())),
      options(options_) {}

std::tuple<Tensor, Tensor> GRUImpl::forward_helper(
  Tensor input,
  Tensor batch_sizes,
  Tensor sorted_indices,
  int64_t max_batch_size,
  Tensor hx) {
  if (!hx.defined()) {
    int64_t num_directions = options.bidirectional() 2 : 1;
    hx = torch::zeros({options.num_layers() * num_directions,
                     max_batch_size, options.hidden_size()},
                     torch::dtype(input.dtype()).device(input.device()));
  } else {
    // Each batch of the hidden state should match the input sequence that
    // the user believes he/she is passing in.
    hx = this->permute_hidden(hx, sorted_indices);
  }    

  this->check_forward_args(input, hx, batch_sizes);
  if (!batch_sizes.defined()) {
    result = torch::gru(input, hx, _flat_weights, options.bias(), options.num_layers(),
                     options.dropout(), is_training(), options.bidirectional(), options.batch_first());
  } else {
    result = torch::gru(input, batch_sizes, hx, _flat_weights, options.bias(),
                     options.num_layers(), options.dropout(), is_training(), options.bidirectional());
  }
  auto output = std::get<0>(result);
  auto hidden = std::get<1>(result);

  return std::make_tuple(output, hidden);
}

std::tuple<Tensor, Tensor> GRUImpl::forward(const Tensor& input, Tensor hx) {
  auto batch_sizes = torch::Tensor();
  auto max_batch_size = options.batch_first() ? input.size(0) : input.size(1);
  auto sorted_indices = torch::Tensor();
  auto unsorted_indices = torch::Tensor();

  Tensor output, hidden;
  std::tie(output, hidden) = this->forward_helper(input, batch_sizes, sorted_indices, max_batch_size, hx);

  return std::make_tuple(output, this->permute_hidden(hidden, unsorted_indices));
}

std::tuple<PackedSequence, Tensor> forward(const PackedSequence& packed_input, Tensor hx) {
  auto input = packed_input.data();
  auto batch_sizes = packed_input.batch_sizes();
  auto sorted_indices = packed_input.sorted_indices();
  auto unsorted_indices = packed_input.unsorted_indices();
  auto max_batch_size = batch_sizes[0].item<int64_t>();

  Tensor output, hidden;
  std::tie(output, hidden) = this->forward_helper(input, batch_sizes, sorted_indices, max_batch_size, hx);

  auto output_packed = PackedSequence(output, batch_sizes, sorted_indices, unsorted_indices);
  return std::make_tuple(output_packed, this->permute_hidden(hidden, unsorted_indices));
}

} // namespace nn
} // namespace torch
