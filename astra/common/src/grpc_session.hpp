// 功能: 定义双向 gRPC 会话和回调边界, 由控制循环独占协议推进与关闭流程.
// 本头文件定义了 gRPC 会话的基类和派生类，用于管理底层的 gRPC 传输与上层业务逻辑的桥接。
#pragma once
#include "astra.grpc.pb.h"
#include "identity.hpp"
#include <astra/policy.hpp>

#include <atomic>
#include <functional>
#include <mutex>

namespace astra {
// RpcSession 类: 每个逻辑流一个对象. gRPC 回调只发布完成标志, 控制循环独占协议状态和角色操作.
// 这个类是对单个 RPC 会话的抽象，主要职责是处理 gRPC 传输的状态、调度读写操作、以及检查权限和生命周期管理。
class RpcSession {
public:
    // 构造函数: 复制配置与 expected, 持有不可变 hello 及 wake, generation 由 Runtime 唯一分配.
    // direction 决定握手方向 (出站或入站), 出站携带授权目标, 入站 expected 为空; wake 不能借用会话或 Runtime.
    // 参数 config: 包含心跳、超时等会话配置 (默认值通常由外部配置对象提供).
    // 参数 direction: 连接的生命周期方向 (入站或出站).
    // 参数 generation: 当前会话的唯一代次，用于区分重建的连接.
    // 参数 hello: 包含当前节点身份与能力的协议握手数据包.
    // 参数 expected: 出站时预期的目标成员信息，入站时为 std::nullopt.
    // 参数 wake: 唤醒外部控制循环的回调函数，当网络 I/O 准备就绪或状态改变时调用.
    RpcSession(const Config& config, Direction direction, SessionGeneration generation, std::shared_ptr<const proto::astra::v1::Hello> hello,
               std::optional<Member> expected, std::function<void()> wake);
               
    // 析构函数: 允许基类指针释放具体 reactor; 仅在最终完成已发布后销毁, 析构不是取消操作.
    virtual ~RpcSession() = default;
    
    // pump 函数: 每轮最多消费一个接收消息. 返回因合法成员替换而需要在其他会话上执行的取消动作.
    // 仅由控制循环调用, policy/identity 仅在调用期间借用, now 为单调时间.
    // 协议错误转换为本会话取消状态, 返回的旧代次取消由 Runtime 执行, 不在回调线程推进.
    // 参数 policy: 控制成员接纳与驱逐的策略对象.
    // 参数 identity: 本地身份，用于验签.
    // 参数 now: 当前单调时间，用于计算超时和截止日期.
    // 返回值: 包含因为本会话建立而需要被取代的其他会话的代次列表.
    std::vector<SessionGeneration> pump(Policy& policy, const Identity& identity, Clock::time_point now);
    
    // cancel 函数: 只请求关闭. 最终完成之前仍由 Runtime 持有, 不能立即析构 context 或写入消息.
    // 仅控制循环调用, error 记录首次关闭原因, 默认 cancelled; 重复请求不重置 250 ms 取消宽限期.
    // 参数 error: 关闭的具体原因代码，默认为 Error::Code::cancelled.
    void cancel(Error::Code error = Error::Code::cancelled);
    
    // done 函数: 以 acquire 读取最终完成发布, true 后可读取最终状态并由 Runtime 回收会话.
    // 返回值: 返回底层会话是否已经完全结束.
    bool done() const;
    
    // installed 函数: 仅控制循环查询 Hello 是否通过身份与角色校验, 不表示已完成业务同步.
    // 返回值: 如果握手已通过且接纳策略同意，则返回 true.
    bool installed() const;
    
    // generation 函数: 返回本进程固定会话代次, 用于精确对应角色索引和完成事件.
    // 返回值: 会话的生成代次标识.
    SessionGeneration generation() const;
    
    // direction 函数: 返回相对于本进程的固定连接方向, 生命周期内不变化.
    // 返回值: 会话方向 (出站或入站).
    Direction direction() const;
    
    // expected 函数: 返回原出站拨号目标的只读借用, 入站为空; 借用不能超过此会话生命周期.
    // 返回值: 期望的对端成员身份，仅对出站有效.
    const std::optional<Member>& expected() const;
    
    // error 函数: 控制循环读取本地关闭原因; 最终非成功 RPC 可细化普通 transport 原因, 不覆盖明确本地协议或身份错误.
    // 返回值: 如果有关闭错误，则返回对应的错误码，否则返回空 opt.
    std::optional<Error::Code> error() const;

protected:
    // transport_ready 函数: 控制循环查询传输是否就绪; 默认适用于已接纳的入站连接, 客户端重载检查 Channel 状态.
    virtual bool transport_ready() const {
        return true;
    }
    
