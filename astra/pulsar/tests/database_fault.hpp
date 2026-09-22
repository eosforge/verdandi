#pragma once
#include "check.hpp"
#include <sqlite3.h>
#include <utility>

namespace astra::test {
// 仅在独立账本测试进程中注册的 VFS, 生产代码没有故障开关. 转发实际磁盘 I/O, 只注入一次写入/同步错误.
class Fault {
public:
    // 默认 VFS 必须已存在, 且同进程只能有一个注入器; 所有测试连接须在析构前关闭.
    Fault() : parent_(sqlite3_vfs_find(nullptr)) {

        CHECK(parent_ && !active_);
        // copy_ 保留原 VFS 的非文件回调和私有数据, 文件打开仍传入原 parent_ 指针.
        copy_ = *parent_;
        copy_.zName = "astra-ledger-fault";
        copy_.szOsFile = parent_->szOsFile + static_cast<int>(sizeof(File));
        copy_.xOpen = open;
        active_ = this;
        if (sqlite3_vfs_register(&copy_, 1) != SQLITE_OK) {
            active_ = nullptr;
            throw std::runtime_error("Cannot install owned SQLite test VFS");
        }
    }

    // 恢复默认 VFS 并注销测试名字, 不关闭其他任务的连接或操作部署文件.
    ~Fault() {
        sqlite3_vfs_register(parent_, 1);
        sqlite3_vfs_unregister(&copy_);
        active_ = nullptr;
    }

    // 禁止复制进程级注入责任.
    Fault(const Fault&) = delete;
    // 禁止覆盖活动 VFS.
    Fault& operator=(const Fault&) = delete;

    // 匹配 xWrite/xSync 返回 code; main 限主库文件, repeat=0 持续失败, 默认一次后自动恢复.
    void arm(int code, bool sync = false, bool main = false, unsigned repeat = 1) {
        CHECK(code != SQLITE_OK);
        sync_ = sync;
        main_ = main;
        repeat_ = repeat;
        error_ = code;
        hits_ = 0;
    }

    // 当前注入是否确实触及对应磁盘路径, 防止未触发的注入被当成通过.
    unsigned hits() const {
        return hits_;
    }

    // 停止持续注入, 供被测连接关闭后在同一路径做真实恢复.
    void clear() {
        error_ = SQLITE_OK;
    }

private:
    // SQLite 分配的头部, 紧随其后为原 VFS 要求的 sqlite3_file 空间, 不另行堆分配.
    struct File {
        // outer 为 SQLite 所见的文件, methods 指向测试转发表.
        sqlite3_file outer;
        // inner 指向同次 SQLite 分配中的真实文件对象, 不独立拥有内存.
        sqlite3_file* inner;
        // main 为真正主数据库, false 表示回滚日志或 SQLite 其他临时文件.
        bool main;
    };

    // 仅本翻译单元的测试进程使用, 生命周期覆盖所有故障连接.
    inline static Fault* active_{};
    // 原默认 VFS 由 SQLite 持有, 恢复时重新注册为默认.
    sqlite3_vfs* parent_;
    // 当前注册的静态接口副本, 修改仅限名字, 文件空间和 xOpen.
    sqlite3_vfs copy_{};
    // error_ 是待消费的一次性错误, 零表示不注入; 本例所有 SQLite 调用在同一测试线程.
    int error_{};
    // sync_ 选择同步故障或写入故障, 默认写入.
    bool sync_{};
    // main_ 限定主库失败, 用于覆盖回滚日志已经持久化之后的提交阶段.
    bool main_{};
    // repeat_ 是剩余失败次数, 零为持续失败直至 clear.
    unsigned repeat_ = 1;
    // hits_ 记录本次 arm 被消费次数, 应严格为一.
    unsigned hits_{};

    // 把 SQLite 暴露的头部映射到本次打开的真实文件, 不修改其所有权.
    static sqlite3_file* file(sqlite3_file* value) {
        return reinterpret_cast<File*>(value)->inner;
    }

