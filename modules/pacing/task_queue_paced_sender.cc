/*
 *  Copyright (c) 2019 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "modules/pacing/task_queue_paced_sender.h"

#include <algorithm>
#include <utility>

#include "api/task_queue/pending_task_safety_flag.h"
#include "api/transport/network_types.h"
#include "rtc_base/checks.h"
#include "rtc_base/experiments/field_trial_parser.h"
#include "rtc_base/experiments/field_trial_units.h"
#include "rtc_base/trace_event.h"

namespace webrtc {

namespace {

constexpr const char* kBurstyPacerFieldTrial = "WebRTC-BurstyPacer";

}  // namespace

const int TaskQueuePacedSender::kNoPacketHoldback = -1;

TaskQueuePacedSender::BurstyPacerFlags::BurstyPacerFlags(
    const FieldTrialsView& field_trials)
    : burst("burst") {
  ParseFieldTrial({&burst}, field_trials.Lookup(kBurstyPacerFieldTrial));
}

TaskQueuePacedSender::TaskQueuePacedSender(
    Clock* clock,
    PacingController::PacketSender* packet_sender,
    const FieldTrialsView& field_trials,
    TimeDelta max_hold_back_window,
    int max_hold_back_window_in_packets,
    absl::optional<TimeDelta> burst_interval)
    : clock_(clock),
      bursty_pacer_flags_(field_trials),
      max_hold_back_window_(max_hold_back_window),
      max_hold_back_window_in_packets_(max_hold_back_window_in_packets),
      pacing_controller_(clock, packet_sender, field_trials),
      next_process_time_(Timestamp::MinusInfinity()),
      is_started_(false),
      is_shutdown_(false),
      packet_size_(/*alpha=*/0.95),
      include_overhead_(false),
      task_queue_(TaskQueueBase::Current()) {
  RTC_DCHECK_GE(max_hold_back_window_, PacingController::kMinSleepTime);
  // There are multiple field trials that can affect burst. If multiple bursts
  // are specified we pick the largest of the values.
  absl::optional<TimeDelta> burst = bursty_pacer_flags_.burst.GetOptional();
  // If not overriden by an experiment, the burst is specified by the
  // `burst_interval` argument.
  if (!burst.has_value()) {
    burst = burst_interval;
  }
  if (burst.has_value()) {
    pacing_controller_.SetSendBurstInterval(burst.value());
  }
}

TaskQueuePacedSender::~TaskQueuePacedSender() {
  RTC_DCHECK_RUN_ON(task_queue_);
  is_shutdown_ = true;
}

void TaskQueuePacedSender::EnsureStarted() {
  RTC_DCHECK_RUN_ON(task_queue_);
  is_started_ = true;
  MaybeProcessPackets(Timestamp::MinusInfinity());
}

void TaskQueuePacedSender::CreateProbeClusters(
    std::vector<ProbeClusterConfig> probe_cluster_configs) {
  RTC_DCHECK_RUN_ON(task_queue_);
  pacing_controller_.CreateProbeClusters(probe_cluster_configs);
  MaybeProcessPackets(Timestamp::MinusInfinity());
}

void TaskQueuePacedSender::Pause() {
  RTC_DCHECK_RUN_ON(task_queue_);
  pacing_controller_.Pause();
}

void TaskQueuePacedSender::Resume() {
  RTC_DCHECK_RUN_ON(task_queue_);
  pacing_controller_.Resume();
  MaybeProcessPackets(Timestamp::MinusInfinity());
}

void TaskQueuePacedSender::SetCongested(bool congested) {
  RTC_DCHECK_RUN_ON(task_queue_);
  pacing_controller_.SetCongested(congested);
  MaybeProcessPackets(Timestamp::MinusInfinity());
}

