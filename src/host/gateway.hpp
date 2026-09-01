// Host 常驻 VMCI 网关接口。
#pragma once

#include <memory>
#include <stop_token>

#include "satsuma/core/config.hpp"
#include "satsuma/core/vmci.hpp"

namespace satsuma::host {

class GatewayStateLock;

class Gateway {
public:
    explicit Gateway(LabConfig config);
    ~Gateway();

    Gateway(const Gateway&) = delete;
    Gateway& operator=(const Gateway&) = delete;

    // 在当前线程监听 VMCI，直到收到停止请求。
    void run(std::stop_token stop_token = {});

    // 供 inproc 单元测试绕过 VMCI 驱动直接验证协议事务。
    [[nodiscard]] transport::Message handle(const transport::Message& request);

private:
    LabConfig config_;
    std::unique_ptr<GatewayStateLock> state_lock_;
};

}  // namespace satsuma::host
