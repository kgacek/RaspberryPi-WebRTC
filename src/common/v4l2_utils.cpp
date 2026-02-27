#include "v4l2_utils.h"
#include "common/logging.h"

#include <errno.h>
#include <fcntl.h>
#include <stdexcept>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

bool V4L2Util::IsSinglePlaneVideo(v4l2_capability *cap) {
    return (cap->capabilities & (V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_VIDEO_OUTPUT) &&
            (cap->capabilities & V4L2_CAP_STREAMING)) ||
           (cap->capabilities & V4L2_CAP_VIDEO_M2M);
}

bool V4L2Util::IsMultiPlaneVideo(v4l2_capability *cap) {
    return (cap->capabilities & (V4L2_CAP_VIDEO_CAPTURE_MPLANE | V4L2_CAP_VIDEO_OUTPUT_MPLANE) &&
            (cap->capabilities & V4L2_CAP_STREAMING)) ||
           (cap->capabilities & V4L2_CAP_VIDEO_M2M_MPLANE);
}

std::string V4L2Util::FourccToString(uint32_t fourcc) {
    int length = 4;
    std::string buf;
    buf.resize(length);

    for (int i = 0; i < length; i++) {
        const int c = fourcc & 0xff;
        buf[i] = c;
        fourcc >>= 8;
    }
    return buf;
}

int V4L2Util::OpenDevice(const char *file) {
    int fd = open(file, O_RDWR);
    if (fd < 0) {
        throw std::runtime_error(std::string("Failed to open v4l2 device: ") + file);
    }
    DEBUG_PRINT("Successfully opened file %s (fd: %d)", file, fd);
    return fd;
}

void V4L2Util::CloseDevice(int fd) {
    close(fd);
    DEBUG_PRINT("fd(%d) is closed!", fd);
}

bool V4L2Util::QueryCapabilities(int fd, v4l2_capability *cap) {
    if (ioctl(fd, VIDIOC_QUERYCAP, cap) < 0) {
        ERROR_PRINT("fd(%d) query capabilities: %s", fd, strerror(errno));
        return false;
    }
    return true;
}

bool V4L2Util::InitBuffer(int fd, V4L2BufferGroup *gbuffer, v4l2_buf_type type, v4l2_memory memory,
                          bool has_dmafd) {
    v4l2_capability cap = {};
    if (!V4L2Util::QueryCapabilities(fd, &cap)) {
        return false;
    }

    // Auto-detect multiplanar and adjust buffer type accordingly
    bool is_multiplanar = V4L2Util::IsMultiPlaneVideo(&cap);
    if (is_multiplanar) {
        if (type == V4L2_BUF_TYPE_VIDEO_CAPTURE) {
            type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
            DEBUG_PRINT("fd(%d) auto-detected multiplanar mode, using VIDEO_CAPTURE_MPLANE", fd);
        } else if (type == V4L2_BUF_TYPE_VIDEO_OUTPUT) {
            type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
            DEBUG_PRINT("fd(%d) auto-detected multiplanar mode, using VIDEO_OUTPUT_MPLANE", fd);
        }
    }

    DEBUG_PRINT("fd(%d) driver '%s' on card '%s' in %s mode", fd, cap.driver, cap.card,
                V4L2Util::IsSinglePlaneVideo(&cap)  ? "splane"
                : V4L2Util::IsMultiPlaneVideo(&cap) ? "mplane"
                                                    : "unknown");
    gbuffer->fd = fd;
    gbuffer->type = type;
    gbuffer->memory = memory;
    gbuffer->has_dmafd = has_dmafd;

    return true;
}

bool V4L2Util::DequeueBuffer(int fd, v4l2_buffer *buffer) {
    DEBUG_PRINT("fd(%d) dequeue: type=%u, memory=%u, length=%u, planes=%p", 
                fd, buffer->type, buffer->memory, buffer->length, buffer->m.planes);
    
    if (ioctl(fd, VIDIOC_DQBUF, buffer) < 0) {
        ERROR_PRINT("fd(%d) dequeue buffer: type=%u, length=%u, error=%s", 
                    fd, buffer->type, buffer->length, strerror(errno));
        return false;
    }
    return true;
}

bool V4L2Util::QueueBuffer(int fd, v4l2_buffer *buffer) {
    if (ioctl(fd, VIDIOC_QBUF, buffer) < 0) {
        ERROR_PRINT("fd(%d) queue buffer(%u): %s\n", fd, buffer->type, strerror(errno));
        return false;
    }
    return true;
}

bool V4L2Util::QueueBuffers(int fd, V4L2BufferGroup *gbuffer) {
    for (int i = 0; i < gbuffer->num_buffers; i++) {
        v4l2_buffer *inner = &gbuffer->buffers[i].inner;
        if (!V4L2Util::QueueBuffer(fd, inner)) {
            return false;
        }
    }
    return true;
}