void TaskQueuePacedSender::SetPacingRates(DataRate pacing_rate,
                                          DataRate padding_rate) {
  RTC_DCHECK_RUN_ON(task_queue_);
  pacing_controller_.SetPacingRates(pacing_rate, padding_rate);
  MaybeProcessPackets(Timestamp::MinusInfinity());
}

void TaskQueuePacedSender::EnqueuePackets(
    std::vector<std::unique_ptr<RtpPacketToSend>> packets) {
  task_queue_->PostTask(
      SafeTask(safety_.flag(), [this, packets = std::move(packets)]() mutable {
        RTC_DCHECK_RUN_ON(task_queue_);
        TRACE_EVENT0(TRACE_DISABLED_BY_DEFAULT("webrtc"),
                     "TaskQueuePacedSender::EnqueuePackets");
        for (auto& packet : packets) {
          TRACE_EVENT2(TRACE_DISABLED_BY_DEFAULT("webrtc"),
                       "TaskQueuePacedSender::EnqueuePackets::Loop",
                       "sequence_number", packet->SequenceNumber(),
                       "rtp_timestamp", packet->Timestamp());

          size_t packet_size = packet->payload_size() + packet->padding_size();
          if (include_overhead_) {
            packet_size += packet->headers_size();
          }
          packet_size_.Apply(1, packet_size);
          RTC_DCHECK_GE(packet->capture_time(), Timestamp::Zero());
          pacing_controller_.EnqueuePacket(std::move(packet));
        }
        MaybeProcessPackets(Timestamp::MinusInfinity());
      }));
}

void TaskQueuePacedSender::RemovePacketsForSsrc(uint32_t ssrc) {
  task_queue_->PostTask(SafeTask(safety_.flag(), [this, ssrc] {
    RTC_DCHECK_RUN_ON(task_queue_);
    pacing_controller_.RemovePacketsForSsrc(ssrc);
    MaybeProcessPackets(Timestamp::MinusInfinity());
  }));
}

void TaskQueuePacedSender::SetAccountForAudioPackets(bool account_for_audio) {
  RTC_DCHECK_RUN_ON(task_queue_);
  pacing_controller_.SetAccountForAudioPackets(account_for_audio);
  MaybeProcessPackets(Timestamp::MinusInfinity());
}

void TaskQueuePacedSender::SetIncludeOverhead() {
  RTC_DCHECK_RUN_ON(task_queue_);
  include_overhead_ = true;
  pacing_controller_.SetIncludeOverhead();
  MaybeProcessPackets(Timestamp::MinusInfinity());
}

void TaskQueuePacedSender::SetTransportOverhead(DataSize overhead_per_packet) {
  RTC_DCHECK_RUN_ON(task_queue_);
  pacing_controller_.SetTransportOverhead(overhead_per_packet);
  MaybeProcessPackets(Timestamp::MinusInfinity());
}

void TaskQueuePacedSender::SetQueueTimeLimit(TimeDelta limit) {
  RTC_DCHECK_RUN_ON(task_queue_);
  pacing_controller_.SetQueueTimeLimit(limit);
  MaybeProcessPackets(Timestamp::MinusInfinity());
}

TimeDelta TaskQueuePacedSender::ExpectedQueueTime() const {
  return GetStats().expected_queue_time;
}

DataSize TaskQueuePacedSender::QueueSizeData() const {
  return GetStats().queue_size;
}

absl::optional<Timestamp> TaskQueuePacedSender::FirstSentPacketTime() const {
  return GetStats().first_sent_packet_time;
}

TimeDelta TaskQueuePacedSender::OldestPacketWaitTime() const {
  Timestamp oldest_packet = GetStats().oldest_packet_enqueue_time;
  if (oldest_packet.IsInfinite()) {
    return TimeDelta::Zero();
  }

  // (webrtc:9716): The clock is not always monotonic.
  Timestamp current = clock_->CurrentTime();
  if (current < oldest_packet) {
    return TimeDelta::Zero();
  }

  return current - oldest_packet;
}

