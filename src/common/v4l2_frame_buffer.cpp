#include "common/v4l2_frame_buffer.h"
#include "common/logging.h"

#include <third_party/libyuv/include/libyuv.h>
#if defined(USE_LIBARGUS_CAPTURE)
#include "common/nv_utils.h"
#endif

#include <chrono>

// Aligning pointer to 64 bytes for improved performance, e.g. use SIMD.
static const int kBufferAlignment = 64;

rtc::scoped_refptr<V4L2FrameBuffer> V4L2FrameBuffer::Create(int width, int height, int size,
                                                            uint32_t format) {
    return rtc::make_ref_counted<V4L2FrameBuffer>(width, height, size, format);
}

rtc::scoped_refptr<V4L2FrameBuffer> V4L2FrameBuffer::Create(int width, int height,
                                                            V4L2Buffer buffer) {
    return rtc::make_ref_counted<V4L2FrameBuffer>(width, height, buffer);
}

V4L2FrameBuffer::V4L2FrameBuffer(int width, int height, uint32_t format, int size, uint32_t flags,
                                 timeval timestamp)
    : width_(width),
      height_(height),
      format_(format),
      size_(size),
      flags_(flags),
      timestamp_(timestamp),
      buffer_({}),
      data_(nullptr) {}

V4L2FrameBuffer::V4L2FrameBuffer(int width, int height, V4L2Buffer buffer)
    : V4L2FrameBuffer(width, height, buffer.pix_fmt, buffer.length, buffer.flags,
                      buffer.timestamp) {
    buffer_ = buffer;
}

V4L2FrameBuffer::V4L2FrameBuffer(int width, int height, int size, uint32_t format)
    : V4L2FrameBuffer(width, height, format, size, 0, {0, 0}) {
    data_.reset(static_cast<uint8_t *>(webrtc::AlignedMalloc(size_, kBufferAlignment)));
}

V4L2FrameBuffer::~V4L2FrameBuffer() {}

webrtc::VideoFrameBuffer::Type V4L2FrameBuffer::type() const { return Type::kNative; }

int V4L2FrameBuffer::width() const { return width_; }
int V4L2FrameBuffer::height() const { return height_; }
uint32_t V4L2FrameBuffer::format() const { return format_; }
uint32_t V4L2FrameBuffer::size() const { return size_; }
uint32_t V4L2FrameBuffer::flags() const { return flags_; }
timeval V4L2FrameBuffer::timestamp() const { return timestamp_; }

