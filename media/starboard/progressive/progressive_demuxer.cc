// Copyright 2012 The Cobalt Authors. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "media/starboard/progressive/progressive_demuxer.h"

#include <inttypes.h>

#include <utility>

#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/functional/callback_helpers.h"
#include "base/strings/stringprintf.h"
#include "base/task/bind_post_task.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "base/trace_event/trace_event.h"
#include "media/base/data_source.h"
#include "media/base/timestamp_constants.h"
#include "media/starboard/starboard_utils.h"
#include "starboard/types.h"

namespace media {

ProgressiveDemuxerStream::ProgressiveDemuxerStream(ProgressiveDemuxer* demuxer,
                                                   Type type)
    : demuxer_(demuxer), type_(type) {
  DCHECK(demuxer_);
}

void ProgressiveDemuxerStream::Read(uint32_t count, ReadCB read_cb) {
  base::AutoLock auto_lock(lock_);
  DCHECK(!read_cb_);

  read_cb_ = base::BindPostTaskToCurrentDefault(std::move(read_cb));

  // Don't accept any additional reads if we've been told to stop.
  // The demuxer_ may have been destroyed in the pipeline thread.
  if (stopped_) {
    std::move(read_cb_).Run(DemuxerStream::kOk,
                            {DecoderBuffer::CreateEOSBuffer()});
    return;
  }

  // Buffers are only queued when there are no pending reads.
  DCHECK(buffer_queue_.empty() || read_queue_.empty());

  if (!buffer_queue_.empty()) {
    // Send the oldest buffer back.
    scoped_refptr<DecoderBuffer> buffer = buffer_queue_.front();
    if (buffer->end_of_stream()) {
      LOG(INFO) << "media_stack ProgressiveDemuxerStream::Read() EOS sent.";
    } else {
      // Do not pop EOS buffers, so that subsequent read requests also get EOS
      total_buffer_size_ -= buffer->data_size();
      --total_buffer_count_;
      buffer_queue_.pop_front();
    }
    std::move(read_cb_).Run(DemuxerStream::kOk, {buffer});
  } else {
    read_queue_.push_back(std::move(read_cb_));
  }
}

AudioDecoderConfig ProgressiveDemuxerStream::audio_decoder_config() {
  return demuxer_->AudioConfig();
}

VideoDecoderConfig ProgressiveDemuxerStream::video_decoder_config() {
  return demuxer_->VideoConfig();
}

Ranges<base::TimeDelta> ProgressiveDemuxerStream::GetBufferedRanges() {
  base::AutoLock auto_lock(lock_);
  return buffered_ranges_;
}

DemuxerStream::Type ProgressiveDemuxerStream::type() const {
  return type_;
}

void ProgressiveDemuxerStream::EnableBitstreamConverter() {
  NOTIMPLEMENTED();
}

void ProgressiveDemuxerStream::EnqueueBuffer(
    scoped_refptr<DecoderBuffer> buffer) {
  base::AutoLock auto_lock(lock_);
  if (stopped_) {
    // it's possible due to pipelining both downstream and within the
    // demuxer that several pipelined reads will be enqueuing packets
    // on a stopped stream. Drop them after complaining.
    LOG(WARNING) << "attempted to enqueue packet on stopped stream";
    return;
  }

  if (buffer->end_of_stream()) {
    LOG(INFO) << "media_stack ProgressiveDemuxerStream::EnqueueBuffer() EOS "
                 "received.";
  } else if (buffer->timestamp() != kNoTimestamp) {
    if (last_buffer_timestamp_ != kNoTimestamp &&
        last_buffer_timestamp_ < buffer->timestamp()) {
      buffered_ranges_.Add(last_buffer_timestamp_, buffer->timestamp());
    }
    last_buffer_timestamp_ = buffer->timestamp();
  } else {
    LOG(WARNING) << "bad timestamp info on enqueued buffer.";
  }

  // Check for any already waiting reads, service oldest read if there
  if (read_queue_.size()) {
    // assumption here is that buffer queue is empty
    DCHECK_EQ(buffer_queue_.size(), 0u);
    ReadCB read_cb = std::move(read_queue_.front());
    read_queue_.pop_front();
    std::move(read_cb).Run(DemuxerStream::kOk, {buffer});
  } else {
    // save the buffer for next read request
    buffer_queue_.push_back(buffer);
    if (!buffer->end_of_stream()) {
      total_buffer_size_ += buffer->data_size();
      ++total_buffer_count_;
    }
  }
}

base::TimeDelta ProgressiveDemuxerStream::GetLastBufferTimestamp() const {
  base::AutoLock auto_lock(lock_);
  return last_buffer_timestamp_;
}

size_t ProgressiveDemuxerStream::GetTotalBufferSize() const {
  base::AutoLock auto_lock(lock_);
  return total_buffer_size_;
}

size_t ProgressiveDemuxerStream::GetTotalBufferCount() const {
  base::AutoLock auto_lock(lock_);
  return total_buffer_count_;
}

void ProgressiveDemuxerStream::FlushBuffers() {
  base::AutoLock auto_lock(lock_);
  // TODO: Investigate if the following warning is valid.
  LOG_IF(WARNING, !read_queue_.empty()) << "Read requests should be empty";
  buffer_queue_.clear();
  total_buffer_size_ = 0;
  total_buffer_count_ = 0;
  last_buffer_timestamp_ = kNoTimestamp;
}

void ProgressiveDemuxerStream::Stop() {
  DCHECK(demuxer_->RunsTasksInCurrentSequence());
  base::AutoLock auto_lock(lock_);
  buffer_queue_.clear();
  total_buffer_size_ = 0;
  total_buffer_count_ = 0;
  last_buffer_timestamp_ = kNoTimestamp;
  // fulfill any pending callbacks with EOS buffers set to end timestamp
  for (ReadQueue::iterator it = read_queue_.begin(); it != read_queue_.end();
       ++it) {
    std::move(*it).Run(DemuxerStream::kOk, {DecoderBuffer::CreateEOSBuffer()});
  }
  read_queue_.clear();
  stopped_ = true;
}

//
// ProgressiveDemuxer
//
ProgressiveDemuxer::ProgressiveDemuxer(
    const scoped_refptr<base::SequencedTaskRunner>& task_runner,
    DataSource* data_source,
    MediaLog* const media_log)
    : task_runner_(task_runner),
      host_(NULL),
      blocking_thread_("ProgDemuxerBlk"),
      data_source_(data_source),
      media_log_(media_log),
      stopped_(false),
      flushing_(false),
      audio_reached_eos_(false),
      video_reached_eos_(false) {
  DCHECK(task_runner_);
  DCHECK(data_source_);
  DCHECK(media_log_);
  reader_ = new DataSourceReader();
  reader_->SetDataSource(data_source_);
}

ProgressiveDemuxer::~ProgressiveDemuxer() {
  // Explicitly stop |blocking_thread_| to ensure that it stops before the
  // destructing of any other members.
  blocking_thread_.Stop();
}

void ProgressiveDemuxer::Initialize(DemuxerHost* host,
                                    PipelineStatusCallback status_cb) {
  DCHECK(RunsTasksInCurrentSequence());
  DCHECK(reader_);
  DCHECK(!parser_);

  LOG(INFO) << "this is a PROGRESSIVE playback.";

  host_ = host;

  // create audio and video demuxer stream objects
  audio_demuxer_stream_.reset(
      new ProgressiveDemuxerStream(this, DemuxerStream::AUDIO));
  video_demuxer_stream_.reset(
      new ProgressiveDemuxerStream(this, DemuxerStream::VIDEO));

  // start the blocking thread and have it download and parse the media config
  if (!blocking_thread_.Start()) {
    std::move(status_cb).Run(DEMUXER_ERROR_COULD_NOT_PARSE);
    return;
  }

  blocking_thread_.task_runner()->PostTask(
      FROM_HERE, base::BindOnce(&ProgressiveDemuxer::ParseConfigBlocking,
                                base::Unretained(this), std::move(status_cb)));
}

void ProgressiveDemuxer::ParseConfigBlocking(PipelineStatusCallback status_cb) {
  DCHECK(blocking_thread_.task_runner()->RunsTasksInCurrentSequence());
  DCHECK(!parser_);

  // construct stream parser with error callback
  PipelineStatus status =
      ProgressiveParser::Construct(reader_, &parser_, media_log_);
  // if we can't construct a parser for this stream it's a fatal error, return
  // false so ParseConfigDone will notify the caller to Initialize() via
  // status_cb.
  if (!parser_ || status != PIPELINE_OK) {
    DCHECK(!parser_);
    DCHECK_NE(status, PIPELINE_OK);
    if (status == PIPELINE_OK) {
      status = DEMUXER_ERROR_COULD_NOT_PARSE;
    }
    ParseConfigDone(std::move(status_cb), status);
    return;
  }

  // instruct the parser to extract audio and video config from the file
  if (!parser_->ParseConfig()) {
    ParseConfigDone(std::move(status_cb), DEMUXER_ERROR_COULD_NOT_PARSE);
    return;
  }

  // make sure we got a valid and complete configuration
  if (!parser_->IsConfigComplete()) {
    ParseConfigDone(std::move(status_cb), DEMUXER_ERROR_COULD_NOT_PARSE);
    return;
  }

  // IsConfigComplete() should guarantee we know the duration
  DCHECK(parser_->Duration() != kInfiniteDuration);
  host_->SetDuration(parser_->Duration());

  // successful parse of config data, inform the nonblocking demuxer thread
  DCHECK_EQ(status, PIPELINE_OK);
  ParseConfigDone(std::move(status_cb), PIPELINE_OK);
}

void ProgressiveDemuxer::ParseConfigDone(PipelineStatusCallback status_cb,
                                         PipelineStatus status) {
  DCHECK(blocking_thread_.task_runner()->RunsTasksInCurrentSequence());

  if (HasStopCalled()) {
    return;
  }

  // if the blocking parser thread cannot parse config we're done.
  if (status != PIPELINE_OK) {
    std::move(status_cb).Run(status);
    return;
  }
  DCHECK(parser_);
  // start downloading data
  Request(DemuxerStream::AUDIO);

  std::move(status_cb).Run(PIPELINE_OK);
}

void ProgressiveDemuxer::Request(DemuxerStream::Type type) {
  if (!blocking_thread_.task_runner()->RunsTasksInCurrentSequence()) {
    blocking_thread_.task_runner()->PostTask(
        FROM_HERE, base::BindRepeating(&ProgressiveDemuxer::Request,
                                       base::Unretained(this), type));
    return;
  }

  DCHECK(!requested_au_) << "overlapping requests not supported!";
  flushing_ = false;
  // Ask parser for next AU
  scoped_refptr<AvcAccessUnit> au = parser_->GetNextAU(type);
  // fatal parsing error returns NULL or malformed AU
  if (!au || !au->IsValid()) {
    if (!HasStopCalled()) {
      LOG(ERROR) << "got back bad AU from parser";
      host_->OnDemuxerError(DEMUXER_ERROR_COULD_NOT_PARSE);
    }
    return;
  }

  // make sure we got back an AU of the correct type
  DCHECK(au->GetType() == type);

  // don't issue allocation requests for EOS AUs
  if (au->IsEndOfStream()) {
    // enqueue EOS buffer with correct stream
    scoped_refptr<DecoderBuffer> eos_buffer = DecoderBuffer::CreateEOSBuffer();
    if (type == DemuxerStream::AUDIO) {
      audio_reached_eos_ = true;
      audio_demuxer_stream_->EnqueueBuffer(eos_buffer);
    } else if (type == DemuxerStream::VIDEO) {
      video_reached_eos_ = true;
      video_demuxer_stream_->EnqueueBuffer(eos_buffer);
    }
    IssueNextRequest();
    return;
  }

  // enqueue the request
  requested_au_ = au;

  AllocateBuffer();
}

void ProgressiveDemuxer::AllocateBuffer() {
  DCHECK(requested_au_);

  if (HasStopCalled()) {
    return;
  }

  if (requested_au_) {
    size_t total_buffer_size = audio_demuxer_stream_->GetTotalBufferSize() +
                               video_demuxer_stream_->GetTotalBufferSize();
    size_t total_buffer_count = audio_demuxer_stream_->GetTotalBufferCount() +
                                video_demuxer_stream_->GetTotalBufferCount();
    // Only sdr video is supported in progressive mode.
    const int kBitDepth = 8;
    int progressive_budget = SbMediaGetProgressiveBufferBudget(
        MediaVideoCodecToSbMediaVideoCodec(VideoConfig().codec()),
        VideoConfig().visible_rect().size().width(),
        VideoConfig().visible_rect().size().height(), kBitDepth);
    base::TimeDelta progressive_duration_cap = base::Microseconds(
        SbMediaGetBufferGarbageCollectionDurationThreshold());
    const int kEstimatedBufferCountPerSeconds = 70;
    int progressive_buffer_count_cap =
        progressive_duration_cap.InSeconds() * kEstimatedBufferCountPerSeconds;
    if (total_buffer_size >= static_cast<size_t>(progressive_budget) ||
        total_buffer_count >
            static_cast<size_t>(progressive_buffer_count_cap)) {
      // Retry after 100 milliseconds.
      const base::TimeDelta kDelay = base::Milliseconds(100);
      blocking_thread_.task_runner()->PostDelayedTask(
          FROM_HERE,
          base::BindRepeating(&ProgressiveDemuxer::AllocateBuffer,
                              base::Unretained(this)),
          kDelay);
      return;
    }

    scoped_refptr<DecoderBuffer> decoder_buffer(
        new DecoderBuffer(requested_au_->GetMaxSize()));
    DCHECK(decoder_buffer);
    decoder_buffer->set_is_key_frame(requested_au_->IsKeyframe());
    Download(decoder_buffer);
  }
}

void ProgressiveDemuxer::Download(scoped_refptr<DecoderBuffer> buffer) {
  DCHECK(base::SequencedTaskRunner::GetCurrentDefault()
             ->RunsTasksInCurrentSequence());
  // We need a requested_au_ or to have canceled this request and
  // are buffering to a new location for this to make sense
  DCHECK(requested_au_);

  // do nothing if stopped
  if (HasStopCalled()) {
    LOG(INFO) << "aborting download task, stopped";
    return;
  }

  // Flushing is a signal to restart the request->download cycle with
  // a new request. Drop current request and issue a new one.
  // flushing_ will be reset by the next call to RequestTask()
  if (flushing_) {
    LOG(INFO) << "skipped AU download due to flush";
    requested_au_ = nullptr;
    IssueNextRequest();
    return;
  }

  if (!requested_au_->Read(reader_.get(), buffer.get())) {
    LOG(ERROR) << "au read failed";
    host_->OnDemuxerError(PIPELINE_ERROR_READ);
    return;
  }

  // copy timestamp and duration values
  buffer->set_timestamp(requested_au_->GetTimestamp());
  buffer->set_duration(requested_au_->GetDuration());

  // enqueue buffer into appropriate stream
  if (requested_au_->GetType() == DemuxerStream::AUDIO) {
    audio_demuxer_stream_->EnqueueBuffer(buffer);
  } else if (requested_au_->GetType() == DemuxerStream::VIDEO) {
    video_demuxer_stream_->EnqueueBuffer(buffer);
  } else {
    NOTREACHED() << "invalid buffer type enqueued";
  }

  // finished with this au, deref
  requested_au_ = nullptr;

  // Calculate total range of buffered data for both audio and video.
  Ranges<base::TimeDelta> buffered(
      audio_demuxer_stream_->GetBufferedRanges().IntersectionWith(
          video_demuxer_stream_->GetBufferedRanges()));
  // Notify host of each disjoint range.
  host_->OnBufferedTimeRangesChanged(buffered);

  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindRepeating(&ProgressiveDemuxer::IssueNextRequest,
                                     base::Unretained(this)));
}

