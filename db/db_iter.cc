//  Copyright (c) 2013, Facebook, Inc.  All rights reserved.
//  This source code is licensed under the BSD-style license found in the
//  LICENSE file in the root directory of this source tree. An additional grant
//  of patent rights can be found in the PATENTS file in the same directory.
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "db/db_iter.h"
#include <stdexcept>
#include <deque>

#include "db/filename.h"
#include "db/dbformat.h"
#include "rocksdb/env.h"
#include "rocksdb/options.h"
#include "rocksdb/iterator.h"
#include "rocksdb/merge_operator.h"
#include "port/port.h"
#include "util/logging.h"
#include "util/mutexlock.h"
#include "util/perf_context_imp.h"

namespace rocksdb {

#if 0
static void DumpInternalIter(Iterator* iter) {
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    ParsedInternalKey k;
    if (!ParseInternalKey(iter->key(), &k)) {
      fprintf(stderr, "Corrupt '%s'\n", EscapeString(iter->key()).c_str());
    } else {
      fprintf(stderr, "@ '%s'\n", k.DebugString().c_str());
    }
  }
}
#endif

namespace {

class IterLookupKey {
 public:
  IterLookupKey() : key_(space_), buf_size_(sizeof(space_)), key_size_(0) {}

  ~IterLookupKey() { Clear(); }

  Slice GetKey() const {
    if (key_ != nullptr) {
      return Slice(key_, key_size_);
    } else {
      return Slice();
    }
  }

  bool Valid() const { return key_ != nullptr; }

  void Clear() {
    if (key_ != nullptr && key_ != space_) {
      delete[] key_;
    }
    key_ = space_;
    buf_size_ = sizeof(buf_size_);
  }

  // Enlarge the buffer size if needed based on key_size.
  // By default, static allocated buffer is used. Once there is a key
  // larger than the static allocated buffer, another buffer is dynamically
  // allocated, until a larger key buffer is requested. In that case, we
  // reallocate buffer and delete the old one.
  void EnlargeBufferIfNeeded(size_t key_size) {
    // If size is smaller than buffer size, continue using current buffer,
    // or the static allocated one, as default
    if (key_size > buf_size_) {
      // Need to enlarge the buffer.
      Clear();
      key_ = new char[key_size];
      buf_size_ = key_size;
    }
    key_size_ = key_size;
  }

  void SetUserKey(const Slice& user_key) {
    size_t size = user_key.size();
    EnlargeBufferIfNeeded(size);
    memcpy(key_, user_key.data(), size);
  }

  void SetInternalKey(const Slice& user_key, SequenceNumber s) {
    size_t usize = user_key.size();
    EnlargeBufferIfNeeded(usize + sizeof(uint64_t));
    memcpy(key_, user_key.data(), usize);
    EncodeFixed64(key_ + usize, PackSequenceAndType(s, kValueTypeForSeek));
  }

 private:
  char* key_;
  size_t buf_size_;
  size_t key_size_;
  char space_[32];  // Avoid allocation for short keys

  // No copying allowed
  IterLookupKey(const IterLookupKey&) = delete;
  void operator=(const LookupKey&) = delete;
};

// Memtables and sstables that make the DB representation contain
// (userkey,seq,type) => uservalue entries.  DBIter
// combines multiple entries for the same userkey found in the DB
// representation into a single entry while accounting for sequence
// numbers, deletion markers, overwrites, etc.
class DBIter: public Iterator {
 public:
  // The following is grossly complicated. TODO: clean it up
  // Which direction is the iterator currently moving?
  // (1) When moving forward, the internal iterator is positioned at
  //     the exact entry that yields this->key(), this->value()
  // (2) When moving backwards, the internal iterator is positioned
  //     just before all entries whose user key == this->key().
  enum Direction {
    kForward,
    kReverse
  };

  DBIter(const std::string* dbname, Env* env, const Options& options,
         const Comparator* cmp, Iterator* iter, SequenceNumber s)
      : dbname_(dbname),
        env_(env),
        logger_(options.info_log.get()),
        user_comparator_(cmp),
        user_merge_operator_(options.merge_operator.get()),
        iter_(iter),
        sequence_(s),
        direction_(kForward),
        valid_(false),
        current_entry_is_merged_(false),
        statistics_(options.statistics.get()) {
    RecordTick(statistics_, NO_ITERATORS, 1);
    max_skip_ = options.max_sequential_skip_in_iterations;
  }
  virtual ~DBIter() {
    RecordTick(statistics_, NO_ITERATORS, -1);
    delete iter_;
  }
  virtual bool Valid() const { return valid_; }
  virtual Slice key() const {
    assert(valid_);
    return saved_key_.GetKey();
  }
  virtual Slice value() const {
    assert(valid_);
    return (direction_ == kForward && !current_entry_is_merged_) ?
      iter_->value() : saved_value_;
  }
  virtual Status status() const {
    if (status_.ok()) {
      return iter_->status();
    } else {
      return status_;
    }
  }