    // begin_call 函数: 由 pump 首次启动调用, 客户端 StartCall, 服务端发送初始元数据; 同一会话最多调用一次.
    virtual void begin_call() = 0;
    
    // begin_read 函数: 提交唯一在途读取, message 存储由会话持有直到 read_done, 完成消费之前禁止复用.
    // 参数 message: 准备接收数据的协议包缓冲指针.
    virtual void begin_read(proto::astra::v1::SessionPacket* message) = 0;
    
    // begin_write 函数: 提交唯一在途写入, message 必须保持有效且不可变直到 write_done, 调用方已设置写入截止.
    // 参数 message: 要发送出去的协议包数据指针.
    virtual void begin_write(const proto::astra::v1::SessionPacket* message) = 0;
    
    // request_cancel 函数: 请求底层取消; force=true 表示宽限期结束必须 TryCancel, false 允许保留远端最终错误状态.
    // 参数 force: 是否强制立刻取消底层上下文.
    virtual void request_cancel(bool force) = 0;
    
    // finish_call 函数: 服务端只能在没有在途写时 Finish; 客户端在外部操作完全停止后归还唯一 hold.
    // error 为首次本地关闭原因, 由服务端映射为固定最终状态; 此函数不表示 OnDone 已到达.
    // 参数 error: 触发调用 Finish 时映射给对端的错误码.
    virtual void finish_call(Error::Code error) = 0;
    
    // read_done 函数: 由读取回调发布 ok 和缓冲所有权, 锁外唤醒控制循环; 不再次发起读取或解析消息.
    // 参数 ok: 指示 gRPC 读操作是否成功结束 (未中断).
    void read_done(bool ok);
    
    // write_done 函数: 由写完成回调释放在途标志并记录 ok, 锁外唤醒控制循环, 不消费发送队列.
    // 参数 ok: 指示写入操作是否成功提交至网络层.
    void write_done(bool ok);
    
    // metadata_done 函数: 由初始元数据回调释放在途标志, !ok 记录发送失败并唤醒, 不安装逻辑身份.
    // 参数 ok: 元数据发送是否成功.
    void metadata_done(bool ok);
    
    // server_cancelled 函数: 由服务端取消回调发布关闭事实并唤醒, 最终完成仍以 OnDone 为准.
    void server_cancelled();
    
    // completed 函数: 仅最终回调调用一次, 移入 status 并以 release 发布 done; 发布后不得再次访问任何会话成员.
    // 参数 status: 描述 RPC 调用的最终完成状态.
    void completed(grpc::Status status);

private:
    // receive 函数: 控制循环解析单条 packet, 先验证容量与 Hello 再调用角色策略, 返回待取消代次或有限错误.
    // 参数仅在调用期间借用; 心跳响应进入本地有界队列, 不在此函数中直接读写传输.
    // 参数 packet: 收到的完整协议包.
    // 参数 policy: 控制策略对象引用.
    // 参数 identity: 本地身份鉴权信息.
    // 参数 now: 当前时刻，用于计算心跳或后续时效.
    // 返回值: 操作结果。如果是成功，则返回需要被取代的老会话的代次数组；否则返回解析或策略错误.
    Result<std::vector<SessionGeneration>> receive(const proto::astra::v1::SessionPacket& packet, Policy& policy, const Identity& identity,
                                                   Clock::time_point now);
                                                   
    // enqueue 函数: 控制循环移入 message 及绝对 deadline; 四槽队列满返回 false, 成功只入队, 不重置原有截止.
    // 参数 message: 准备排队发送的消息包.
    // 参数 deadline: 此消息发送操作必须完成的截止时间.
    // 返回值: 如果队列已满，返回 false 表示无法排队；否则返回 true.
    bool enqueue(proto::astra::v1::SessionPacket message, Clock::time_point deadline);
    
    // config_: 保存会话的各项超时配置和心跳间隔.
    Config config_;
    // direction_: 记录该会话属于主动出站还是被动入站.
    Direction direction_;
    // generation_: 在系统范围内用于唯一标识当前会话的代次编号.
    SessionGeneration generation_;
    // hello_: 保存着本端要向对方展示的身份认证及基础参数 Hello 包.
    std::shared_ptr<const proto::astra::v1::Hello> hello_;
    // expected_: 仅对于出站连接，包含期望远端具备的成员身份。
    std::optional<Member> expected_;
    // wake_: 唤醒绑定在宿主环境控制循环的事件触发器回调.
    std::function<void()> wake_;
    // mutex_: 会话级别的互斥锁，用于保护与异步回调交织的并发状态.
    mutable std::mutex mutex_;
    