void ProgressiveDemuxer::IssueNextRequest() {
  DCHECK(!requested_au_);
  // if we're stopped don't download anymore
  if (HasStopCalled()) {
    LOG(INFO) << "stopped so request loop is stopping";
    return;
  }

  DemuxerStream::Type type = DemuxerStream::UNKNOWN;
  // if we have eos in one or both buffers the decision is easy
  if (audio_reached_eos_ || video_reached_eos_) {
    if (audio_reached_eos_) {
      if (video_reached_eos_) {
        // both are true, issue no more requests!
        LOG(INFO) << "both streams at EOS, request loop stopping";
        return;
      } else {
        // audio is at eos, video isn't, get more video
        type = DemuxerStream::VIDEO;
      }
    } else {
      // audio is not at eos, video is, get more audio
      type = DemuxerStream::AUDIO;
    }
  } else {
    // priority order for figuring out what to download next
    base::TimeDelta audio_stamp =
        audio_demuxer_stream_->GetLastBufferTimestamp();
    base::TimeDelta video_stamp =
        video_demuxer_stream_->GetLastBufferTimestamp();
    // if the audio demuxer stream is empty, always fill it first
    if (audio_stamp == kNoTimestamp) {
      type = DemuxerStream::AUDIO;
    } else if (video_stamp == kNoTimestamp) {
      // the video demuxer stream is empty, we need data for it
      type = DemuxerStream::VIDEO;
    } else if (video_stamp < audio_stamp) {
      // video is earlier, fill it first
      type = DemuxerStream::VIDEO;
    } else {
      type = DemuxerStream::AUDIO;
    }
  }
  DCHECK_NE(type, DemuxerStream::UNKNOWN);
  // We cannot call Request() directly even if this function is also run on
  // |blocking_thread_| as otherwise it is possible that this function is
  // running in a tight loop and seek or stop request has no chance to kick in.
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindRepeating(&ProgressiveDemuxer::Request,
                                     base::Unretained(this), type));
}

