#include <ATen/native/autotune/dispatch/core.h>

#include <chrono>
#include <map>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

#include <ATen/Context.h>
#include <ATen/native/autotune/api.h>
#include <ATen/native/autotune/bandits/common.h>
#include <ATen/native/autotune/bandits/gaussian.h>
#include <ATen/native/autotune/bandits/random.h>
#include <ATen/native/autotune/utils/logging.h>
#include <c10/util/Exception.h>
#include <c10/util/flat_hash_map.h>

namespace autotune {
namespace selection {

template <typename T>
class ActiveBandits {
 public:
  static_assert(std::is_base_of<bandits::Bandit, T>::value);
  static ActiveBandits<T>& singleton() {
    static ActiveBandits<T> _singleton;
    return _singleton;
  }

  std::unique_ptr<T>& get(
      KernelEntryPoint::MapKey key,
      std::function<KernelEntryPoint::cost_estimates()> cost_fn) {
    if (bandits_.find(key) == bandits_.end()) {
      ordered_keys_.push_back(key); // Preserve order for debugging.
      auto cost_estimates = cost_fn();
      bandits_[key] = std::make_unique<T>(cost_estimates, next_seed_);
      next_seed_++;
    }

    return bandits_.at(key);
  }

  std::unique_ptr<T>& get(KernelEntryPoint::MapKey key) {
    return bandits_.at(key);
  }

  void summarize() {
    for (auto& k : ordered_keys_) {
      bandits_[k]->summarize(k);
    }
  }

  void reset() {
    next_seed_ = 0;
    ordered_keys_.clear();
    bandits_.clear();
  }

 private:
  ActiveBandits(){};
  size_t next_seed_{0};
  std::vector<KernelEntryPoint::MapKey> ordered_keys_;
  ska::flat_hash_map<
      KernelEntryPoint::MapKey,
      std::unique_ptr<T>,
      KernelEntryPoint::Hash>
      bandits_;
};

const auto& DrunkenBandits = ActiveBandits<bandits::DrunkenBandit>::singleton;
const auto& GaussianBandits = ActiveBandits<bandits::GaussianBandit>::singleton;

api::AvailableBandits DispatchInterface::active_bandit() {
  return active_bandit_;
}

void DispatchInterface::set_active_bandit(api::AvailableBandits b) {
  active_bandit_ = b;
}

api::Implementation DispatchInterface::choose(
    api::AvailableBandits bandit,
    KernelEntryPoint::MapKey key,
    std::function<KernelEntryPoint::cost_estimates()> cost_estimates) {
  api::Implementation choice;
  switch (bandit) {
    case api::AvailableBandits::kRandomChoice:
      choice = DrunkenBandits().get(key, cost_estimates)->choose();
      break;
    case api::AvailableBandits::kGaussian:
      choice = GaussianBandits().get(key, cost_estimates)->choose();
      break;
    default:
      TORCH_INTERNAL_ASSERT(false, "Could not select bandit.")
  }
  TORCH_INTERNAL_ASSERT(choice != api::Implementation::TOTAL_COUNT);
  chosen_counts_[static_cast<size_t>(choice)]++;
  return choice;
}

size_t DispatchInterface::times_chosen(api::Implementation choice) {
  TORCH_INTERNAL_ASSERT(choice != api::Implementation::TOTAL_COUNT);
  return chosen_counts_[static_cast<size_t>(choice)];
}

void DispatchInterface::update(
    api::AvailableBandits bandit,
    KernelEntryPoint::MapKey key,
    api::Implementation choice,
    size_t delta_ns) {
  switch (bandit) {
    case api::AvailableBandits::kRandomChoice:
      DrunkenBandits().get(key)->update(choice, delta_ns);
      break;
    case api::AvailableBandits::kGaussian:
      GaussianBandits().get(key)->update(choice, delta_ns);
      break;
    default:
      TORCH_INTERNAL_ASSERT(false, "Could not select bandit.")
  }
}

void DispatchInterface::summarize() {
  auto bandit = DispatchInterface::singleton().active_bandit();
  switch (bandit) {
    case api::AvailableBandits::kRandomChoice:
      DrunkenBandits().summarize();
      break;
    case api::AvailableBandits::kGaussian:
      GaussianBandits().summarize();
      break;
    default:
      TORCH_INTERNAL_ASSERT(false, "Could not select bandit.")
  }
}

void DispatchInterface::reset() {
  DrunkenBandits().reset();
  GaussianBandits().reset();
  active_bandit_ = api::AvailableBandits::kNone;
  for (size_t i = 0; i < chosen_counts_.size(); i++) {
    chosen_counts_[i] = 0;
  }
}

template <typename T>
SelectImplementation<T>::SelectImplementation(typename T::Args args)
    : entry_point_(args) {
  bandit_type_ = DispatchInterface::singleton().active_bandit();
  if (bandit_type_ == api::AvailableBandits::kNone) {
    choice_ = api::Implementation::kDisabled;
  } else if (entry_point_.fallback()) {
    choice_ = api::Implementation::kFallback;
  } else {
    auto available_implementations = entry_point_.implementations();
    TORCH_INTERNAL_ASSERT(
        entry_point_.implementations().size(),
        "Autotuning is enabled and kernel did not request a fallback, "
        "however no implemenations are available.");

    choice_ = DispatchInterface::singleton().choose(
        bandit_type_, entry_point_.key(), [&]() {
          return entry_point_.costs();
        });

    record_duration_ = true;
    start_ = std::chrono::high_resolution_clock::now();
  }
}

template <typename T>
api::Implementation SelectImplementation<T>::choice() {
  return choice_;
}

template <typename T>
void SelectImplementation<T>::finish() {
  if (!record_duration_)
    return;
  auto end = std::chrono::high_resolution_clock::now();
  auto delta_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(end - start_)
          .count();
  TORCH_INTERNAL_ASSERT(!record_finished_, "finish() called twice.");
  record_finished_ = true;
  auto key = entry_point_.key();
  DispatchInterface::singleton().update(bandit_type_, key, choice_, delta_ns);
  logging::register_key(key, [&]() { return entry_point_.repr(); });
  logging::record(bandit_type_, key, choice_, delta_ns);
}

} // namespace selection
} // namespace autotune