std::unordered_set<std::string> V4L2Util::GetDeviceSupportedFormats(const char *file) {
    int fd = V4L2Util::OpenDevice(file);
    v4l2_fmtdesc fmtdesc = {0};
    fmtdesc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    std::unordered_set<std::string> formats;

    while (ioctl(fd, VIDIOC_ENUM_FMT, &fmtdesc) == 0) {
        auto pixel_format = V4L2Util::FourccToString(fmtdesc.pixelformat);
        formats.insert(pixel_format);
        fmtdesc.index++;
    }
    V4L2Util::CloseDevice(fd);

    return formats;
}

bool V4L2Util::SubscribeEvent(int fd, uint32_t type) {
    v4l2_event_subscription sub = {};
    sub.type = type;
    if (ioctl(fd, VIDIOC_SUBSCRIBE_EVENT, &sub) < 0) {
        ERROR_PRINT("fd(%d) does not support VIDIOC_SUBSCRIBE_EVENT(%d)", fd, type);
        return false;
    }
    return true;
}

bool V4L2Util::SetFps(int fd, v4l2_buf_type type, uint32_t fps) {
    struct v4l2_streamparm streamparms = {};
    streamparms.type = type;
    streamparms.parm.capture.timeperframe.numerator = 1;
    streamparms.parm.capture.timeperframe.denominator = fps;
    if (ioctl(fd, VIDIOC_S_PARM, &streamparms) < 0) {
        DEBUG_PRINT("fd(%d) set fps(%d): %s (may not be supported by this device)", fd, fps, strerror(errno));
        return false;
    }
    DEBUG_PRINT("fd(%d) set fps to %d", fd, fps);
    return true;
}

bool V4L2Util::SetFormat(int fd, V4L2BufferGroup *gbuffer, uint32_t width, uint32_t height,
                         uint32_t &pixel_format) {
    v4l2_format fmt = {};
    fmt.type = gbuffer->type;
    ioctl(fd, VIDIOC_G_FMT, &fmt);

    bool is_multiplanar = (gbuffer->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE ||
                          gbuffer->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);

    if (is_multiplanar) {
        DEBUG_PRINT("fd(%d) original formats: %s(%dx%d)", gbuffer->fd,
                    V4L2Util::FourccToString(fmt.fmt.pix_mp.pixelformat).c_str(), fmt.fmt.pix_mp.width,
                    fmt.fmt.pix_mp.height);

        if (width > 0 && height > 0) {
            fmt.fmt.pix_mp.width = width;
            fmt.fmt.pix_mp.height = height;
            fmt.fmt.pix_mp.pixelformat = pixel_format;
        }

        if (ioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
            ERROR_PRINT("fd(%d) set format(%s) : %s", fd,
                        V4L2Util::FourccToString(fmt.fmt.pix_mp.pixelformat).c_str(), strerror(errno));
            return false;
        }

        DEBUG_PRINT("fd(%d) latest format: %s(%dx%d) num_planes=%u", gbuffer->fd,
                    V4L2Util::FourccToString(fmt.fmt.pix_mp.pixelformat).c_str(), fmt.fmt.pix_mp.width,
                    fmt.fmt.pix_mp.height, fmt.fmt.pix_mp.num_planes);
        // use the  return format
        pixel_format = fmt.fmt.pix_mp.pixelformat;
        gbuffer->num_planes = fmt.fmt.pix_mp.num_planes;
        
        DEBUG_PRINT("fd(%d) stored num_planes=%u in gbuffer", gbuffer->fd, gbuffer->num_planes);

        if (fmt.fmt.pix_mp.width != width || fmt.fmt.pix_mp.height != height) {
            ERROR_PRINT("fd(%d) input size (%dx%d) doesn't match driver's output size (%dx%d): %s", fd,
                        width, height, fmt.fmt.pix_mp.width, fmt.fmt.pix_mp.height, strerror(EINVAL));
            throw std::runtime_error("the frame size doesn't match");
        }
    } else {
        DEBUG_PRINT("fd(%d) original formats: %s(%dx%d)", gbuffer->fd,
                    V4L2Util::FourccToString(fmt.fmt.pix.pixelformat).c_str(), fmt.fmt.pix.width,
                    fmt.fmt.pix.height);

        if (width > 0 && height > 0) {
            fmt.fmt.pix.width = width;
            fmt.fmt.pix.height = height;
            fmt.fmt.pix.pixelformat = pixel_format;
        }

        if (ioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
            ERROR_PRINT("fd(%d) set format(%s) : %s", fd,
                        V4L2Util::FourccToString(fmt.fmt.pix.pixelformat).c_str(), strerror(errno));
            return false;
        }

        DEBUG_PRINT("fd(%d) latest format: %s(%dx%d)", gbuffer->fd,
                    V4L2Util::FourccToString(fmt.fmt.pix.pixelformat).c_str(), fmt.fmt.pix.width,
                    fmt.fmt.pix.height);
        // use the  return format
        pixel_format = fmt.fmt.pix.pixelformat;
        gbuffer->num_planes = 1;

        if (fmt.fmt.pix.width != width || fmt.fmt.pix.height != height) {
            ERROR_PRINT("fd(%d) input size (%dx%d) doesn't match driver's output size (%dx%d): %s", fd,
                        width, height, fmt.fmt.pix.width, fmt.fmt.pix.height, strerror(EINVAL));
            throw std::runtime_error("the frame size doesn't match");
        }
    }

    return true;
}

