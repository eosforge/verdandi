// 功能: 将 gRPC 状态映射为有限本地错误类别, 防止远端错误正文进入诊断.
// 这个文件封装了底层的 gRPC 错误代码转换为我们的内部错误代码，确保错误类型的纯粹性。
#pragma once

#include <astra/types.hpp>

#include <grpcpp/support/status.h>

namespace astra {
// 定义了两个传输适配器模块（Star/Planet）共用的状态转化映射规范。
// 本函数设计强制仅基于抽象状态码分类，绝不去冒险读取或保留任何远端回传带过来的不可控 message/details 数据字段; 
// 暂态突发的单纯传输网络错误断开，不能作为将候选身份直接从配置名单内永久性隔离的依据。
// 由于部分简单的会话回收逻辑内部只需要基础的枚举码就能运行, 特在此分离本方法，从而避免单纯为取得底层 code 还要被迫发生字符串内存临时动态申请分配的 Error 包装对象开销。
// 参数 status: 传入的 grpc 底层状态枚举。
// 返回: 映射对应的 Astra 内部错误类型枚举 Error::Code。
constexpr Error::Code rpc_error_code(grpc::StatusCode status) noexcept {
    switch (status) {
    case grpc::StatusCode::UNAUTHENTICATED: // 未认证的情况
    case grpc::StatusCode::PERMISSION_DENIED: // 无权限，拒绝访问的情况
        // 以上身份不被认可，归为安全凭证与身份错误类别。
        return Error::Code::identity;
    case grpc::StatusCode::ABORTED: // 操作被中间终止
    case grpc::StatusCode::ALREADY_EXISTS: // 资源竞争，已存在冲突
        // 归类为系统的冲突并发或者对象争用错误。
        return Error::Code::conflict;
    case grpc::StatusCode::CANCELLED: // 外部信号干预被主动取消
        // 用户操作或者环境原因产生的取消操作。
        return Error::Code::cancelled;
    case grpc::StatusCode::RESOURCE_EXHAUSTED: // 资源超限、配额耗尽
        // 超过物理资源或者配额许可限制的阈值抛错。
        return Error::Code::capacity;
    case grpc::StatusCode::DEADLINE_EXCEEDED: // 处理过程发生耗时超时现象
        // 发起重试或者网络挂起的关键时间判断。
        return Error::Code::timeout;
    case grpc::StatusCode::UNKNOWN: // 未能识别的错误，常见网络抖动
    case grpc::StatusCode::UNAVAILABLE: // 网络设备离线导致服务彻底不可用
        // 作为标准传输链路故障失败来进行降级与退避。
        return Error::Code::transport;
    default:
        // 对于那些与框架协议紧密强相关以及所有剩余未做特殊明确归属划定的杂项故障代码，全部兜底退化认为是网络协议层面或者版本校验层面的不兼容错误。
        return Error::Code::protocol;
    }
}

// 封装状态码并结合固定错误诊断文本生成标准 Error 对象结构的帮助函数。
// 直接从发生失败状态的 grpc::Status 去快捷构造出一个持有完全独立可控本地写死固定诊断文本的标准 Error 实例，
// 在此严格规定必须只去提取其中的错误状态码核心信息，坚决抛弃不保留由于外部远端带入的任何附带的 message 或者其它危险的 details 动态错误明文描述内容。
// 参数 status: gRPC 核心对象，包含调用期间的全部状态。
// 返回: 组装好属于业务安全定义的 Error 错误描述实体。
inline Error rpc_error(const grpc::Status& status) {
    // 强制截断远端文字注入，使用写死的无风险 "gRPC operation failed" 常量充当文本消息内容返回。
    return {rpc_error_code(status.error_code()), "gRPC operation failed"};
}
} // namespace astra