void ProgressiveDemuxer::Stop() {
  DCHECK(RunsTasksInCurrentSequence());
  // set our internal stop flag, to not treat read failures as
  // errors anymore but as a natural part of stopping
  {
    base::AutoLock auto_lock(lock_for_stopped_);
    stopped_ = true;
  }

  // stop the reader, which will stop the datasource and call back
  reader_->Stop();
}

bool ProgressiveDemuxer::HasStopCalled() {
  base::AutoLock auto_lock(lock_for_stopped_);
  return stopped_;
}

void ProgressiveDemuxer::Seek(base::TimeDelta time, PipelineStatusCallback cb) {
  blocking_thread_.task_runner()->PostTask(
      FROM_HERE,
      base::BindOnce(&ProgressiveDemuxer::SeekTask, base::Unretained(this),
                     time, BindPostTaskToCurrentDefault(std::move(cb))));
}

// runs on blocking thread
void ProgressiveDemuxer::SeekTask(base::TimeDelta time,
                                  PipelineStatusCallback cb) {
  LOG(ERROR) << base::StringPrintf("Cobalt seek to: %" PRId64 " ms",
                                   time.InMilliseconds());
  // clear any enqueued buffers on demuxer streams
  audio_demuxer_stream_->FlushBuffers();
  video_demuxer_stream_->FlushBuffers();
  // advance parser to new timestamp
  if (!parser_->SeekTo(time)) {
    std::move(cb).Run(PIPELINE_ERROR_READ);
    return;
  }
  // if both streams had finished downloading, we need to restart the request
  bool issue_new_request = audio_reached_eos_ && video_reached_eos_;
  audio_reached_eos_ = false;
  video_reached_eos_ = false;
  flushing_ = true;
  std::move(cb).Run(PIPELINE_OK);
  if (issue_new_request) {
    Request(DemuxerStream::AUDIO);
  }
}

std::vector<DemuxerStream*> ProgressiveDemuxer::GetAllStreams() {
  return std::vector<DemuxerStream*>(
      {audio_demuxer_stream_.get(), video_demuxer_stream_.get()});
}

base::TimeDelta ProgressiveDemuxer::GetStartTime() const {
  // we always assume a start time of 0
  return base::TimeDelta();
}

const AudioDecoderConfig& ProgressiveDemuxer::AudioConfig() {
  return parser_->AudioConfig();
}

const VideoDecoderConfig& ProgressiveDemuxer::VideoConfig() {
  return parser_->VideoConfig();
}

}  // namespace media