bool V4L2Util::SetCtrl(int fd, uint32_t id, int32_t value) {
    v4l2_control ctrls = {};
    ctrls.id = id;
    ctrls.value = value;
    if (ioctl(fd, VIDIOC_S_CTRL, &ctrls) < 0) {
        DEBUG_PRINT("fd(%d) set ctrl(%d): %s (may not be supported)", fd, id, strerror(errno));
        return false;
    }
    DEBUG_PRINT("fd(%d) set ctrl(%d) = %d", fd, id, value);
    return true;
}

bool V4L2Util::SetExtCtrl(int fd, uint32_t id, int32_t value) {
    v4l2_ext_controls ctrls = {};
    v4l2_ext_control ctrl = {};

    /* set ctrls */
    ctrls.ctrl_class = V4L2_CTRL_CLASS_CODEC;
    ctrls.controls = &ctrl;
    ctrls.count = 1;

    /* set ctrl*/
    ctrl.id = id;
    ctrl.value = value;

    if (ioctl(fd, VIDIOC_S_EXT_CTRLS, &ctrls) < 0) {
        DEBUG_PRINT("fd(%d) set ext ctrl(%d): %s (may not be supported)", fd, id, strerror(errno));
        return false;
    }
    DEBUG_PRINT("fd(%d) set ext ctrl(%d) = %d", fd, id, value);
    return true;
}

bool V4L2Util::StreamOn(int fd, v4l2_buf_type type) {
    if (ioctl(fd, VIDIOC_STREAMON, &type) < 0) {
        ERROR_PRINT("fd(%d) turn on stream: %s", fd, strerror(errno));
        return false;
    }
    return true;
}

bool V4L2Util::StreamOff(int fd, v4l2_buf_type type) {
    if (ioctl(fd, VIDIOC_STREAMOFF, &type) < 0) {
        ERROR_PRINT("fd(%d) turn off stream: %s", fd, strerror(errno));
        return false;
    }
    return true;
}