  virtual void Next();
  virtual void Prev();
  virtual void Seek(const Slice& target);
  virtual void SeekToFirst();
  virtual void SeekToLast();

 private:
  inline void FindNextUserEntry(bool skipping);
  void FindNextUserEntryInternal(bool skipping);
  void FindPrevUserEntry();
  bool ParseKey(ParsedInternalKey* key);
  void MergeValuesNewToOld();

  inline void ClearSavedValue() {
    if (saved_value_.capacity() > 1048576) {
      std::string empty;
      swap(empty, saved_value_);
    } else {
      saved_value_.clear();
    }
  }

  const std::string* const dbname_;
  Env* const env_;
  Logger* logger_;
  const Comparator* const user_comparator_;
  const MergeOperator* const user_merge_operator_;
  Iterator* const iter_;
  SequenceNumber const sequence_;

  Status status_;
  IterLookupKey saved_key_;   // == current key when direction_==kReverse
  std::string saved_value_;   // == current raw value when direction_==kReverse
  std::string skip_key_;
  Direction direction_;
  bool valid_;
  bool current_entry_is_merged_;
  Statistics* statistics_;
  uint64_t max_skip_;

  // No copying allowed
  DBIter(const DBIter&);
  void operator=(const DBIter&);
};

inline bool DBIter::ParseKey(ParsedInternalKey* ikey) {
  if (!ParseInternalKey(iter_->key(), ikey)) {
    status_ = Status::Corruption("corrupted internal key in DBIter");
    Log(logger_, "corrupted internal key in DBIter: %s",
        iter_->key().ToString(true).c_str());
    return false;
  } else {
    return true;
  }
}

void DBIter::Next() {
  assert(valid_);

  if (direction_ == kReverse) {  // Switch directions?
    direction_ = kForward;
    // iter_ is pointing just before the entries for this->key(),
    // so advance into the range of entries for this->key() and then
    // use the normal skipping code below.
    if (!iter_->Valid()) {
      iter_->SeekToFirst();
    } else {
      iter_->Next();
    }
    if (!iter_->Valid()) {
      valid_ = false;
      saved_key_.Clear();
      return;
    }
  }

  // If the current value is merged, we might already hit end of iter_
  if (!iter_->Valid()) {
    valid_ = false;
    return;
  }
  FindNextUserEntry(true /* skipping the current user key */);
}


// PRE: saved_key_ has the current user key if skipping
// POST: saved_key_ should have the next user key if valid_,
//       if the current entry is a result of merge
//           current_entry_is_merged_ => true
//           saved_value_             => the merged value
//
// NOTE: In between, saved_key_ can point to a user key that has
//       a delete marker
inline void DBIter::FindNextUserEntry(bool skipping) {
  StopWatchNano timer(env_, false);
  StartPerfTimer(&timer);
  FindNextUserEntryInternal(skipping);
  BumpPerfTime(&perf_context.find_next_user_entry_time, &timer);
}

// Actual implementation of DBIter::FindNextUserEntry()
void DBIter::FindNextUserEntryInternal(bool skipping) {
  // Loop until we hit an acceptable entry to yield
  assert(iter_->Valid());
  assert(direction_ == kForward);
  current_entry_is_merged_ = false;
  uint64_t num_skipped = 0;
  do {
    ParsedInternalKey ikey;
    if (ParseKey(&ikey) && ikey.sequence <= sequence_) {
      if (skipping &&
          user_comparator_->Compare(ikey.user_key, saved_key_.GetKey()) <= 0) {
        num_skipped++; // skip this entry
        BumpPerfCount(&perf_context.internal_key_skipped_count);
      } else {
        skipping = false;
        switch (ikey.type) {
          case kTypeDeletion:
            // Arrange to skip all upcoming entries for this key since
            // they are hidden by this deletion.
            saved_key_.SetUserKey(ikey.user_key);
            skipping = true;
            num_skipped = 0;
            BumpPerfCount(&perf_context.internal_delete_skipped_count);
            break;
          case kTypeValue:
            valid_ = true;
            saved_key_.SetUserKey(ikey.user_key);
            return;
          case kTypeMerge:
            // By now, we are sure the current ikey is going to yield a value
            saved_key_.SetUserKey(ikey.user_key);
            current_entry_is_merged_ = true;
            valid_ = true;
            MergeValuesNewToOld();  // Go to a different state machine
            return;
          default:
            assert(false);
            break;
        }
      }
    }
    // If we have sequentially iterated via numerous keys and still not
    // found the next user-key, then it is better to seek so that we can
    // avoid too many key comparisons. We seek to the last occurence of
    // our current key by looking for sequence number 0.
    if (skipping && num_skipped > max_skip_) {
      num_skipped = 0;
      std::string last_key;
      AppendInternalKey(&last_key, ParsedInternalKey(saved_key_.GetKey(), 0,
                                                     kValueTypeForSeek));
      iter_->Seek(last_key);
      RecordTick(statistics_, NUMBER_OF_RESEEKS_IN_ITERATION);
    } else {
      iter_->Next();
    }
  } while (iter_->Valid());
  valid_ = false;
}

// Merge values of the same user key starting from the current iter_ position
// Scan from the newer entries to older entries.
// PRE: iter_->key() points to the first merge type entry
//      saved_key_ stores the user key
// POST: saved_value_ has the merged value for the user key
//       iter_ points to the next entry (or invalid)
void DBIter::MergeValuesNewToOld() {
  if (!user_merge_operator_) {
    Log(logger_, "Options::merge_operator is null.");
    throw std::logic_error("DBIter::MergeValuesNewToOld() with"
                           " Options::merge_operator null");
  }

  // Start the merge process by pushing the first operand
  std::deque<std::string> operands;
  operands.push_front(iter_->value().ToString());

  std::string merge_result;   // Temporary string to hold merge result later
  ParsedInternalKey ikey;
  for (iter_->Next(); iter_->Valid(); iter_->Next()) {
    if (!ParseKey(&ikey)) {
      // skip corrupted key
      continue;
    }

    if (user_comparator_->Compare(ikey.user_key, saved_key_.GetKey()) != 0) {
      // hit the next user key, stop right here
      break;
    }

    if (kTypeDeletion == ikey.type) {
      // hit a delete with the same user key, stop right here
      // iter_ is positioned after delete
      iter_->Next();
      break;
    }

    if (kTypeValue == ikey.type) {
      // hit a put, merge the put value with operands and store the
      // final result in saved_value_. We are done!
      // ignore corruption if there is any.
      const Slice value = iter_->value();
      user_merge_operator_->FullMerge(ikey.user_key, &value, operands,
                                      &saved_value_, logger_);
      // iter_ is positioned after put
      iter_->Next();
      return;
    }

    if (kTypeMerge == ikey.type) {
      // hit a merge, add the value as an operand and run associative merge.
      // when complete, add result to operands and continue.
      const Slice& value = iter_->value();
      operands.push_front(value.ToString());
    }
  }

  // we either exhausted all internal keys under this user key, or hit
  // a deletion marker.
  // feed null as the existing value to the merge operator, such that
  // client can differentiate this scenario and do things accordingly.
  user_merge_operator_->FullMerge(saved_key_.GetKey(), nullptr, operands,
                                  &saved_value_, logger_);
}

void DBIter::Prev() {
  assert(valid_);

  // Throw an exception now if merge_operator is provided
  // TODO: support backward iteration
  if (user_merge_operator_) {
    Log(logger_, "Prev not supported yet if merge_operator is provided");
    throw std::logic_error("DBIter::Prev backward iteration not supported"
                           " if merge_operator is provided");
  }

  if (direction_ == kForward) {  // Switch directions?
    // iter_ is pointing at the current entry.  Scan backwards until
    // the key changes so we can use the normal reverse scanning code.
    assert(iter_->Valid());  // Otherwise valid_ would have been false
    saved_key_.SetUserKey(ExtractUserKey(iter_->key()));
    while (true) {
      iter_->Prev();
      if (!iter_->Valid()) {
        valid_ = false;
        saved_key_.Clear();
        ClearSavedValue();
        return;
      }
      if (user_comparator_->Compare(ExtractUserKey(iter_->key()),
                                    saved_key_.GetKey()) < 0) {
        break;
      }
    }
    direction_ = kReverse;
  }

  FindPrevUserEntry();
}

void DBIter::FindPrevUserEntry() {
  assert(direction_ == kReverse);
  uint64_t num_skipped = 0;

  ValueType value_type = kTypeDeletion;
  bool saved_key_valid = true;
  if (iter_->Valid()) {
    do {
      ParsedInternalKey ikey;
      if (ParseKey(&ikey) && ikey.sequence <= sequence_) {
        if ((value_type != kTypeDeletion) &&
            user_comparator_->Compare(ikey.user_key, saved_key_.GetKey()) < 0) {
          // We encountered a non-deleted value in entries for previous keys,
          break;
        }
        value_type = ikey.type;
        if (value_type == kTypeDeletion) {
          saved_key_.Clear();
          ClearSavedValue();
          saved_key_valid = false;
        } else {
          Slice raw_value = iter_->value();
          if (saved_value_.capacity() > raw_value.size() + 1048576) {
            std::string empty;
            swap(empty, saved_value_);
          }
          saved_key_.SetUserKey(ExtractUserKey(iter_->key()));
          saved_value_.assign(raw_value.data(), raw_value.size());
        }
      } else {
        // In the case of ikey.sequence > sequence_, we might have already
        // iterated to a different user key.
        saved_key_valid = false;
      }
      num_skipped++;
      // If we have sequentially iterated via numerous keys and still not
      // found the prev user-key, then it is better to seek so that we can
      // avoid too many key comparisons. We seek to the first occurence of
      // our current key by looking for max sequence number.
      if (saved_key_valid && num_skipped > max_skip_) {
        num_skipped = 0;
        std::string last_key;
        AppendInternalKey(&last_key, ParsedInternalKey(saved_key_.GetKey(),
                                                       kMaxSequenceNumber,
                                                       kValueTypeForSeek));
        iter_->Seek(last_key);
        RecordTick(statistics_, NUMBER_OF_RESEEKS_IN_ITERATION);
      } else {
        iter_->Prev();
      }
    } while (iter_->Valid());
  }

  if (value_type == kTypeDeletion) {
    // End
    valid_ = false;
    saved_key_.Clear();
    ClearSavedValue();
    direction_ = kForward;
  } else {
    valid_ = true;
  }
}

void DBIter::Seek(const Slice& target) {
  saved_key_.Clear();
  // now savved_key is used to store internal key.
  saved_key_.SetInternalKey(target, sequence_);
  StopWatchNano internal_seek_timer(env_, false);
  StartPerfTimer(&internal_seek_timer);
  iter_->Seek(saved_key_.GetKey());
  BumpPerfTime(&perf_context.seek_internal_seek_time, &internal_seek_timer);
  if (iter_->Valid()) {
    direction_ = kForward;
    ClearSavedValue();
    FindNextUserEntry(false /*not skipping */);
  } else {
    valid_ = false;
  }
}

void DBIter::SeekToFirst() {
  direction_ = kForward;
  ClearSavedValue();
  StopWatchNano internal_seek_timer(env_, false);
  StartPerfTimer(&internal_seek_timer);
  iter_->SeekToFirst();
  BumpPerfTime(&perf_context.seek_internal_seek_time, &internal_seek_timer);
  if (iter_->Valid()) {
    FindNextUserEntry(false /* not skipping */);
  } else {
    valid_ = false;
  }
}

void DBIter::SeekToLast() {
  // Throw an exception for now if merge_operator is provided
  // TODO: support backward iteration
  if (user_merge_operator_) {
    Log(logger_, "SeekToLast not supported yet if merge_operator is provided");
    throw std::logic_error("DBIter::SeekToLast: backward iteration not"
                           " supported if merge_operator is provided");
  }

  direction_ = kReverse;
  ClearSavedValue();
  StopWatchNano internal_seek_timer(env_, false);
  StartPerfTimer(&internal_seek_timer);
  iter_->SeekToLast();
  BumpPerfTime(&perf_context.seek_internal_seek_time, &internal_seek_timer);
  FindPrevUserEntry();
}

}  // anonymous namespace

Iterator* NewDBIterator(
    const std::string* dbname,
    Env* env,
    const Options& options,
    const Comparator *user_key_comparator,
    Iterator* internal_iter,
    const SequenceNumber& sequence) {
  return new DBIter(dbname, env, options, user_key_comparator,
                    internal_iter, sequence);
}

}  // namespace rocksdb