rtc::scoped_refptr<webrtc::I420BufferInterface> V4L2FrameBuffer::ToI420() {
    rtc::scoped_refptr<webrtc::I420Buffer> i420_buffer(webrtc::I420Buffer::Create(width_, height_));
    i420_buffer->InitializeData();

    const uint8_t *src = static_cast<const uint8_t *>(Data());
    
    // Check if this is multiplanar buffer
    bool is_multiplanar = buffer_.plane_start[0] != nullptr;
    
    if (is_multiplanar) {
        DEBUG_PRINT("Multiplanar conversion: format=%u, plane0=%p (%u bytes), plane1=%p (%u bytes)", 
                   format_, buffer_.plane_start[0], buffer_.plane_bytesused[0],
                   buffer_.plane_start[1], buffer_.plane_bytesused[1]);
    }

    if (format_ == V4L2_PIX_FMT_YUV420) {
        if (is_multiplanar && buffer_.plane_start[1] != nullptr) {
            // Multiplanar I420: Y, U, V in separate planes with potential stride/padding
            const uint8_t *src_y = static_cast<const uint8_t *>(buffer_.plane_start[0]);
            const uint8_t *src_u = static_cast<const uint8_t *>(buffer_.plane_start[1]);
            const uint8_t *src_v = buffer_.plane_start[2] != nullptr ? 
                                  static_cast<const uint8_t *>(buffer_.plane_start[2]) : nullptr;
            
            // Calculate stride from bytesused/height
            int src_stride_y = buffer_.plane_bytesused[0] > 0 ? 
                              buffer_.plane_bytesused[0] / height_ : width_;
            int src_stride_u = buffer_.plane_bytesused[1] > 0 ? 
                              buffer_.plane_bytesused[1] / (height_ / 2) : (width_ / 2);
            int src_stride_v = buffer_.plane_bytesused[2] > 0 ? 
                              buffer_.plane_bytesused[2] / (height_ / 2) : (width_ / 2);
            
            // Use libyuv to handle stride properly
            if (src_v != nullptr) {
                libyuv::I420Copy(src_y, src_stride_y, src_u, src_stride_u, src_v, src_stride_v,
                                i420_buffer->MutableDataY(), i420_buffer->StrideY(),
                                i420_buffer->MutableDataU(), i420_buffer->StrideU(),
                                i420_buffer->MutableDataV(), i420_buffer->StrideV(),
                                width_, height_);
            }
        } else {
            // Single-plane I420: Y, U, V contiguous
            int y_size = width_ * height_;
            int uv_size = (width_ / 2) * (height_ / 2);
            
            memcpy(i420_buffer->MutableDataY(), src, y_size);
            memcpy(i420_buffer->MutableDataU(), src + y_size, uv_size);
            memcpy(i420_buffer->MutableDataV(), src + y_size + uv_size, uv_size);
        }
    } else if (format_ == V4L2_PIX_FMT_NV12) {
        // NV12 format: Y plane, then interleaved UV plane
        const uint8_t *src_y;
        const uint8_t *src_uv;
        int src_stride_y;
        int src_stride_uv;
        
        if (is_multiplanar && buffer_.plane_start[1] != nullptr) {
            // Multiplanar NV12: separate Y and UV planes
            src_y = static_cast<const uint8_t *>(buffer_.plane_start[0]);
            src_uv = static_cast<const uint8_t *>(buffer_.plane_start[1]);
            
            // Calculate stride from bytesused/height (accounting for padding)
            src_stride_y = buffer_.plane_bytesused[0] > 0 ? 
                          buffer_.plane_bytesused[0] / height_ : width_;
            src_stride_uv = buffer_.plane_bytesused[1] > 0 ? 
                           buffer_.plane_bytesused[1] / (height_ / 2) : width_;
        } else {
            // Single-plane NV12: contiguous Y then UV
            src_y = src;
            src_uv = src + (width_ * height_);
            src_stride_y = width_;
            src_stride_uv = width_;
        }
        
        libyuv::NV12ToI420(src_y, src_stride_y, src_uv, src_stride_uv,
                          i420_buffer->MutableDataY(), i420_buffer->StrideY(),
                          i420_buffer->MutableDataU(), i420_buffer->StrideU(),
                          i420_buffer->MutableDataV(), i420_buffer->StrideV(),
                          width_, height_);
    } else if (format_ == V4L2_PIX_FMT_NV21) {
        // NV21 format: Y plane, then interleaved VU plane
        const uint8_t *src_y;
        const uint8_t *src_vu;
        int src_stride_y;
        int src_stride_vu;
        
        if (is_multiplanar && buffer_.plane_start[1] != nullptr) {
            // Multiplanar NV21: separate Y and VU planes
            src_y = static_cast<const uint8_t *>(buffer_.plane_start[0]);
            src_vu = static_cast<const uint8_t *>(buffer_.plane_start[1]);
            
            // Calculate stride from bytesused/height
            src_stride_y = buffer_.plane_bytesused[0] > 0 ? 
                          buffer_.plane_bytesused[0] / height_ : width_;
            src_stride_vu = buffer_.plane_bytesused[1] > 0 ? 
                           buffer_.plane_bytesused[1] / (height_ / 2) : width_;
        } else {
            // Single-plane NV21: contiguous Y then VU
            src_y = src;
            src_vu = src + (width_ * height_);
            src_stride_y = width_;
            src_stride_vu = width_;
        }
        
        libyuv::NV21ToI420(src_y, src_stride_y, src_vu, src_stride_vu,
                          i420_buffer->MutableDataY(), i420_buffer->StrideY(),
                          i420_buffer->MutableDataU(), i420_buffer->StrideU(),
                          i420_buffer->MutableDataV(), i420_buffer->StrideV(),
                          width_, height_);
    } else if (format_ == V4L2_PIX_FMT_UYVY) {
        // UYVY format: Packed YUV 4:2:2 (U0 Y0 V0 Y1)
        // stride is 2 bytes per pixel (width * 2 for UYVY)
        int src_stride_uyvy = width_ * 2;
        
        libyuv::UYVYToI420(src, src_stride_uyvy,
                          i420_buffer->MutableDataY(), i420_buffer->StrideY(),
                          i420_buffer->MutableDataU(), i420_buffer->StrideU(),
                          i420_buffer->MutableDataV(), i420_buffer->StrideV(),
                          width_, height_);
    } else if (format_ == V4L2_PIX_FMT_YUYV) {
        // YUYV format: Packed YUV 4:2:2 (Y0 U0 Y1 V0)
        int src_stride_yuyv = width_ * 2;
        
        libyuv::YUY2ToI420(src, src_stride_yuyv,
                          i420_buffer->MutableDataY(), i420_buffer->StrideY(),
                          i420_buffer->MutableDataU(), i420_buffer->StrideU(),
                          i420_buffer->MutableDataV(), i420_buffer->StrideV(),
                          width_, height_);
    } else {
#if defined(USE_LIBARGUS_CAPTURE)
        if (NvUtils::ConvertToI420(buffer_.dmafd, i420_buffer->MutableDataY(), size_, width_,
                                   height_) < 0) {
            ERROR_PRINT("NvUtils ConvertToI420 Failed");
        }
#else
        if (libyuv::ConvertToI420(src, size_, i420_buffer->MutableDataY(), i420_buffer->StrideY(),
                                  i420_buffer->MutableDataU(), i420_buffer->StrideU(),
                                  i420_buffer->MutableDataV(), i420_buffer->StrideV(), 0, 0, width_,
                                  height_, width_, height_, libyuv::kRotate0, format_) < 0) {
            ERROR_PRINT("libyuv ConvertToI420 Failed");
        }
#endif
    }

    return i420_buffer;
}

V4L2Buffer V4L2FrameBuffer::GetRawBuffer() { return buffer_; }

const void *V4L2FrameBuffer::Data() const { return data_ ? data_.get() : buffer_.start; }

uint8_t *V4L2FrameBuffer::MutableData() {
    if (!data_) {
        throw std::runtime_error(
            "MutableData() is not supported for frames directly created from V4L2 buffers. Use "
            "Clone() to create an owning (writable) copy before calling MutableData().");
    }
    return data_.get();
}

int V4L2FrameBuffer::GetDmaFd() const { return buffer_.dmafd; }

void V4L2FrameBuffer::SetDmaFd(int fd) {
    if (fd > 0) {
        buffer_.dmafd = fd;
    }
}

void V4L2FrameBuffer::SetTimestamp(timeval timestamp) { timestamp_ = timestamp; }

rtc::scoped_refptr<V4L2FrameBuffer> V4L2FrameBuffer::Clone() const {
    auto clone = rtc::make_ref_counted<V4L2FrameBuffer>(width_, height_, size_, format_);

    memcpy(clone->MutableData(), Data(), size_);

    clone->SetDmaFd(buffer_.dmafd);
    clone->flags_ = flags_;
    clone->timestamp_ = timestamp_;

    return clone;
}