    // 对匹配类型消费一次注入, 其余调用保持 SQLITE_OK 以便实际回滚清理.
    static int failure(sqlite3_file* file, bool sync) {
        if (active_->error_ && active_->sync_ == sync && (!active_->main_ || reinterpret_cast<File*>(file)->main)) {
            ++active_->hits_;
            const auto error = active_->error_; // 清除注入前保存本次具体 SQLite 错误码.
            if (active_->repeat_ && --active_->repeat_ == 0) {
                active_->error_ = SQLITE_OK;
            }
            return error;
        }
        return SQLITE_OK;
    }

    // 转发普通 DELETE 模式需要的 v1 文件接口. lambda 参数保持 SQLite 原生 ABI 和单位.
    inline static const sqlite3_io_methods methods_ = {
        1,
        // 关闭内部文件, 包装内存随后由 SQLite 释放.
        [](sqlite3_file* value) { return file(value)->pMethods->xClose(file(value)); },
        // amount 为字节数, offset 为文件偏移, 输出 buffer 由 SQLite 拥有.
        [](sqlite3_file* value, void* buffer, int amount, sqlite3_int64 offset) { return file(value)->pMethods->xRead(file(value), buffer, amount, offset); },
        // 写入前消费一次故障; 未注入时原样转交真实字节和偏移.
        [](sqlite3_file* value, const void* buffer, int amount, sqlite3_int64 offset) {
            const auto error = failure(value, false);
            return error ? error : file(value)->pMethods->xWrite(file(value), buffer, amount, offset);
        },
        // 截断为 size 字节, 由真实回滚流程决定是否需要.
        [](sqlite3_file* value, sqlite3_int64 size) { return file(value)->pMethods->xTruncate(file(value), size); },
        // flags 保持 SQLite 原同步等级, 不把失败伪造成已经持久化.
        [](sqlite3_file* value, int flags) {
            const auto error = failure(value, true);
            return error ? error : file(value)->pMethods->xSync(file(value), flags);
        },
        // 输出真实文件长度, 不用内存中的假长度替代恢复验证.
        [](sqlite3_file* value, sqlite3_int64* size) { return file(value)->pMethods->xFileSize(file(value), size); },
        // mode 为 SQLite 锁级别, 不绕过真实 POSIX 锁冲突.
        [](sqlite3_file* value, int mode) { return file(value)->pMethods->xLock(file(value), mode); },
        // mode 为解锁后的目标级别, 清理由原 VFS 执行.
        [](sqlite3_file* value, int mode) { return file(value)->pMethods->xUnlock(file(value), mode); },
        // held 返回保留锁是否存在, 供 SQLite 处理并发恢复.
        [](sqlite3_file* value, int* held) { return file(value)->pMethods->xCheckReservedLock(file(value), held); },
        // operation/argument 为 SQLite 定义的文件控制参数, 不由测试重解释.
        [](sqlite3_file* value, int operation, void* argument) { return file(value)->pMethods->xFileControl(file(value), operation, argument); },
        // 返回底层实际扇区大小.
        [](sqlite3_file* value) { return file(value)->pMethods->xSectorSize(file(value)); },
        // 返回底层实际设备能力, 不虚构原子写入保证.
        [](sqlite3_file* value) { return file(value)->pMethods->xDeviceCharacteristics(file(value)); },
        // v1 不提供 WAL/共享内存或 mmap, 与被测 DELETE 模式一致.
        nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};

    // value 的空间由 szOsFile 预留, name/flags/output 原样交给真正的默认 VFS.
    static int open(sqlite3_vfs*, const char* name, sqlite3_file* value, int flags, int* output) {

        // wrapper 覆盖 SQLite 的原始空间; 后续 inner 仍保持 sqlite3_file 所需对齐.
        static_assert(sizeof(File) % alignof(sqlite3_file) == 0);
        auto* wrapper = reinterpret_cast<File*>(value);
        wrapper->main = (flags & SQLITE_OPEN_MAIN_DB) != 0;
        wrapper->inner = reinterpret_cast<sqlite3_file*>(reinterpret_cast<char*>(value) + sizeof(File));
        const auto error = active_->parent_->xOpen(active_->parent_, name, wrapper->inner, flags, output);
        if (error == SQLITE_OK || wrapper->inner->pMethods) {
            wrapper->outer.pMethods = &methods_;
        }
        return error;
    }
};
} // namespace astra::test
