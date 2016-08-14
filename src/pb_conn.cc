#include "pb_conn.h"
#include "pink_define.h"
#include "worker_thread.h"
#include "xdebug.h"

#include <string>

namespace pink {

PbConn::PbConn(const int fd, const std::string &ip_port) :
  PinkConn(fd, ip_port),
  header_len_(-1),
  cur_pos_(0),
  rbuf_len_(0),
  connStatus_(kHeader),
  wbuf_len_(0),
  wbuf_pos_(0),
  wbuf_size_(PB_MAX_MESSAGE)
{
  rbuf_ = (char *)malloc(sizeof(char) * PB_MAX_MESSAGE);
  wbuf_ = (char *)malloc(sizeof(char) * PB_MAX_MESSAGE);
}

PbConn::~PbConn()
{
  free(rbuf_);
  free(wbuf_);
}

ReadStatus PbConn::GetRequest()
{
  ssize_t nread = read(fd(), rbuf_ + rbuf_len_, PB_MAX_MESSAGE - rbuf_len_);

  if (nread == -1) {
    if (errno == EAGAIN) {
      return kReadHalf;
    } else {
      return kReadError;
    }
  } else if (nread == 0) {
    return kReadClose;
  }

  if (nread) {
    uint32_t integer = 0;
    bool flag = true;
    rbuf_len_ += nread;
    while (flag) {
      switch (connStatus_) {
      case kHeader:
        if (rbuf_len_ - cur_pos_ >= COMMAND_HEADER_LENGTH) {
          memcpy((char *)(&integer), rbuf_ + cur_pos_, sizeof(uint32_t));
          header_len_ = ntohl(integer);
          log_info("Header_len %u", header_len_);
          connStatus_ = kPacket;
          cur_pos_ += COMMAND_HEADER_LENGTH;
        } else {
          flag = false;
        }
        break;
      case kPacket:
        if (rbuf_len_ >= cur_pos_ + header_len_) {
          cur_pos_ += header_len_;
          log_info("k Packet cur_pos_ %d rbuf_len_ %d", cur_pos_, rbuf_len_);
          connStatus_ = kComplete;
        } else {
          flag = false;
        }
        break;
      case kComplete:
        DealMessage();
        if (!BuildObuf().ok()) {
          return kFullError;
        }

        connStatus_ = kHeader;
        log_info("%d %d", cur_pos_, rbuf_len_);
        if (cur_pos_ == rbuf_len_) {
          cur_pos_ = 0;
          rbuf_len_ = 0;
          return kReadAll;
        }
        /*
         * Add this switch case just for delete compile warning
         */
      case kBuildObuf:
        break;

      case kWriteObuf:
        break;
      }
    }

    // memmov the remain part to begin if needed
    if (cur_pos_ != 0) {
      memmove(rbuf_, rbuf_ + cur_pos_, rbuf_len_ - cur_pos_);
      rbuf_len_ -= cur_pos_;
      cur_pos_ = 0;
    }
  }
  return kReadHalf;
}

WriteStatus PbConn::SendReply()
{
  //BuildObuf();
  ssize_t nwritten = 0;
  while (wbuf_len_ > 0) {
    nwritten = write(fd(), wbuf_ + wbuf_pos_, wbuf_len_ - wbuf_pos_);
    if (nwritten <= 0) {
      break;
    }
    wbuf_pos_ += nwritten;
    if (wbuf_pos_ == wbuf_len_) {
      wbuf_len_ = 0;
      wbuf_pos_ = 0;
    }
  }
  if (nwritten == -1) {
    if (errno == EAGAIN) {
      return kWriteHalf;
    } else {
      // Here we should close the connection
      return kWriteError;
    }
  }
  if (wbuf_len_ == 0) {
    return kWriteAll;
  } else {
    return kWriteHalf;
  }
}

// Simply double wbuf but not too large
bool PbConn::ExpandWbuf(uint32_t claim_size) {
  uint32_t alloc_size = wbuf_size_;
  for (; alloc_size < claim_size; alloc_size <<= 1);
  
  if (alloc_size >= PB_MAX_MESSAGE * 32) {
    return false;
  }

  wbuf_size_ = alloc_size;
  wbuf_ = (char*)realloc(wbuf_, wbuf_size_);
  return true;
}

Status PbConn::BuildObuf()
{
  uint32_t byte_size = res_->ByteSize();
  if (!ExpandWbuf(wbuf_len_ + byte_size + COMMAND_HEADER_LENGTH)) {
    return Status::IOError("wbuf will overflow");
  }

  uint32_t u;
  u = htonl(byte_size);
  memcpy(wbuf_ + wbuf_len_, &u, sizeof(uint32_t));
  wbuf_len_ += COMMAND_HEADER_LENGTH;
  res_->SerializeToArray(wbuf_ + wbuf_len_, byte_size);
  wbuf_len_ += byte_size;

  return Status::OK();
}

}
