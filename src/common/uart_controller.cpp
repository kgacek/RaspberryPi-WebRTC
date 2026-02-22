#include "uart_controller.h"

#include "common/logging.h"

#include <cstring>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

std::shared_ptr<UartController> UartController::Create(const std::string &device, int baud_rate) {
    auto controller = std::make_shared<UartController>(device, baud_rate);
    if (!controller->Init()) {
        WARN_PRINT("Failed to initialize UART controller");
    }
    return controller;
}

UartController::UartController(const std::string &device, int baud_rate)
    : device_(device),
      baud_rate_(baud_rate),
      fd_(-1),
      seq_(0),
      connected_(false) {}

UartController::~UartController() { Stop(); }

bool UartController::Init() {
    if (!OpenPort()) {
        return false;
    }

    if (!ConfigurePort()) {
        ClosePort();
        return false;
    }

    connected_ = true;
    INFO_PRINT("UART initialized: %s @ %d", device_.c_str(), baud_rate_);
    return true;
}

bool UartController::OpenPort() {
    fd_ = open(device_.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd_ < 0) {
        ERROR_PRINT("Failed to open UART device: %s", device_.c_str());
        return false;
    }
    return true;
}

bool UartController::ConfigurePort() {
    struct termios tty;

    if (tcgetattr(fd_, &tty) != 0) {
        ERROR_PRINT("tcgetattr failed: %s", strerror(errno));
        return false;
    }

    // Set baud rate (support common rates)
    speed_t speed;
    switch (baud_rate_) {
    case 9600:
        speed = B9600;
        break;
    case 19200:
        speed = B19200;
        break;
    case 38400:
        speed = B38400;
        break;
    case 57600:
        speed = B57600;
        break;
    case 115200:
        speed = B115200;
        break;
    case 230400:
        speed = B230400;
        break;
    default:
        ERROR_PRINT("Unsupported baud rate: %d", baud_rate_);
        return false;
    }

    cfsetospeed(&tty, speed);
    cfsetispeed(&tty, speed);

    // 8N1 mode
    tty.c_cflag &= ~PARENB;        // No parity
    tty.c_cflag &= ~CSTOPB;        // 1 stop bit
    tty.c_cflag &= ~CSIZE;
    tty.c_cflag |= CS8;            // 8 data bits

    tty.c_cflag &= ~CRTSCTS;       // No hardware flow control
    tty.c_cflag |= CREAD | CLOCAL; // Enable receiver

    // Raw mode
    tty.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
    tty.c_iflag &= ~(IXON | IXOFF | IXANY);
    tty.c_oflag &= ~OPOST;

    // Non-blocking reads
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 0;

    if (tcsetattr(fd_, TCSANOW, &tty) != 0) {
        ERROR_PRINT("tcsetattr failed: %s", strerror(errno));
        return false;
    }

    // Flush buffers
    tcflush(fd_, TCIOFLUSH);

    return true;
}

void UartController::SendCommand(int throttle, int steer) {
    if (!connected_ || fd_ < 0) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    seq_ = (seq_ + 1) & 0xFFFF;

    // Format: "T,{throttle},{steer},0,{seq}\n"
    char cmd[64];
    int len = snprintf(cmd, sizeof(cmd), "T,%d,%d,0,%u\n", throttle, steer, seq_);

    ssize_t written = write(fd_, cmd, len);
    if (written < 0) {
        ERROR_PRINT("UART write failed: %s", strerror(errno));
    }

    // Log non-zero commands occasionally
    if ((throttle != 0 || steer != 0) && (seq_ % 100 == 0)) {
        DEBUG_PRINT("UART TX: throttle=%d, steer=%d, seq=%u", throttle, steer, seq_);
    }
}

void UartController::Stop() {
    if (connected_) {
        SendCommand(0, 0); // Stop car
        usleep(100000);    // 100ms
    }
    ClosePort();
}

void UartController::ClosePort() {
    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
        connected_ = false;
        INFO_PRINT("UART closed");
    }
}
