#include "receiver.hpp"
#include "orbit.pb.h"
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace astra {
Receiver::Receiver(Library& output, Access& access) : output_(output), access_(access), inventory_(output.positions()) {}

bool Receiver::ready() const noexcept {
    return ready_ && !failed_;
}

bool Receiver::valid(const Scope& scope, const proto::comet::v1::AlmanacChange& change, bool snapshot) {

    if (!Scope::text(change.key(), 1024) || change.action_case() == proto::comet::v1::AlmanacChange::ACTION_NOT_SET || (snapshot && change.action_case() != proto::comet::v1::AlmanacChange::kValue) || change.value().size() > 1024 * 1024) {
        return false;
    }
    if (scope.sector != "__auth" || scope.spectrum != "comet") {
        return true;
    }

    // 内部登录表必须与管理入口采用相同约束, 不允许坏底稿成为部分有效的认证状态.
    if (change.key().size() > 128 || change.value().size() > 8192) {
        return false;
    }
    if (change.action_case() == proto::comet::v1::AlmanacChange::kErase) {
        return true;
    }
    proto::orbit::v1::Credential credential; // 只在固定内部 Scope 解析, 不解释普通业务 Buffer.
    return credential.ParseFromString(change.value()) && !credential.secret().empty() && credential.secret().size() <= 4096;
}

Result<void> Receiver::failure(Almanac::Error error) {
    if (error == Almanac::Error::capacity) {
        return Status::capacity("Almanac installation capacity exceeded");
    }
    if (error == Almanac::Error::version || error == Almanac::Error::history || error == Almanac::Error::unready) {
        return Status::conflict("Almanac installation is not continuous");
    }
    return Status::protocol("Invalid Almanac installation data");
}

Result<void> Receiver::plan(const proto::polaris::v1::Inventory& page) {

    if (planned_) {
        return Status::protocol("Repeated initial Almanac plan");
    }
    // position 是暂存的最低要求, 多页只有全部完成后才可以用于业务启动.
    for (const auto& position : page.positions()) {
        Scope scope{position.scope().sector(), position.scope().spectrum()};
        if (!scope.valid() || plan_.contains(scope) || plan_.size() >= 4096) {
            return Status::protocol("Invalid or duplicate Almanac plan scope");
        }
        if (const auto book = output_.find(scope)) {
            if (const auto current = book->usage().version; current && *current > position.version()) {
                return Status::conflict("Authority is behind installed Almanac");
            }
        }
        plan_.emplace(std::move(scope), position.version());
    }
    planned_ = page.complete();
    return {};
}

void Receiver::acknowledge(const Scope& scope, std::uint64_t version) {
    // present 不区分值零, Map 项的存在本身表达完整空基线已经安装.
    auto& present = acknowledgements_[scope];
    present = std::max(present, version);
}

Result<void> Receiver::snapshot(const proto::polaris::v1::Snapshot& page) {

    // scope/version 必须在每页稳定存在, 一页超限在上游 gRPC 解码预算之外再次按业务检查.
    Scope scope{page.scope().sector(), page.scope().spectrum()};
    if (!scope.valid() || !page.has_version() || page.entries_size() > 65536 || (!page.complete() && page.entries().empty())) {
        return Status::protocol("Invalid Almanac snapshot page");
    }
    for (const auto& entry : page.entries()) {
        if (!valid(scope, entry, true)) {
            return Status::protocol("Invalid Almanac snapshot record");
        }
    }
    if (!draft_) {
        auto draft = output_.prepare(scope, page.version()); // 私有准备不提前建立路由或改变旧读视图.
        if (!draft) {
            return failure(draft.error());
        }
        draft_.emplace(std::move(*draft));
        if (scope.sector == "__auth" && scope.spectrum == "comet") {
            credentials_.emplace();
        }
    }
    if (draft_->scope() != scope || draft_->version() != page.version()) {
        return Status::protocol("Almanac snapshot scope or version changed");
    }

    // 每个 value 只复制一次进入不可变存储, 后续各读视图共享正文, 不保留 Proto 可写缓冲.
    for (const auto& entry : page.entries()) {
        if (credentials_) {
            proto::orbit::v1::Credential credential; // 已预检过正文, 候选索引仍只接收完整有效 SECRET.
            if (!credential.ParseFromString(entry.value()) || !credentials_->set(entry.key(), Almanac::Buffer(credential.secret().begin(), credential.secret().end()))) {
                return Status::protocol("Invalid credential snapshot index");
            }
        }
        Almanac::Buffer value(entry.value().begin(), entry.value().end());
        if (auto added = draft_->set(entry.key(), std::move(value)); !added) {
            return failure(added.error());
        }
    }
    if (page.complete()) {
        // commit 不分开推进凭据版本和索引, Access 先使旧授权失效, 解锁后才传播流取消.
        auto commit = [&] { return output_.reset(std::move(*draft_)); };
        if (auto installed = credentials_ ? access_.reset(std::move(*credentials_), commit) : commit(); !installed) {
            return failure(installed.error());
        }
        draft_.reset();
        credentials_.reset();
        acknowledge(scope, page.version());
    }
    return {};
}

Result<void> Receiver::updates(const proto::polaris::v1::Updates& page) {

    Scope scope{page.scope().sector(), page.scope().spectrum()}; // 整包只属于同一真实分组.
    if (!scope.valid() || page.patches().empty() || page.patches_size() > 65536 || draft_) {
        return Status::protocol("Invalid Almanac update batch");
    }
    // previous 只验证包内连续性, 对本地已安装前缀的幂等跳过由原生 Almanac 负责.
    std::uint64_t previous{};
    for (const auto& patch : page.patches()) {
        if (!patch.has_change() || patch.version() == 0 || (previous != 0 && (previous == std::numeric_limits<std::uint64_t>::max() || patch.version() != previous + 1)) || !valid(scope, patch.change(), false)) {
            return Status::protocol("Invalid or noncontiguous Almanac patches");
        }
        previous = patch.version();
    }

    // 每个成功前缀已经完整可见, 后续失败保留其版本, 重连清单会报告真实受理位置.
    for (const auto& patch : page.patches()) {
        std::optional<Almanac::Buffer> value; // 空表示 Delete, 非空零长度表示合法 Set.
        if (patch.change().action_case() == proto::comet::v1::AlmanacChange::kValue) {
            value.emplace(patch.change().value().begin(), patch.change().value().end());
        }
        auto commit = [&] { return output_.apply(scope, patch.version(), patch.change().key(), std::move(value)); };
        std::expected<bool, Almanac::Error> applied; // 每个补丁只有一次真实提交, 重放不产生第二次撤销.
        if (scope.sector == "__auth" && scope.spectrum == "comet") {
            std::optional<Almanac::Buffer> secret; // Delete 不创建新凭据, 空 SECRET 已在整包预检时拒绝.
            if (value) {
                proto::orbit::v1::Credential credential;
                if (!credential.ParseFromString(patch.change().value())) {
                    return Status::protocol("Invalid credential patch index");
                }
                secret.emplace(credential.secret().begin(), credential.secret().end());
            }
            applied = access_.apply(patch.change().key(), std::move(secret), commit);
        } else {
            applied = commit();
        }
        if (!applied) {
            return failure(applied.error());
        }
        acknowledge(scope, *output_.find(scope)->usage().version);
    }
    return {};
}

Result<void> Receiver::receive(const proto::polaris::v1::Packet& packet) {

    if (failed_) {
        return Status::protocol("Almanac receiver is closed");
    }
    try {
        auto result = process(packet); // 只有此入口能推进协议, 错误后丢弃私有准备并封闭状态.
        if (!result) {
            failed_ = true;
            draft_.reset();
        }
        return result;
    } catch (...) {
        failed_ = true;
        draft_.reset();
        throw;
    }
}

Result<void> Receiver::process(const proto::polaris::v1::Packet& packet) {

    if (packet.ByteSizeLong() > 8 * 1024 * 1024) {
        return Status::capacity("Almanac packet exceeds receive budget");
    }
    if (packet.has_plan()) {
        return plan(packet.plan());
    }
    if (!planned_) {
        return Status::protocol("Almanac initial plan is incomplete");
    }

    if (packet.has_snapshot()) {
        return snapshot(packet.snapshot());
    }
    if (packet.has_updates()) {
        return updates(packet.updates());
    }
    if (packet.has_probe()) {
        if (inventory_) {
            return Status::protocol("Overlapping Almanac inventory probes");
        }
        inventory_ = output_.positions();
        position_ = 0;
        return {};
    }
    if (packet.has_ready() && !ready_ && !draft_) {
        // ready 是服务端观察, 客户端仍独立核对本轮清单, 不能被空标志绕过完整安装.
        for (const auto& [scope, minimum] : plan_) {
            const auto book = output_.find(scope);
            if (!book || !book->usage().version || *book->usage().version < minimum) {
                return Status::protocol("Premature Almanac readiness");
            }
        }
        ready_ = true;
        return {};
    }
    return Status::protocol("Unexpected Almanac control frame");
}

std::optional<proto::polaris::v1::Packet> Receiver::next(std::size_t maximum) {

    if (maximum < 1024 || maximum > 8 * 1024 * 1024) {
        throw std::invalid_argument("Invalid Almanac negotiated send capacity");
    }

    if (failed_) {
        return std::nullopt;
    }

    proto::polaris::v1::Packet packet; // 拥有式帧只能在 gRPC 写完成后被复用.
    if (inventory_) {
        auto* page = packet.mutable_inventory(); // 即使空表也发送 complete, 不用零条消息代表完整.
        std::size_t bytes = 32;                  // 保守编码计费, 单个位置最多两个 128 字节地址和版本.
        while (position_ < inventory_->size()) {
            const auto& position = (*inventory_)[position_];
            // 编码前保守计费, 不能先越过协商上限再在下一轮停止; 至少一个位置必定容纳于 1024 字节.
            const auto cost = position.scope.sector.size() + position.scope.spectrum.size() + 32;
            if (bytes + cost > std::min(maximum, std::size_t{256 * 1024})) {
                break;
            }
            auto* entry = page->add_positions();
            entry->mutable_scope()->set_sector(position.scope.sector);
            entry->mutable_scope()->set_spectrum(position.scope.spectrum);
            entry->set_version(position.version);
            bytes += cost;
            ++position_;
        }
        if (position_ == inventory_->size()) {
            page->set_complete(true);
            inventory_.reset();
        }
        return packet;
    }
    if (acknowledgements_.empty()) {
        return std::nullopt;
    }

    const auto& [scope, version] = *acknowledgements_.begin(); // 编码成功前保留原 ACK, 分配失败不丢确认依据.
    auto* position = packet.mutable_acknowledged();
    position->mutable_scope()->set_sector(scope.sector);
    position->mutable_scope()->set_spectrum(scope.spectrum);
    position->set_version(version);
    acknowledgements_.erase(acknowledgements_.begin());
    return packet;
}
} // namespace astra
