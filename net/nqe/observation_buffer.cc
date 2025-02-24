// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/nqe/observation_buffer.h"

#include <float.h>

#include <algorithm>
#include <utility>

#include "base/macros.h"
#include "base/time/default_tick_clock.h"
#include "base/time/time.h"
#include "net/nqe/network_quality_estimator_params.h"
#include "net/nqe/weighted_observation.h"

namespace net {

namespace nqe {

namespace internal {

ObservationBuffer::ObservationBuffer(
    const NetworkQualityEstimatorParams* params,
    base::TickClock* tick_clock,
    double weight_multiplier_per_second,
    double weight_multiplier_per_signal_level)
    : params_(params),
      weight_multiplier_per_second_(weight_multiplier_per_second),
      weight_multiplier_per_signal_level_(weight_multiplier_per_signal_level),
      tick_clock_(tick_clock) {
  DCHECK_LT(0u, params_->observation_buffer_size());
  DCHECK_LE(0.0, weight_multiplier_per_second_);
  DCHECK_GE(1.0, weight_multiplier_per_second_);
  DCHECK_LE(0.0, weight_multiplier_per_signal_level_);
  DCHECK_GE(1.0, weight_multiplier_per_signal_level_);
  DCHECK(params_);
  DCHECK(tick_clock_);
}

ObservationBuffer::~ObservationBuffer() {}

void ObservationBuffer::AddObservation(const Observation& observation) {
  DCHECK_LE(observations_.size(), params_->observation_buffer_size());

  // Observations must be in the non-decreasing order of the timestamps.
  DCHECK(observations_.empty() ||
         observation.timestamp() >= observations_.back().timestamp());

  // Evict the oldest element if the buffer is already full.
  if (observations_.size() == params_->observation_buffer_size())
    observations_.pop_front();

  observations_.push_back(observation);
  DCHECK_LE(observations_.size(), params_->observation_buffer_size());
}

base::Optional<int32_t> ObservationBuffer::GetPercentile(
    base::TimeTicks begin_timestamp,
    const base::Optional<int32_t>& current_signal_strength,
    int percentile,
    size_t* observations_count) const {
  // Stores weighted observations in increasing order by value.
  std::vector<WeightedObservation> weighted_observations;

  // Total weight of all observations in |weighted_observations|.
  double total_weight = 0.0;

  ComputeWeightedObservations(begin_timestamp, current_signal_strength,
                              &weighted_observations, &total_weight);

  if (observations_count) {
    // |observations_count| may be null.
    *observations_count = weighted_observations.size();
  }

  if (weighted_observations.empty())
    return base::nullopt;

  double desired_weight = percentile / 100.0 * total_weight;

  double cumulative_weight_seen_so_far = 0.0;
  for (const auto& weighted_observation : weighted_observations) {
    cumulative_weight_seen_so_far += weighted_observation.weight;
    if (cumulative_weight_seen_so_far >= desired_weight)
      return weighted_observation.value;
  }

  // Computation may reach here due to floating point errors. This may happen
  // if |percentile| was 100 (or close to 100), and |desired_weight| was
  // slightly larger than |total_weight| (due to floating point errors).
  // In this case, we return the highest |value| among all observations.
  // This is same as value of the last observation in the sorted vector.
  return weighted_observations.at(weighted_observations.size() - 1).value;
}

void ObservationBuffer::GetPercentileForEachHostWithCounts(
    base::TimeTicks begin_timestamp,
    int percentile,
    const base::Optional<std::set<IPHash>>& host_filter,
    std::map<IPHash, int32_t>* host_keyed_percentiles,
    std::map<IPHash, size_t>* host_keyed_counts) const {
  DCHECK_GE(Capacity(), Size());
  DCHECK_LE(0, percentile);
  DCHECK_GE(100, percentile);

  host_keyed_percentiles->clear();
  host_keyed_counts->clear();

  // Filter the observations based on timestamp, and the
  // presence of a valid host tag. Split the observations into a map keyed by
  // the remote host to make it easy to calculate percentiles for each host.
  std::map<IPHash, std::vector<int32_t>> host_keyed_observations;
  for (const auto& observation : observations_) {
    // Look at only those observations which have a |host|.
    if (!observation.host())
      continue;

    IPHash host = observation.host().value();
    if (host_filter && (host_filter->find(host) == host_filter->end()))
      continue;

    // Filter the observations recorded before |begin_timestamp|.
    if (observation.timestamp() < begin_timestamp)
      continue;

    // Skip 0 values of RTT.
    if (observation.value() < 1)
      continue;

    // Create the map entry if it did not already exist. Does nothing if
    // |host| was seen before.
    host_keyed_observations.emplace(host, std::vector<int32_t>());
    host_keyed_observations[host].push_back(observation.value());
  }

  if (host_keyed_observations.empty())
    return;

  // Calculate the percentile values for each host.
  for (auto& host_observations : host_keyed_observations) {
    IPHash host = host_observations.first;
    auto& observations = host_observations.second;
    std::sort(observations.begin(), observations.end());
    size_t count = observations.size();
    DCHECK_GT(count, 0u);
    (*host_keyed_counts)[host] = count;
    int percentile_index = ((count - 1) * percentile) / 100;
    (*host_keyed_percentiles)[host] = observations[percentile_index];
  }
}

void ObservationBuffer::RemoveObservationsWithSource(
    bool deleted_observation_sources[NETWORK_QUALITY_OBSERVATION_SOURCE_MAX]) {
  observations_.erase(
      std::remove_if(
          observations_.begin(), observations_.end(),
          [deleted_observation_sources](const Observation& observation) {
            return deleted_observation_sources[static_cast<size_t>(
                observation.source())];
          }),
      observations_.end());
}

void ObservationBuffer::ComputeWeightedObservations(
    const base::TimeTicks& begin_timestamp,
    const base::Optional<int32_t>& current_signal_strength,
    std::vector<WeightedObservation>* weighted_observations,
    double* total_weight) const {
  DCHECK_GE(Capacity(), Size());

  weighted_observations->clear();
  double total_weight_observations = 0.0;
  base::TimeTicks now = tick_clock_->NowTicks();

  for (const auto& observation : observations_) {
    if (observation.timestamp() < begin_timestamp)
      continue;

    base::TimeDelta time_since_sample_taken = now - observation.timestamp();
    double time_weight =
        pow(weight_multiplier_per_second_, time_since_sample_taken.InSeconds());

    double signal_strength_weight = 1.0;
    if (current_signal_strength && observation.signal_strength()) {
      int32_t signal_strength_weight_diff =
          std::abs(current_signal_strength.value() -
                   observation.signal_strength().value());
      signal_strength_weight =
          pow(weight_multiplier_per_signal_level_, signal_strength_weight_diff);
    }

    double weight = time_weight * signal_strength_weight;

    weight = std::max(DBL_MIN, std::min(1.0, weight));

    weighted_observations->push_back(
        WeightedObservation(observation.value(), weight));
    total_weight_observations += weight;
  }

  // Sort the samples by value in ascending order.
  std::sort(weighted_observations->begin(), weighted_observations->end());
  *total_weight = total_weight_observations;

  DCHECK_LE(0.0, *total_weight);
  DCHECK(weighted_observations->empty() || 0.0 < *total_weight);

  // |weighted_observations| may have a smaller size than |observations_|
  // since the former contains only the observations later than
  // |begin_timestamp|.
  DCHECK_GE(observations_.size(), weighted_observations->size());
}

size_t ObservationBuffer::Capacity() const {
  return params_->observation_buffer_size();
}

}  // namespace internal

}  // namespace nqe

}  // namespace net
