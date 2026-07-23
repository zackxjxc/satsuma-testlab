// Satsuma 统一错误类型。
#pragma once

#include <stdexcept>
#include <string>

namespace satsuma {

// 表示可向 CLI 用户报告的业务或环境错误。
class Error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

}  // namespace satsuma
