package server

import (
	"time"

	"github.com/eosforge/verdandi/astra/internal/generated/polaris"
	"github.com/eosforge/verdandi/astra/polaris/internal/storage"
	"google.golang.org/protobuf/proto"
)

// credit 仅记录尚未被累计安装确认覆盖的发送边界, 不复制历史载荷.
type credit struct {
	scope   storage.Scope   // 该信用所属范围, 释放时按范围匹配前缀.
	version storage.Version // 该信用的发送版本, 确认覆盖即释放.
	bytes   int64           // 该信用占据的在途字节, 释放时扣减.
}

// budget 允许普通提交在 8 MiB/128 个发送边界内流水推进. 单份更大的完整快照可独占窗口,
// 但必须先等旧窗口排空, 其自身仍受 Scope 和共享准备预算限制, 不在每页后停等 ACK.
// cost 为本次发送的字节成本; 窗口满时收控制帧直到腾出空间, 上下文取消即返回.
func (transfer *transfer) budget(cost int64) error {
	// 等待期间设阶段截止, 慢端由 Open 侧裁决, 退出即取消.
	transfer.wait(30 * time.Second)
	defer transfer.wait(0)
	// 窗口满 (128 项或 8 MiB) 时只收控制帧, 不发送, 直到腾出空间或出错.
	for len(transfer.credits) != 0 && (len(transfer.credits) >= 128 || cost > 8<<20-transfer.bytes) {
		packet, err := transfer.next()
		if err != nil {
			return err
		}
		if err := transfer.control(packet); err != nil {
			return err
		}
	}
	return transfer.ctx.Err()
}

// charge 仅在完整提交已经交给 gRPC 后调用, 空 Scope 的版本零也占据确认边界.
// scope/version/cost 为本次发送的范围、版本与字节成本; 登记信用并推进已发送.
func (transfer *transfer) charge(scope storage.Scope, version storage.Version, cost int64) {
	transfer.sent[scope] = max(transfer.sent[scope], version)
	transfer.credits = append(transfer.credits, credit{scope: scope, version: version, bytes: cost})
	transfer.bytes += cost
}

// release 以实际安装版本释放对应 Scope 的已发送前缀. 窗口最多 128 项, 原地压缩不分配.
// scope/version 为确认的范围与版本; 覆盖的前缀信用释放, 其余保留.
func (transfer *transfer) release(scope storage.Scope, version storage.Version) {
	// 原地过滤: 保留未覆盖项, 覆盖项扣减字节, 尾部清零防泄漏.
	kept := transfer.credits[:0]
	for _, item := range transfer.credits {
		if item.scope == scope && item.version <= version {
			transfer.bytes -= item.bytes
		} else {
			kept = append(kept, item)
		}
	}
	clear(transfer.credits[len(kept):])
	transfer.credits = kept
}

// updates 单包交接和已发送位置一起推进, 由调用方保证包内连续 +1.
// scope/page 为目标范围与更新页; 先占窗口再发送, 发送后登记信用.
func (transfer *transfer) updates(scope storage.Scope, page *polaris.Updates) error {
	packet := &polaris.Packet{Body: &polaris.Packet_Updates{Updates: page}}
	cost := int64(proto.Size(packet))
	if err := transfer.budget(cost); err != nil {
		return err
	}
	if err := transfer.send(packet); err != nil {
		return err
	}
	transfer.charge(scope, storage.Version(page.Patches[len(page.Patches)-1].Version), cost)
	return nil
}

// snapshot 逐页发送同一版本的完整内容, 页间允许消费控制消息, 只在最终完整页建立安装确认边界.
// snapshot 为冻结快照; 先估算成本占窗口, 再分页发送, 最后登记信用.
func (transfer *transfer) snapshot(snapshot *storage.Snapshot) error {
	// 成本预估: 范围名 + 每条键值开销, 用于窗口准入.
	cost := int64(64 + len(snapshot.Sector) + len(snapshot.Spectrum))
	for _, record := range snapshot.Records {
		cost += int64(32 + len(record.Key) + len(record.Value))
	}
	if err := transfer.budget(cost); err != nil {
		return err
	}
	// 分页发送, 非尾页后处理控制帧, 尾页建立确认边界.
	if err := pages(snapshot, transfer.maximum, func(page *polaris.Snapshot) error {
		if err := transfer.send(&polaris.Packet{Body: &polaris.Packet_Snapshot{Snapshot: page}}); err != nil {
			return err
		}
		// 最后一页的 ACK 可能已经到达, 必须先登记 sent 再由外层消费, 不能提前误报超前.
		if !page.Complete {
			return transfer.drain()
		}
		return nil
	}); err != nil {
		return err
	}
	transfer.charge(snapshot.Scope, snapshot.Version, cost)
	return nil
}
