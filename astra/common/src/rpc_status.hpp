// 功能: 将 gRPC 状态映射为有限本地错误类别, 防止远端错误正文进入诊断.
#pragma once

#include <astra/types.hpp>

#include <grpcpp/support/status.h>

namespace astra {
// 两个传输适配器共用稳定状态映射. 不读取远端 message/details, 暂态传输失败不能隔离身份.
// 会话回收只需要枚举, 不为取得 code 临时构造带 string 的 Error.
constexpr ErrorCode rpc_error_code(grpc::StatusCode status) noexcept {
    switch (status) {
    case grpc::StatusCode::UNAUTHENTICATED:
    case grpc::StatusCode::PERMISSION_DENIED:
        return ErrorCode::identity;
    case grpc::StatusCode::ABORTED:
    case grpc::StatusCode::ALREADY_EXISTS:
        return ErrorCode::conflict;
    case grpc::StatusCode::CANCELLED:
        return ErrorCode::cancelled;
    case grpc::StatusCode::RESOURCE_EXHAUSTED:
        return ErrorCode::capacity;
    case grpc::StatusCode::DEADLINE_EXCEEDED:
        return ErrorCode::timeout;
    case grpc::StatusCode::UNKNOWN:
    case grpc::StatusCode::UNAVAILABLE:
        return ErrorCode::transport;
    default:
        return ErrorCode::protocol;
    }
}

// 从失败 status 构造拥有固定诊断文本的 Error, 只读取状态码, 不保留远端 message 或 details.
inline Error rpc_error(const grpc::Status& status) {
    return {rpc_error_code(status.error_code()), "gRPC operation failed"};
}
} // namespace astra