    // read_: 会话生命周期内唯一用于当前接收的读取包缓冲.
    proto::astra::v1::SessionPacket read_;
    // write_: 会话生命周期内唯一用于当前正在发送的包缓冲.
    proto::astra::v1::SessionPacket write_;
    
    // 以下标志位表示当前的 I/O 操作状态:
    // read_inflight_: 默认值 false, 标记是否有读请求正在等待底层完成.
    bool read_inflight_{};
    // read_ready_: 默认值 false, 标记是否有新读入的完整消息尚未被业务提取消费.
    bool read_ready_{};
    // read_failed_: 默认值 false, 标记读操作是否遇到了致命异常或断流.
    bool read_failed_{};
    // write_inflight_: 默认值 false, 标记是否有写操作正在排队等待网络层执行完毕.
    bool write_inflight_{};
    // write_failed_: 默认值 false, 标记写入网络是否返回失败.
    bool write_failed_{};
    // metadata_inflight_: 默认值 false, 标记是否正在发送初始服务端元数据.
    bool metadata_inflight_{};
    // remote_cancelled_: 默认值 false, 标记对端是否主动发起了取消信号.
    bool remote_cancelled_{};
    
    // final_status_: gRPC 回调收到的最后一次结束状态对象，内部可能包含详细错误码和信息.
    grpc::Status final_status_;
    // done_: 默认值 false，原子布尔类型，用于指示整个会话过程及其所有底层回调是否彻底终结.
    std::atomic_bool done_{false};
    
    // 以下字段只由控制循环访问, 不与 callback 共享可变协议状态.
    // started_: 默认 false，标识该会话是否已经被业务层接管并开始推进 pump。
    bool started_{};
    // installed_: 默认 false，表示对端的身份认证 Hello 包是否被成功解析和验证通过。
    bool installed_{};
    // finish_called_: 默认 false，表示是否已经请求对端终止 RPC 流（Finish 调用）。
    bool finish_called_{};
    
    // error_: 首次本地关闭原因同时表示进入关闭阶段, 避免另外维护可不一致的 closing 标志.
    std::optional<Error::Code> error_;
    
    // 各种超时及限期时间戳记录：
    // handshake_deadline_: 完成双向握手(验证身份、交换协议基本参数)的最后期限.
    Clock::time_point handshake_deadline_;
    // connect_deadline_: 底层网络层连通所允许的最长等待时间.
    Clock::time_point connect_deadline_;
    // next_ping_: 下次应该主动向对方发起心跳检测的时刻.
    Clock::time_point next_ping_;
    // cancel_deadline_: 如果需要发起中止流程，最多宽限的最后时刻.
    Clock::time_point cancel_deadline_;
    // write_deadline_: 当前唯一正在发送（如果有）的报文被允许的最晚完成时间.
    Clock::time_point write_deadline_;
    
    // pending_ping_: 可选的挂起 ping 请求记录。保存当前心跳 ID 和该心跳允许响应回来的最晚时间。
    std::optional<std::pair<std::uint64_t, Clock::time_point>> pending_ping_;
    
    // next_id_: 心跳请求递增序列号，从 1 开始分配.
    std::uint64_t next_id_ = 1;
    // maximum_: 协商后的单包大小上限，默认初始值为 4096，后续可能会根据双方的 hello 被重新计算缩减.
    std::uint32_t maximum_ = 4096;
    