void TaskQueuePacedSender::OnStatsUpdated(const Stats& stats) {
  RTC_DCHECK_RUN_ON(task_queue_);
  current_stats_ = stats;
}

void TaskQueuePacedSender::MaybeProcessPackets(
    Timestamp scheduled_process_time) {
  RTC_DCHECK_RUN_ON(task_queue_);

  TRACE_EVENT0(TRACE_DISABLED_BY_DEFAULT("webrtc"),
               "TaskQueuePacedSender::MaybeProcessPackets");

  if (is_shutdown_ || !is_started_) {
    return;
  }

  Timestamp next_send_time = pacing_controller_.NextSendTime();
  RTC_DCHECK(next_send_time.IsFinite());
  const Timestamp now = clock_->CurrentTime();
  TimeDelta early_execute_margin =
      pacing_controller_.IsProbing()
          ? PacingController::kMaxEarlyProbeProcessing
          : TimeDelta::Zero();

  // Process packets and update stats.
  while (next_send_time <= now + early_execute_margin) {
    pacing_controller_.ProcessPackets();
    next_send_time = pacing_controller_.NextSendTime();
    RTC_DCHECK(next_send_time.IsFinite());

    // Probing state could change. Get margin after process packets.
    early_execute_margin = pacing_controller_.IsProbing()
                               ? PacingController::kMaxEarlyProbeProcessing
                               : TimeDelta::Zero();
  }
  UpdateStats();

  // Ignore retired scheduled task, otherwise reset `next_process_time_`.
  if (scheduled_process_time.IsFinite()) {
    if (scheduled_process_time != next_process_time_) {
      return;
    }
    next_process_time_ = Timestamp::MinusInfinity();
  }

  // Do not hold back in probing.
  TimeDelta hold_back_window = TimeDelta::Zero();
  if (!pacing_controller_.IsProbing()) {
    hold_back_window = max_hold_back_window_;
    DataRate pacing_rate = pacing_controller_.pacing_rate();
    if (max_hold_back_window_in_packets_ != kNoPacketHoldback &&
        !pacing_rate.IsZero() &&
        packet_size_.filtered() != rtc::ExpFilter::kValueUndefined) {
      TimeDelta avg_packet_send_time =
          DataSize::Bytes(packet_size_.filtered()) / pacing_rate;
      hold_back_window =
          std::min(hold_back_window,
                   avg_packet_send_time * max_hold_back_window_in_packets_);
    }
  }

  // Calculate next process time.
  TimeDelta time_to_next_process =
      std::max(hold_back_window, next_send_time - now - early_execute_margin);
  next_send_time = now + time_to_next_process;

  // If no in flight task or in flight task is later than `next_send_time`,
  // schedule a new one. Previous in flight task will be retired.
  if (next_process_time_.IsMinusInfinity() ||
      next_process_time_ > next_send_time) {
    // Prefer low precision if allowed and not probing.
    task_queue_->PostDelayedHighPrecisionTask(
        SafeTask(
            safety_.flag(),
            [this, next_send_time]() { MaybeProcessPackets(next_send_time); }),
        time_to_next_process.RoundUpTo(TimeDelta::Millis(1)));
    next_process_time_ = next_send_time;
  }
}

void TaskQueuePacedSender::UpdateStats() {
  Stats new_stats;
  new_stats.expected_queue_time = pacing_controller_.ExpectedQueueTime();
  new_stats.first_sent_packet_time = pacing_controller_.FirstSentPacketTime();
  new_stats.oldest_packet_enqueue_time =
      pacing_controller_.OldestPacketEnqueueTime();
  new_stats.queue_size = pacing_controller_.QueueSizeData();
  OnStatsUpdated(new_stats);
}

TaskQueuePacedSender::Stats TaskQueuePacedSender::GetStats() const {
  RTC_DCHECK_RUN_ON(task_queue_);
  return current_stats_;
}

}  // namespace webrtc
