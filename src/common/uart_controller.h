#ifndef UART_CONTROLLER_H_
#define UART_CONTROLLER_H_

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

class UartController {
  public:
    static std::shared_ptr<UartController> Create(const std::string &device, int baud_rate);

    UartController(const std::string &device, int baud_rate);
    ~UartController();

    bool Init();
    void SendCommand(int throttle, int steer);
    void Stop();
    bool IsConnected() const { return connected_.load(); }

  private:
    bool OpenPort();
    void ClosePort();
    bool ConfigurePort();

    std::string device_;
    int baud_rate_;
    int fd_;
    uint16_t seq_;

    std::mutex mutex_;
    std::atomic<bool> connected_;
};

#endif // UART_CONTROLLER_H_
