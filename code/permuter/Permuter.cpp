#include <cassert>

#include <algorithm>
#include <chrono>
#include <list>
#include <memory>
#include <unordered_set>
#include <vector>

#include "Permuter.h"
#include "../utils/utils.h"

namespace fs_testing {
namespace permuter {

using std::list;
using std::pair;
using std::shared_ptr;
using std::size_t;
using std::vector;

using fs_testing::utils::disk_write;
using fs_testing::utils::DiskWriteData;

namespace {

static const unsigned int kRetryMultiplier = 2;
static const unsigned int kMinRetries = 1000;
static const unsigned int kKernelSectorSize = 512;

// Max time that can be between two bio positions before the current soft epoch
// is ended and a new one is started. Equal to 2.5 seconds.
// TODO(ashmrtn): Make this a parameter?
static const unsigned long long kSoftEpochMaxDelayNs = 2500000000;

}  // namespace


size_t BioVectorHash::operator() (const vector<unsigned int>& permutation)
    const {
  unsigned int seed = permutation.size();
  for (const auto& bio_pos : permutation) {
    seed ^= bio_pos + 0x9e3779b9 + (seed << 6) + (seed >> 2);
  }
  return seed;
}

bool BioVectorEqual::operator() (const std::vector<unsigned int>& a,
    const std::vector<unsigned int>& b) const {
  if (a.size() != b.size()) {
    return false;
  }
  for (unsigned int i = 0; i < a.size(); ++i) {
    if (a.at(i) != b.at(i)) {
      return false;
    }
  }
  return true;
}

vector<EpochOpSector> epoch_op::ToSectors(unsigned int sector_size) {
  const unsigned int num_sectors =
    (op.metadata.size + (sector_size - 1)) / sector_size;
  vector<EpochOpSector> res(num_sectors);

  for (unsigned int i = 0; i < num_sectors; ++i) {
    unsigned int size = sector_size;
    if (i == num_sectors - 1) {
      // Last sector may not be comepletely filled. This is really only a
      // problem if someone was silly and picked a sector size that isn't a
      // multiple of two smaller than the size of the bio data.
      size = op.metadata.size - (i * sector_size);
    }

    res.at(i) =
      EpochOpSector(this, i,
          (kKernelSectorSize * op.metadata.write_sector) + (i * sector_size),
          size, sector_size);
  }

  return res;
}

DiskWriteData epoch_op::ToWriteData() {
  return DiskWriteData(true, abs_index, 0,
      op.metadata.write_sector * kKernelSectorSize, op.metadata.size,
      op.get_data(), 0);
}

EpochOpSector::EpochOpSector() :
      parent(NULL), parent_sector_index(0), disk_offset(0), max_sector_size(0),
      size(0){ }

EpochOpSector::EpochOpSector(epoch_op *parent, unsigned int parent_sector_index,
    unsigned int disk_offset, unsigned int size, unsigned int max_sector_size) :
      parent(parent), parent_sector_index(parent_sector_index),
      disk_offset(disk_offset), max_sector_size(max_sector_size), size(size) { }

bool EpochOpSector::operator==(const EpochOpSector &other) const {
  if (parent != other.parent) {
    return false;
  }
  if (parent_sector_index != other.parent_sector_index) {
    return false;
  }
  if (disk_offset != other.disk_offset) {
    return false;
  }
  if (size != other.size) {
    return false;
  }
  if (max_sector_size != other.max_sector_size) {
    return false;
  }

  return true;
}

bool EpochOpSector::operator!=(const EpochOpSector &other) const {
  return !(*this == other);
}

void * EpochOpSector::GetData() {
  return parent->op.get_data().get() + (max_sector_size * parent_sector_index);
}

DiskWriteData EpochOpSector::ToWriteData() {
  return DiskWriteData(false, parent->abs_index, parent_sector_index,
      disk_offset, size, parent->op.get_data(),
      (max_sector_size * parent_sector_index));
}

epoch* Permuter::AddNewEpoch() {
  epochs_.emplace_back();
  epochs_.back().ops.clear();
  epochs_.back().num_meta = 0;
  epochs_.back().overlaps = false;
  epochs_.back().has_barrier = false;
  epochs_.back().checkpoint_epoch = 0;

  return &epochs_.back();
}

/*
 * Check if the given op has a flush flag with data. If it does, then return
 * true as it can be divided into an operation with the flush flag and an
 * operation with the data where the data should be available only in the start
 * of the next epoch. This is necessary because a flush flag only
 * stipulates the previous data is persisted, and says nothing about the
 * persistence of the data in this operation. If the FUA flag is present, then
 * the data is persisted and this operation should not be split.
 *
 * Returns true if the operation can be split according to the above, otherwise
 * false.
 */
bool Permuter::CanSplitBarrier(disk_write &barrier_op) {
  return ((barrier_op.has_flush_flag() || barrier_op.has_flush_seq_flag()) &&
      barrier_op.has_write_flag() && !barrier_op.has_FUA_flag() &&
      barrier_op.metadata.size > 0);
}

/*
 * Splits an operation into two operations, one with the flags and no data an
 * the other with the flags (sans any flush flags) and the data. This method
 * does no validation as to whether the operation should be split (use
 * CanSplitBarrier()).
 *
 * Returns a pair of operations, one with the flags and no data, the other with
 * the data and the flags sans flush flags.
 */
pair<disk_write, disk_write> Permuter::SplitBarrier(disk_write &barrier_op) {
  pair<disk_write, disk_write> res(barrier_op, barrier_op);

  if (res.first.has_flush_flag()) {
    res.second.clear_flush_flag();
  }
  if (res.first.has_flush_seq_flag()) {
    res.second.clear_flush_seq_flag();
  }

  res.first.metadata.size = 0;
  res.first.clear_data();

  return res;
}

/*
 * Given a disk_write operation and a *sorted* list of already existing ranges,
 * determine if the current operation partially or completely overlaps any of
 * the operations already in the list.
 *
 * Returns false if the operation does not belong to any range.
 * Else, returns true.
 */
bool Permuter::FindOverlapsAndInsert(disk_write &dw,
    list<pair<unsigned int, unsigned int>> &ranges) const {

  unsigned int start = dw.metadata.write_sector;
  unsigned int end = start + dw.metadata.size - 1;
  for (auto range_iter = ranges.begin(); range_iter != ranges.end();
      range_iter++) {
    if ((range_iter->first <= start && range_iter->second >= start) ||
        (range_iter->first <= end && range_iter->second >= end) ||
        (range_iter->first >= start && range_iter->second <= end)) {
      // We need to extend our range to cover what we are looking at.
      if (range_iter->first > dw.metadata.write_sector) {
        range_iter->first = dw.metadata.write_sector;
      }
      unsigned int end =
        dw.metadata.write_sector + dw.metadata.size - 1;
      if (range_iter->second < end) {
        range_iter->second = end;
      }
      return true;
    } else if (range_iter->first > end) {
      // We assume the list we are given is ordered. Therefore, if the next item
      // in the list has a start that is greater than the end of the disk_write
      // in question, we know we won't find anything else in the list this
      // disk_write overlaps with. In this case, we should insert the
      // disk_write in the list where we currently are.
      ranges.insert(range_iter,
          {dw.metadata.write_sector,
          dw.metadata.write_sector + dw.metadata.size - 1});
      return false;
    }
  }

  // We reached the end of the list of ranges without finding anything starting
  // after the end of what we are looking at.
  ranges.emplace_back(dw.metadata.write_sector,
      dw.metadata.write_sector + dw.metadata.size - 1);
  return false;
}

/*
 * Initializes the set of epochs based solely off the flags contained in the
 * recorded workload. This will lead to more pessimistic crash states in many
 * cases because nothing is assumed to be persisted unless a flush/fua operation
 * has been seen. Basically, this assumes the disk caches *all* data (regardless
 * of age) until a flush/fua operation, at which point all data is persisted.
 */
void Permuter::InitDataVector(unsigned int sector_size,
    vector<disk_write> &data) {
  sector_size_ = sector_size;
  epochs_.clear();
  list<pair<unsigned int, unsigned int>> epoch_overlaps;
  epoch *current_epoch = NULL;
  // Make sure that the first time we mark a checkpoint epoch, we start at 0 and
  // not 1.
  int curr_checkpoint_epoch = -1;
  // Aligns with the index of the bio in the profile dump, 0 indexed.
  unsigned int abs_index = 0;

  auto curr_op = data.begin();
  while (curr_op != data.end()) {
    if (current_epoch == NULL) {
      current_epoch = AddNewEpoch();
      // Overlaps are only searched for within the current epoch, not across
      // epochs.
      epoch_overlaps.clear();
      current_epoch->checkpoint_epoch = curr_checkpoint_epoch;
    }

    // While the operation is not a barrier operation (flush or FUA of some
    // sort), we should add it to the current epoch. If the operation is a
    // barrier operation, we want to add it to the current epoch and switch
    // epochs.
    while (curr_op != data.end() && !curr_op->is_barrier()) {
      // Checkpoint operations will only be seen once we have switched over
      // epochs, so we need to edit the checkpoint epoch of the current epoch as
      // well as incrementing the curr_checkpoint_epoch counter.
      if (curr_op->is_checkpoint()) {
        ++curr_checkpoint_epoch;
        current_epoch->checkpoint_epoch = curr_checkpoint_epoch;
        // Checkpoint operations should not appear in the bio stream passed to
        // actual permuters.
        ++curr_op;
        ++abs_index;
        continue;
      }

      // Check if the current operation overlaps anything we have seen already
      // in this epoch.
      if (FindOverlapsAndInsert(*curr_op, epoch_overlaps)) {
        current_epoch->overlaps = true;
      }

      current_epoch->ops.push_back({abs_index, *curr_op});
      current_epoch->num_meta += curr_op->is_meta();
      ++abs_index;
      ++curr_op;
    }

    // Check is the op at the current index is a "barrier." If it is then add it
    // to the end of the epoch, otherwise just push the current epoch onto the
    // list and move to the next segment of the log. The only reasons you should
    // get here really are 1. you reached the end of the epoch (saw a barrier)
    // or 2. you reached the end of the recorded workload.
    if (curr_op != data.end()) {
      assert(curr_op->is_barrier());

      // Check if the op at the current index has a flush flag with data. If it
      // does, then divide it into an operation with the flush flag and an
      // operation with the data where the data is available only in the start
      // of the next epoch. This is necessary because a flush flag only
      // stipulates the previous data is persisted, and says nothing about the
      // data in the current operation's persistence. If the FUA flag is
      // present, then the current data is persisted with the previous data,
      // meaning this block does not apply.
      if (CanSplitBarrier(*curr_op)) {
        pair<disk_write, disk_write> split = SplitBarrier(*curr_op);

        // Add the flush to the current epoch.
        current_epoch->ops.push_back({abs_index, split.first});
        current_epoch->num_meta += split.first.is_meta();
        current_epoch->has_barrier = true;

        // Switch epochs.
        current_epoch = AddNewEpoch();
        current_epoch->checkpoint_epoch = curr_checkpoint_epoch;
        epoch_overlaps.clear();
        // We are adding a new operation to the new epoch, so we need to record
        // it in the list of things to check for overlaps.
        FindOverlapsAndInsert(split.second, epoch_overlaps);

        // Setup the rest of the data part of the operation.
        // TODO(ashmrtn): Find a better way to handle matching an index to a bio
        // in the profile dump.
        current_epoch->ops.push_back({abs_index, split.second});
        current_epoch->num_meta += split.second.is_meta();

        ++abs_index;
        ++curr_op;
      } else {
        // This is just the case where we have a normal barrier operation ending
        // the epoch.
        current_epoch->ops.push_back({abs_index, *curr_op});
        current_epoch->num_meta += curr_op->is_meta();
        current_epoch->has_barrier = true;
        ++abs_index;
        ++curr_op;

        // This will cause us to create a new epoch at the end of our vector on
        // the next loop.
        current_epoch = NULL;
      }
    }
  }
}

/*
 * Initializes the set of epochs based on both the relative times between bio
 * submissions and the flags within the workload. This leads to crash states
 * where opertions are considered persisted if enough time has passed between
 * the submission of one operation and the submission of the next operation.
 *
 * If a checkpoint is between two operations such that the time between the
 * checkpoint and either operation is less than the soft epoch cutoff time but
 * the time between the operations themselves is greater than or equal to the
 * soft epoch cutoff time, the operations are considered to be in different soft
 * epochs and the later operation (and its soft epoch) is after the intervening
 * checkpoint.
 */
void Permuter::InitDataVectorSoft(unsigned int sector_size,
    vector<disk_write> &data) {
  sector_size_ = sector_size;
  const std::chrono::nanoseconds max_delay(kSoftEpochMaxDelayNs);

  epochs_.clear();
  list<pair<unsigned int, unsigned int>> epoch_overlaps;
  epoch *current_epoch = AddNewEpoch();
  // Make sure that the first time we mark a checkpoint epoch, we start at 0 and
  // not 1.
  int curr_checkpoint_epoch = -1;
  // Aligns with the index of the bio in the profile dump, 0 indexed.
  unsigned int abs_index = 0;

  // Dummy starting value. Not changed when checkpoints are seen. Set to 0 every
  // time we end an epoch with a flush/fua so that we don't compare times across
  // soft epochs.
  std::chrono::nanoseconds last_time_seen(0);
  auto curr_op = data.begin();
  while (curr_op != data.end()) {
    if (curr_op->is_checkpoint()) {
      // We may be switching soft epochs on the next opeation, so don't set the
      // checkpoint epoch unless we know that we just switched epochs.
      ++curr_checkpoint_epoch;
      if (current_epoch->ops.size() == 0) {
        current_epoch->checkpoint_epoch = curr_checkpoint_epoch;
      }
    } else if (!curr_op->is_barrier()) {
      // Regular write operation, so compare times and add this operation to the
      // proper soft epoch.
      std::chrono::nanoseconds cur_time(curr_op->metadata.time_ns);
      std::chrono::duration<long long, std::nano> diff =
        cur_time - last_time_seen;
      if (last_time_seen.count() > 0 && diff >= max_delay) {
        // We need a new soft epoch.
        current_epoch = AddNewEpoch();
        epoch_overlaps.clear();
        current_epoch->checkpoint_epoch = curr_checkpoint_epoch;
      }

      // In all cases, we need to put the current operation at the back of
      // the current epoch.
      current_epoch->ops.push_back({abs_index, *curr_op});
      current_epoch->num_meta += curr_op->is_meta();
      last_time_seen = std::chrono::nanoseconds(curr_op->metadata.time_ns);
      if (FindOverlapsAndInsert(*curr_op, epoch_overlaps)) {
        current_epoch->overlaps = true;
      }
    } else {
      // We have a barrier operation. We need to decide if this barrier
      // operation has data that appears in the next epoch or if it just ends
      // the current epoch.
      if (CanSplitBarrier(*curr_op)) {
        pair<disk_write, disk_write> split = SplitBarrier(*curr_op);

        // Add the flush to the current epoch.
        current_epoch->ops.push_back({abs_index, split.first});
        current_epoch->num_meta += split.first.is_meta();
        current_epoch->has_barrier = true;

        // Switch epochs.
        current_epoch = AddNewEpoch();
        epoch_overlaps.clear();
        current_epoch->checkpoint_epoch = curr_checkpoint_epoch;
        // We are adding a new operation to the new epoch, so we need to record
        // it in the list of things to check for overlaps.
        FindOverlapsAndInsert(split.second, epoch_overlaps);

        // Setup the rest of the data part of the operation.
        // TODO(ashmrtn): Find a better way to handle matching an index to a bio
        // in the profile dump.
        current_epoch->ops.push_back({abs_index, split.second});
        current_epoch->num_meta += split.second.is_meta();
      } else {
        // This is just the case where we have a normal barrier operation ending
        // the epoch.
        current_epoch->ops.push_back({abs_index, *curr_op});
        current_epoch->num_meta += curr_op->is_meta();
        current_epoch->has_barrier = true;

        // Create a new epoch.
        current_epoch = AddNewEpoch();
        epoch_overlaps.clear();
        current_epoch->checkpoint_epoch = curr_checkpoint_epoch;
      }

      last_time_seen = std::chrono::nanoseconds(0);
    }

    ++curr_op;
    ++abs_index;
  }

  // There is the possibility that we created an empty final epoch with no new
  // checkpoint due to the way we switch epochs. If this the case, we should
  // remove that element from the list of epochs we track.
  if (epochs_.size() > 1 &&
      (epochs_.at(epochs_.size() - 1).checkpoint_epoch ==
        epochs_.at(epochs_.size() - 2).checkpoint_epoch) &&
      epochs_.back().ops.size() == 0) {
    epochs_.pop_back();
  }
}

vector<epoch>* Permuter::GetEpochs() {
  return &epochs_;
}


bool Permuter::GenerateCrashState(vector<DiskWriteData> &res,
    PermuteTestResult &log_data) {
  vector<epoch_op> crash_state;
  unsigned long retries = 0;
  unsigned int exists = 0;
  bool new_state = true;
  vector<unsigned int> crash_state_hash;

  unsigned long max_retries =
    ((kRetryMultiplier * completed_permutations_.size()) < kMinRetries)
      ? kMinRetries
      : kRetryMultiplier * completed_permutations_.size();
  do {
    new_state = gen_one_state(crash_state, log_data);

    crash_state_hash.clear();
    crash_state_hash.resize(crash_state.size());
    for (unsigned int i = 0; i < crash_state.size(); ++i) {
      crash_state_hash.at(i) = crash_state.at(i).abs_index;
    }

    ++retries;
    exists = completed_permutations_.count(crash_state_hash);
    if (!new_state || retries >= max_retries) {
      // We've likely found all possible crash states so just break. The
      // constant in the multiplier was randomly chosen in the hopes that it
      // would be a good hueristic. This is more to make sure that we don't spin
      // endlessly than it is for it to be a good way to break out of trying to
      // make unique permutations.
      break;
    }
  } while (exists > 0);

  // Move the permuted crash state data over into the returned crash state
  // vector.
  res.resize(crash_state.size());
  for (unsigned int i = 0; i < crash_state.size(); ++i) {
    res.at(i) = crash_state.at(i).ToWriteData();
  }

  // Messy bit to add everything to the logging data struct.
  log_data.crash_state = res;

  if (exists == 0) {
    completed_permutations_.insert(crash_state_hash);
    // We broke out of the above loop because this state is unique.
    return new_state;
  }

  // We broke out of the above loop because we haven't found a new state in some
  // time.
  return false;
}

bool Permuter::GenerateSectorCrashState(std::vector<DiskWriteData> &res,
    PermuteTestResult &log_data) {
  unsigned long retries = 0;
  unsigned int exists = 0;
  bool new_state = true;
  vector<unsigned int> crash_state_hash;

  unsigned long max_retries =
    ((kRetryMultiplier * completed_permutations_.size()) < kMinRetries)
      ? kMinRetries
      : kRetryMultiplier * completed_permutations_.size();
  do {
    new_state = gen_one_sector_state(res, log_data);

    crash_state_hash.clear();
    // We need both the sector index in the epoch and and which epoch_op that
    // sector came from to ensure uniqueness (would also work to index all
    // sectors across all epoch_ops, but we haven't done that).
    crash_state_hash.resize(res.size() * 2);
    for (unsigned int i = 0; i < res.size(); ++i) {
      crash_state_hash.at((i << 1)) = res.at(i).bio_index;
      crash_state_hash.at((i << 1) + 1) = res.at(i).bio_sector_index;
    }

    ++retries;
    exists = completed_permutations_.count(crash_state_hash);
    if (!new_state || retries >= max_retries) {
      // We've likely found all possible crash states so just break. The
      // constant in the multiplier was randomly chosen in the hopes that it
      // would be a good hueristic. This is more to make sure that we don't spin
      // endlessly than it is for it to be a good way to break out of trying to
      // make unique permutations.
      break;
    }
  } while (exists > 0);

  // Move the permuted crash state data over into the returned crash state
  // vector.
  log_data.crash_state = res;

  if (exists == 0) {
    completed_permutations_.insert(crash_state_hash);
    // We broke out of the above loop because this state is unique.
    return new_state;
  }

  // We broke out of the above loop because we haven't found a new state in some
  // time.
  return false;
}

vector<EpochOpSector> Permuter::CoalesceSectors(
    vector<EpochOpSector> &sector_list) {

  // At most, the returned vector will have as many elements as the given
  // vector.
  vector<EpochOpSector> res(sector_list.size());
  unsigned int num_unique_sectors = 0;
  // Place to store previously seen sectors for latere comparison.
  std::unordered_set<unsigned int> sector_offsets;

  // Iterate through the list of sectors backwards, adding any new sectors
  // encountered.
  for (auto iter = sector_list.rbegin(); iter != sector_list.rend(); ++iter) {
    if (sector_offsets.count(iter->disk_offset) == 0) {
      res.at(num_unique_sectors) = *iter;
      ++num_unique_sectors;
      sector_offsets.insert(iter->disk_offset);
    }
  }

  // Trim down to the actual number of unique sectors for this list.
  res.resize(num_unique_sectors);
  // Reverse the list of sectors that we generated.
  std::reverse(res.begin(), res.end());

  return res;
}

}  // namespace permuter
}  // namespace fs_testing