    // PendingPacket 结构体：保存排队等待发送的消息和其过期时间
    struct PendingPacket {
        // packet: 排队等待中的协议包实体.
        proto::astra::v1::SessionPacket packet;
        // deadline: 发送它的最晚容忍截止时间.
        Clock::time_point deadline;
    };
    // queue_: 使用 4 槽的环形数组存放控制层面的协议消息，避免队列过长造成背压.
    std::array<std::optional<PendingPacket>, 4> queue_;
    // head_: 环形队列的头部指针（索引，默认 0）.
    std::size_t head_{};
    // queued_: 当前排队积压的消息计数（默认 0）.
    std::size_t queued_{};
};

// connect_session 工厂函数: 在启动 Call 前把对象交给 Runtime, 不从 callback 中 delete this 或创建所有权环.
// 使用 config/identity 准备 target 对应的客户端, 持有 hello/wake 和目标副本; 返回共享对象供 Runtime 接管后 pump.
// 参数 config: 会话级配置参数.
// 参数 identity: 本地的身份鉴权信息.
// 参数 generation: 指定要赋予的会话代次.
// 参数 hello: 本端准入和角色属性对象.
// 参数 target: 想要去连接的远端节点成员目标对象.
// 参数 wake: 状态改变时的回调函数.
// 返回值: 新建的出站 RpcSession 派生类(客户端会话)共享指针.
std::shared_ptr<RpcSession> connect_session(const Config& config, const Identity& identity, SessionGeneration generation,
                                            std::shared_ptr<const proto::astra::v1::Hello> hello, Member target, std::function<void()> wake);

// AcceptedSession 类: 生成的服务 handler 使用实际 ServerBidiReactor, 生命周期由同一 Runtime 集合持有.
// 作为服务端的实现，它同时继承自 RpcSession 和 gRPC 的双向流处理反应器。
class AcceptedSession : public RpcSession, public grpc::ServerBidiReactor<proto::astra::v1::SessionPacket, proto::astra::v1::SessionPacket> {
public:
    // 构造函数: context 由 gRPC 借出直到 OnDone, config 被复制, hello/wake 被持有; Runtime 须在 handler 返回前登记所有权.
    // 参数 context: gRPC 注入的服务端环境信息上下文，管理元数据与取消操作.
    // 参数 config: 配置信息(心跳、超时等).
    // 参数 generation: 当前服务端分配接受此连接的新代次.
    // 参数 hello: 作为服务端返回给客户端的本端凭证.
    // 参数 wake: 状态改变通知控制循环的回调。
    AcceptedSession(grpc::CallbackServerContext* context, const Config& config, SessionGeneration generation,
                    std::shared_ptr<const proto::astra::v1::Hello> hello, std::function<void()> wake);
                    
    // OnReadDone 回调函数: gRPC 读取完成入口, 仅将 ok 转发给 read_done, 不推进角色状态.
    // 参数 ok: true 表示流的读取成功，false 可能表示流关闭或者异常结束.
    void OnReadDone(bool ok) override;
    
    // OnWriteDone 回调函数: gRPC 写入完成入口, 仅将 ok 转发给 write_done, 不复用写缓冲.
    // 参数 ok: true 表示写入操作成功到底层发送缓冲区.
    void OnWriteDone(bool ok) override;
    
    // OnSendInitialMetadataDone 回调函数: gRPC 初始元数据完成入口, 将 ok 发布到控制循环.
    // 参数 ok: true 表示发送 Header/元数据 成功.
    void OnSendInitialMetadataDone(bool ok) override;
    
    // OnCancel 回调函数: gRPC 服务端取消通知入口, 记录事实而不释放 reactor.
    void OnCancel() override;
    
    // OnDone 回调函数: gRPC 最终完成入口, 发布完成状态, 生命周期由 Runtime 回收.
    void OnDone() override;

private:
    // begin_call 重写: 发送不含准入凭证的初始元数据, 避免客户端等待响应头导致 Hello 互等.
    void begin_call() override;
    
    // begin_read 重写: 将会话拥有的接收缓冲借给 StartRead, 直到 OnReadDone 前不得修改.
    // 参数 message: 会话传入供 gRPC 填入字节的接收报文指针.
    void begin_read(proto::astra::v1::SessionPacket* message) override;
    
    // begin_write 重写: 将不可变发送缓冲借给 StartWrite, 直到 OnWriteDone 前不得复用.
    // 参数 message: 会话提供的等待向客户端写出的报文内容指针.
    void begin_write(const proto::astra::v1::SessionPacket* message) override;
    
    // request_cancel 重写: 仅 force 时取消服务端 context, 宽限期内允许 Finish 携带明确拒绝状态.
    // 参数 force: 若为 true 强制调用 gRPC 的 TryCancel 中止服务端流，否则仅仅是进入关闭流程等待发送缓冲排空.
    void request_cancel(bool force) override;
    
    // finish_call 重写: 将本地 error 转为固定 gRPC 状态并 Finish, 调用方须保证写入和初始元数据已完成.
    // 参数 error: 用于映射成 grpc::StatusCode 的底层关闭原因代码.
    void finish_call(Error::Code error) override;
    
    // context_: 关联当前 RPC 流的服务端环境上下文引用，由 gRPC 控制底层状态.
    grpc::CallbackServerContext* context_;
};
} // namespace astra
