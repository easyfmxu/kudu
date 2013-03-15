// Copyright (c) 2012, Cloudera, inc.

#include <arpa/inet.h>
#include <string>

#include "cfile/block_cache.h"
#include "cfile/block_encodings.h"
#include "cfile/cfile.h"
#include "cfile/cfile_reader.h"
#include "cfile/string_plain_block.h"
#include "tablet/deltafile.h"
#include "util/coding-inl.h"
#include "util/env.h"
#include "util/hexdump.h"

namespace kudu {

using cfile::BlockCacheHandle;
using cfile::BlockPointer;

namespace tablet {

DeltaFileWriter::DeltaFileWriter(const Schema &schema,
                                 const shared_ptr<WritableFile> &file) :
  schema_(schema)
#ifndef NDEBUG
  ,last_row_idx_(~0)
#endif
{
  cfile::WriterOptions opts;
  opts.write_validx = true;
  writer_.reset(new cfile::Writer(opts, STRING, cfile::PLAIN, file));
}


Status DeltaFileWriter::Start() {
  return writer_->Start();
}

Status DeltaFileWriter::Finish() {
  return writer_->Finish();
}

Status DeltaFileWriter::AppendDelta(
  uint32_t row_idx, const RowChangeList &delta) {
  Slice delta_slice(delta.slice());

#ifndef NDEBUG
  // Sanity check insertion order in debug mode.
  if (last_row_idx_ != ~0) {
    DCHECK_GT(row_idx, last_row_idx_) <<
      "must insert deltas in sorted order!";
  }
  last_row_idx_ = row_idx;
#endif


  tmp_buf_.clear();

  // Write the row index in big-endian, so that it
  // sorts correctly
  // TODO: we could probably varint encode it and save some space, no?
  // varints still sort lexicographically, no?
  uint32_t row_idx_bigendian = htonl(row_idx);
  tmp_buf_.append(&row_idx_bigendian, sizeof(uint32_t));

  tmp_buf_.append(delta_slice.data(), delta_slice.size());
  return writer_->AppendEntries(&tmp_buf_, 1, 0);
}

////////////////////////////////////////////////////////////
// Reader
////////////////////////////////////////////////////////////

Status DeltaFileReader::Open(Env *env, const string &path,
                             const Schema &schema,
                             DeltaFileReader **reader) {
  RandomAccessFile *raf;
  RETURN_NOT_OK(env->NewRandomAccessFile(path, &raf));
  shared_ptr<RandomAccessFile> f(raf);

  uint64_t size;
  RETURN_NOT_OK(env->GetFileSize(path, &size));

  *reader = new DeltaFileReader(f, size, schema);
  return (*reader)->Init();
}

DeltaFileReader::DeltaFileReader(
  const shared_ptr<RandomAccessFile> &file,
  uint64_t file_size,
  const Schema &schema) :
  reader_(new cfile::CFileReader(cfile::ReaderOptions(),
                                 file, file_size)),
  schema_(schema)
{
}

Status DeltaFileReader::Init() {
  RETURN_NOT_OK(reader_->Init());

  if (!reader_->has_validx()) {
    return Status::Corruption("file does not have a value index!");
  }
  return Status::OK();
}

Status DeltaFileReader::ApplyUpdates(
  size_t col_idx, uint32_t start_row, ColumnBlock *dst) const
{
  VLOG(2) << "Starting to apply updates for " <<
    start_row << "-" << (start_row + dst->size());
  BlockPointer validx_root = reader_->validx_root();
  gscoped_ptr<cfile::IndexTreeIterator> iter(
    cfile::IndexTreeIterator::Create(reader_.get(), STRING, validx_root));

  uint32_t start_row_bigendian = htonl(start_row);
  Slice key_slice(reinterpret_cast<const uint8_t *>(&start_row_bigendian),
                  sizeof(uint32_t));

  Status s = iter->SeekAtOrBefore(&key_slice);
  if (PREDICT_FALSE(s.IsNotFound())) {
    // Seeking to a value before the first value in the file
    // will return NotFound, due to the way the index seek
    // works. We need to special-case this and have the
    // iterator seek all the way down its leftmost branches
    // to get the correct result.
    s = iter->SeekToFirst();
  }

  RETURN_NOT_OK(s);

  // Successful seek - process deltas until we have gotten
  // all that apply to this range of rows.
  // TODO: this is a very inefficient impl based around reusing
  // StringPlainBlockDecoder. Instead we should have a Delta-specific
  // format which can easily skip if a column isn't updated, etc.
  // - maybe even its own file format outside of cfile, so the index
  // structure can carry filter information on columns, etc.

  ScopedColumnBlock<STRING> buf(1000);
  while (true) {
    BlockPointer dblk_ptr = iter->GetCurrentBlockPointer();
    BlockCacheHandle dblk_data;

    RETURN_NOT_OK(reader_->ReadBlock(dblk_ptr, &dblk_data));

    cfile::StringPlainBlockDecoder sbd(dblk_data.data());
    RETURN_NOT_OK(sbd.ParseHeader());

    bool exact;
    s = sbd.SeekAtOrAfterValue(&key_slice, &exact);
    if (!s.ok()) {
      if (s.IsNotFound()) {
        // This can happen if we seek into the middle of a block, but we
        // hit _after_ the key we're looking for, and there is no more
        // data in the block.
      } else {
        // Actual error.
        return s;
      }
    } else {
      while (sbd.HasNext()) {
        size_t n = buf.size();
        RETURN_NOT_OK(sbd.CopyNextValues(&n, &buf));

        // Process all of the deltas fetched in this batch
        for (int i = 0; i < n; i++) {
          const Slice &slice = buf[i];
          bool done;
          Status s = ApplyEncodedDelta(slice, col_idx, start_row, dst, &done);
          if (!s.ok()) {
            LOG(WARNING) << "Unable to apply delta from block " <<
              dblk_ptr.ToString() << ": " << s.ToString();
            return s;
          }
          if (done) {
            return Status::OK();
          }
        }
      }

      VLOG(2) << "processed all deltas from block " << dblk_ptr.ToString();
    }

    if (iter->HasNext()) {
      RETURN_NOT_OK(iter->Next());
      buf.arena()->Reset();
    } else {
      break;
    }
  }
  
  return Status::OK();
}

Status DeltaFileReader::ApplyEncodedDelta(const Slice &s_in, size_t col_idx,
                                          uint32_t start_row, ColumnBlock *dst,
                                          bool *done) const
{
  *done = false;

  Slice s(s_in);

  if (s.size() < sizeof(uint32_t)) {
    return Status::Corruption("delta slice too small");
  }

  // Decode and check the ID of the row we're going to update.
  uint32_t rowid = ntohl(*reinterpret_cast<const uint32_t *>(s.data()));

  if (rowid >= start_row + dst->size()) {
    VLOG(2) << "done processing deltas for block "
            << start_row << "-" << (start_row + dst->size());
    *done = true;
    return Status::OK();
  }

  size_t rel_idx = rowid - start_row;

  // Skip the rowid we just decoded.
  s.remove_prefix(sizeof(uint32_t));


  RowChangeListDecoder decoder(schema_, s);
  return decoder.ApplyToOneColumn(col_idx, dst->cell_ptr(rel_idx), dst->arena());
}

} // namespace tablet
} // namespace kudu