void V4L2Util::UnMap(V4L2BufferGroup *gbuffer) {
    bool is_multiplanar = (gbuffer->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE ||
                          gbuffer->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
    
    for (int i = 0; i < gbuffer->num_buffers; i++) {
        if (gbuffer->buffers[i].dmafd > 0) {
            DEBUG_PRINT("close (%d) dmafd", gbuffer->buffers[i].dmafd);
            close(gbuffer->buffers[i].dmafd);
        }
        
        if (is_multiplanar) {
            // Unmap all planes for multiplanar
            for (uint32_t p = 0; p < gbuffer->num_planes; p++) {
                if (gbuffer->buffers[i].plane_start[p] != nullptr) {
                    DEBUG_PRINT("unmapped (%d) buffer %d plane %u", gbuffer->fd, i, p);
                    munmap(gbuffer->buffers[i].plane_start[p], gbuffer->buffers[i].plane_length[p]);
                    gbuffer->buffers[i].plane_start[p] = nullptr;
                }
            }
        } else {
            if (gbuffer->buffers[i].start != nullptr) {
                DEBUG_PRINT("unmapped (%d) buffers", gbuffer->fd);
                munmap(gbuffer->buffers[i].start, gbuffer->buffers[i].length);
                gbuffer->buffers[i].start = nullptr;
            }
        }
    }
}

bool V4L2Util::MMap(int fd, V4L2BufferGroup *gbuffer) {
    bool is_multiplanar = (gbuffer->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE ||
                          gbuffer->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
    
    for (int i = 0; i < gbuffer->num_buffers; i++) {
        V4L2Buffer *buffer = &gbuffer->buffers[i];
        v4l2_buffer *inner = &buffer->inner;
        inner->type = gbuffer->type;
        inner->memory = V4L2_MEMORY_MMAP;
        inner->index = i;
        
        if (is_multiplanar) {
            inner->length = gbuffer->num_planes;
            inner->m.planes = buffer->plane;
            // Initialize planes
            for (uint32_t j = 0; j < gbuffer->num_planes; j++) {
                buffer->plane[j].length = 0;
                buffer->plane[j].bytesused = 0;
            }
        } else {
            inner->length = 0;
        }

        if (ioctl(fd, VIDIOC_QUERYBUF, inner) < 0) {
            ERROR_PRINT("fd(%d) query buffer: %s", fd, strerror(errno));
            return false;
        }

        if (gbuffer->has_dmafd) {
            v4l2_exportbuffer expbuf = {};
            expbuf.type = gbuffer->type;
            expbuf.index = i;
            expbuf.plane = 0;
            if (ioctl(fd, VIDIOC_EXPBUF, &expbuf) < 0) {
                ERROR_PRINT("fd(%d) export buffer: %s", fd, strerror(errno));
                return false;
            }
            buffer->dmafd = expbuf.fd;
            DEBUG_PRINT("fd(%d) export dma at fd(%d)", gbuffer->fd, buffer->dmafd);
        }

        if (gbuffer->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE ||
            gbuffer->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
            // Multiplanar: map each plane separately
            for (uint32_t p = 0; p < gbuffer->num_planes; p++) {
                buffer->plane_length[p] = inner->m.planes[p].length;
                buffer->plane_start[p] = mmap(NULL, buffer->plane_length[p], 
                                             PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                                             inner->m.planes[p].m.mem_offset);
                
                if (MAP_FAILED == buffer->plane_start[p]) {
                    ERROR_PRINT("fd(%d) mmap failed for plane %u: %s", gbuffer->fd, p, strerror(errno));
                    // Unmap already mapped planes
                    for (uint32_t q = 0; q < p; q++) {
                        munmap(buffer->plane_start[q], buffer->plane_length[q]);
                        buffer->plane_start[q] = nullptr;
                    }
                    return false;
                }
                
                DEBUG_PRINT("fd(%d) mapped plane %u at %p (length: %u)", gbuffer->fd, p,
                           buffer->plane_start[p], buffer->plane_length[p]);
            }
            // For backward compatibility, set start to first plane
            buffer->start = buffer->plane_start[0];
            buffer->length = buffer->plane_length[0];
        } else if (gbuffer->type == V4L2_BUF_TYPE_VIDEO_CAPTURE ||
                   gbuffer->type == V4L2_BUF_TYPE_VIDEO_OUTPUT) {
            buffer->length = inner->length;
            buffer->start =
                mmap(NULL, buffer->length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, inner->m.offset);
        }

        if (MAP_FAILED == buffer->start) {
            perror("MAP FAILED");
            munmap(buffer->start, buffer->length);
            buffer->start = nullptr;
            return false;
        }

        DEBUG_PRINT("fd(%d) query buffer at %p (length: %d)", gbuffer->fd, buffer->start,
                    buffer->length);
    }

    return true;
}

bool V4L2Util::AllocateBuffer(int fd, V4L2BufferGroup *gbuffer, int num_buffers) {
    gbuffer->num_buffers = num_buffers;
    gbuffer->buffers.resize(num_buffers);

    v4l2_requestbuffers req = {};
    req.count = num_buffers;
    req.memory = gbuffer->memory;
    req.type = gbuffer->type;

    if (ioctl(fd, VIDIOC_REQBUFS, &req) < 0) {
        ERROR_PRINT("fd(%d) request buffer: %s", fd, strerror(errno));
        return false;
    }

    if (gbuffer->memory == V4L2_MEMORY_MMAP) {
        return MMap(fd, gbuffer);
    } else if (gbuffer->memory == V4L2_MEMORY_DMABUF) {
        bool is_multiplanar = (gbuffer->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE ||
                              gbuffer->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
        for (int i = 0; i < num_buffers; i++) {
            V4L2Buffer *buffer = &gbuffer->buffers[i];
            v4l2_buffer *inner = &buffer->inner;
            inner->type = gbuffer->type;
            inner->memory = V4L2_MEMORY_DMABUF;
            inner->index = i;
            
            if (is_multiplanar) {
                inner->length = gbuffer->num_planes;
                inner->m.planes = buffer->plane;
            } else {
                inner->length = 1;
            }
        }
    }

    return true;
}

bool V4L2Util::DeallocateBuffer(int fd, V4L2BufferGroup *gbuffer) {
    if (gbuffer->memory == V4L2_MEMORY_MMAP) {
        V4L2Util::UnMap(gbuffer);
    }

    v4l2_requestbuffers req = {};
    req.count = 0;
    req.memory = gbuffer->memory;
    req.type = gbuffer->type;

    if (ioctl(fd, VIDIOC_REQBUFS, &req) < 0) {
        ERROR_PRINT("fd(%d) request buffer: %s", fd, strerror(errno));
        return false;
    }

    gbuffer->fd = 0;
    gbuffer->has_dmafd = false;

    return true;
}
