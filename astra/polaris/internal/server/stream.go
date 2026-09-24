package server

import (
	"context"
	"errors"
	"io"
	rand "math/rand/v2"
	"sync"
	"sync/atomic"
	"time"

	"github.com/eosforge/verdandi/astra/internal/admission"
	"github.com/eosforge/verdandi/astra/internal/generated/comet"
	"github.com/eosforge/verdandi/astra/internal/generated/orbit"
	"github.com/eosforge/verdandi/astra/internal/generated/polaris"
	"github.com/eosforge/verdandi/astra/polaris/internal/storage"
	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"
	"google.golang.org/protobuf/proto"
)

// Streams 与管理读取共用快照缓存, 每个 Star 实例最多一条当前流, 不反向拨号.
type Streams struct {
	polaris.UnimplementedAlmanacServer
	node      *admission.Node     // 所属准入节点, 提供目录与会话校验.
	authority *Authority          // 权威服务, 提供持久库与快照缓存.
	mutex     sync.Mutex          // 保护活跃实例集合, 只做短临界区.
	active    map[string]struct{} // 当前在线实例 ID 集合, 同实例第二连接拒绝.
	slots     chan struct{}       // 流并发许可 (16), 满时直接拒绝.
	workers   sync.WaitGroup      // 后台收发 worker, Wait 等待其全部退出.
}

// NewStreams 不启动线程或监听, authority 的持久库必须已经恢复完成.
func NewStreams(node *admission.Node, authority *Authority) *Streams {
	return &Streams{node: node, authority: authority, active: make(map[string]struct{}), slots: make(chan struct{}, 16)}
}

// Wait 在 gRPC 停止接纳并结束全部 handler 后调用, 等待真实发送/接收资源退出后再关闭库.
func (streams *Streams) Wait() { streams.workers.Wait() }

// incoming 只有一个 Recv 所有者, 有界通道最多暂存一帧, 不解码无限先行控制消息.
type incoming struct {
	packet *polaris.Packet // 已解码帧, 错误时为空.
	err    error           // 接收错误 (含 EOF), 空表示正常帧.
}

// transfer 的所有协议状态只由 run 所在 goroutine 修改, Open 仅监控阻塞阶段截止.
type transfer struct {
	owner     *Streams                                                 // 所属流集合, 用于目录校验与实例登记.
	wire      grpc.BidiStreamingServer[polaris.Packet, polaris.Packet] // 双向流句柄, 收发各一个所有者.
	ctx       context.Context                                          // 本次传输上下文, handler 退出即取消.
	input     chan incoming                                            // 解码帧队列 (容量 1), 接收与处理解耦.
	received  chan struct{}                                            // 接收 goroutine 退出信号, 释放许可前等待.
	deadline  atomic.Pointer[time.Time]                                // 当前阻塞阶段截止, Open 侧监控.
	wake      chan struct{}                                            // 截止变更唤醒, 容量 1, 不阻塞 run.
	remote    *orbit.Member                                            // 对端 Star 身份, 握手后确定.
	maximum   int                                                      // 对端声明的最大帧字节, 发送不超此限.
	installed map[storage.Scope]storage.Version                        // 对端已确认安装位置, 首轮完成与核对依据.
	sent      map[storage.Scope]storage.Version                        // 本端已发送位置, ACK 不得超过它.
	plan      map[storage.Scope]storage.Version                        // 首轮计划位置, 首轮完成定义不变.
	probe     map[storage.Scope]storage.Version                        // 核对中收集的对端清单, 完成后转 actual.
	boundary  map[storage.Scope]storage.Version                        // 核对发出时的已发送位置, 防新发送误判丢失.
	floor     map[storage.Scope]storage.Version                        // 核对发出时的已安装位置, 丢失判定下限.
	actual    map[storage.Scope]storage.Version                        // 已完成的核对结果, 待 reconcile 处理.
	credits   []credit                                                 // 发送信用 (窗口机制), 详见 window.go.
	bytes     int64                                                    // 在途字节计量, 信用发放依据.
	ready     bool                                                     // 首轮完成标志, 只发送一次 Ready.
}

// Open 让 handler 自身保留结束 RPC 的权力, 即使 Send/Recv 被慢端阻塞也能按单调期限返回.
// 许可在真实 worker 和 Recv 都退出后才归还, 不把请求取消当成资源已经释放.
// wire 为双向流句柄; 返回流处理结果, 慢端阻塞超时返回期限错误.
func (streams *Streams) Open(wire grpc.BidiStreamingServer[polaris.Packet, polaris.Packet]) error {
	// 先占流许可, 满时直接拒绝, 不排队.
	select {
	case streams.slots <- struct{}{}:
	default:
		return status.Error(codes.ResourceExhausted, "Almanac stream capacity exceeded")
	}
	// 派生传输上下文, 任何退出路径都取消, 解除收发阻塞.
	ctx, cancel := context.WithCancel(wire.Context())
	defer cancel()
	transfer := &transfer{owner: streams, wire: wire, ctx: ctx, input: make(chan incoming, 1), received: make(chan struct{}), wake: make(chan struct{}, 1), maximum: 8 << 20, installed: make(map[storage.Scope]storage.Version), sent: make(map[storage.Scope]storage.Version)}
	done := make(chan error, 1)
	streams.workers.Add(1)
	// 接收与主流程分离: 接收只管解码入队, 主流程只管协议, 退出顺序由 received 保证.
	go transfer.receive()
	go func() {
		defer streams.workers.Done()
		// run 不借用 Open 的可写栈对象. handler 返回后 gRPC 取消 transport, 排空真实 Recv 再释放许可.
		err := transfer.run()
		done <- err
		<-transfer.received
		<-streams.slots
	}()
	// 单调期限监控: run 设置阶段截止后唤醒本循环重算定时器, 慢端阻塞超时即结束流.
	timer := time.NewTimer(time.Hour)
	defer timer.Stop()
	for {
		select {
		case err := <-done:
			// 主流程结束, 返回其结果.
			return err
		case <-ctx.Done():
			return status.FromContextError(ctx.Err()).Err()
		case <-transfer.wake:
			// 截止变更, 按新截止重制定时器, 无截止则停止.
			if deadline := transfer.deadline.Load(); deadline != nil {
				timer.Reset(max(time.Until(*deadline), time.Nanosecond))
			} else {
				timer.Stop()
			}
		case <-timer.C:
			// 定时器触发, 过期即结束, 未过期按剩余重设 (防 Reset 竞态).
			if deadline := transfer.deadline.Load(); deadline != nil {
				if time.Until(*deadline) <= 0 {
					return status.Error(codes.DeadlineExceeded, "Almanac stream progress deadline exceeded")
				}
				timer.Reset(time.Until(*deadline))
			}
		}
	}
}

// wait 标记当前有限阻塞阶段, duration=0 取消阶段截止; time.Time 保留本地单调部分.
// duration 为阶段时长, 零表示取消; 设置后唤醒 Open 侧重算定时器.
func (transfer *transfer) wait(duration time.Duration) {
	if duration == 0 {
		transfer.deadline.Store(nil)
	} else {
		deadline := time.Now().Add(duration)
		transfer.deadline.Store(&deadline)
	}
	// 唤醒通知不阻塞, 满时说明 Open 侧已有待处理唤醒.
	select {
	case transfer.wake <- struct{}{}:
	default:
	}
}

// receive 是唯一解码方向, handler 退出取消底层流后解除阻塞, 不主动释放主流程仍使用的快照.
func (transfer *transfer) receive() {
	defer close(transfer.received)
	// 循环解码入队, 上下文取消或流错误即退出, 错误帧也入队让主流程裁决.
	for {
		packet, err := transfer.wire.Recv()
		select {
		case transfer.input <- incoming{packet: packet, err: err}:
		case <-transfer.ctx.Done():
			return
		}
		if err != nil {
			return
		}
	}
}

// next 只取得一条已解码帧, EOF 不是初始同步完成的替代标志.
// 返回下一帧, 上游关闭或上下文取消时返回错误.
func (transfer *transfer) next() (*polaris.Packet, error) {
	select {
	case item := <-transfer.input:
		if errors.Is(item.err, io.EOF) {
			// 上游正常关闭不视为同步完成, 转为取消错误让主流程明确结束.
			return nil, status.Error(codes.Canceled, "Almanac upstream closed")
		}
		return item.packet, item.err
	case <-transfer.ctx.Done():
		return nil, transfer.ctx.Err()
	}
}

// send 独占写方向, 一页发送实际完成后才能复用配额; 整流身份替换不允许继续发送新页.
// packet 为待发送帧; 发送前校验上下文、实例新鲜度与帧大小, 发送期间设 30 秒阶段截止.
func (transfer *transfer) send(packet *polaris.Packet) error {
	// 上下文已取消直接返回, 不向已死流写入.
	if transfer.ctx.Err() != nil {
		return transfer.ctx.Err()
	}
	// 实例被准入更替后禁止继续发送, 防止向旧身份推送新数据.
	if transfer.remote != nil && !transfer.owner.node.Directory().Current(transfer.remote) {
		return status.Error(codes.Unauthenticated, "Star admission replaced")
	}
	// 帧大小不得超过对端声明的接收能力.
	if proto.Size(packet) > transfer.maximum {
		return status.Error(codes.ResourceExhausted, "Almanac frame exceeds receiver capacity")
	}
	// 发送期间设阶段截止, 完成即取消, 慢端阻塞由 Open 侧裁决.
	transfer.wait(30 * time.Second)
	err := transfer.wire.Send(packet)
	transfer.wait(0)
	return err
}

// inventory 收集一个完整位置列表, 要求每 Scope 唯一且在实际部署容量内, 不将半张表发布.
// 返回对端已安装位置, 未收齐完整页即返回错误.
func (transfer *transfer) inventory() (map[storage.Scope]storage.Version, error) {
	positions := make(map[storage.Scope]storage.Version)
	// 清单收集限 10 秒, 慢端由 Open 侧裁决.
	transfer.wait(10 * time.Second)
	defer transfer.wait(0)
	// 循环收页合并, 直到收到完整标志.
	for {
		packet, err := transfer.next()
		if err != nil {
			return nil, err
		}
		page := packet.GetInventory()
		if page == nil {
			return nil, status.Error(codes.InvalidArgument, "Expected complete installation inventory")
		}
		if err := transfer.positions(positions, page); err != nil {
			return nil, err
		}
		if page.Complete {
			return positions, nil
		}
	}
}

// positions 在元数据预算内合并分页, 版本零仍是一份完整基线, 与没有该 Map 项不同.
// target 为合并目标; page 为单页清单; 逐项校验唯一性与范围合法性.
func (transfer *transfer) positions(target map[storage.Scope]storage.Version, page *polaris.Inventory) error {
	for _, position := range page.Positions {
		// 空项直接拒绝, 不跳过.
		if position == nil || position.Scope == nil {
			return status.Error(codes.InvalidArgument, "Missing installation scope")
		}
		scope := storage.Scope{Sector: position.Scope.Sector, Spectrum: position.Scope.Spectrum}
		// 重复范围或非法范围拒绝, 防止清单歧义.
		if _, duplicate := target[scope]; duplicate || !scope.Valid() {
			return status.Error(codes.InvalidArgument, "Invalid or duplicate installation scope")
		}
		// 范围总数不得超过部署容量.
		if int64(len(target)) >= transfer.owner.authority.store.Limits().Scopes {
			return status.Error(codes.ResourceExhausted, "Installation inventory capacity exceeded")
		}
		target[scope] = storage.Version(position.Version)
	}
	return nil
}

// run 固定首轮清单, 逐范围安装且保持提交后缀, 初始完成只依据实际安装 ACK.
func (transfer *transfer) run() error {
	// 首帧限 10 秒, 必须是合法 Hello (协议主版本 1, 帧上限 1 KiB..8 MiB).
	transfer.wait(10 * time.Second)
	packet, err := transfer.next()
	transfer.wait(0)
	if err != nil {
		return err
	}
	hello := packet.GetHello()
	if hello == nil || hello.ProtocolMajor != 1 || hello.MaxFrameBytes < 1024 || hello.MaxFrameBytes > 8<<20 {
		return status.Error(codes.InvalidArgument, "Invalid Almanac Hello")
	}
	// 验签对端 Star 身份, 发送上限取对端声明值.
	transfer.remote, err = transfer.owner.node.Directory().Verify(hello.Admission, hello.AdmissionSignature, orbit.Role_ROLE_STAR)
	if err != nil {
		return err
	}
	transfer.maximum = int(hello.MaxFrameBytes)
	// 同实例第二条连接明确拒绝, 重连等待旧 handler 和缓冲实际退出, 不制造相互抢占循环.
	transfer.owner.mutex.Lock()
	if _, exists := transfer.owner.active[transfer.remote.Id]; exists {
		transfer.owner.mutex.Unlock()
		return status.Error(codes.AlreadyExists, "Star already has an Almanac stream")
	}
	transfer.owner.active[transfer.remote.Id] = struct{}{}
	transfer.owner.mutex.Unlock()
	defer func() {
		transfer.owner.mutex.Lock()
		delete(transfer.owner.active, transfer.remote.Id)
		transfer.owner.mutex.Unlock()
	}()
	// 回送本端 Hello, 准入未就绪即返回不可用.
	local := transfer.owner.node.Hello()
	if local == nil {
		return status.Error(codes.Unavailable, "Polaris admission pending")
	}
	if err := transfer.send(&polaris.Packet{Body: &polaris.Packet_Hello{Hello: local}}); err != nil {
		return err
	}
	// 收集对端已安装清单, 作为首轮起点.
	transfer.installed, err = transfer.inventory()
	if err != nil {
		return err
	}
	// 必须先接入并发提交后缀再固定清单, 夹在初始化和后续发现之间的新 Scope 也不会丢失.
	feed, err := transfer.owner.authority.store.Subscribe(8<<20, 16384)
	if err != nil {
		return failure(err)
	}
	defer feed.Close()
	// 固定首轮计划: 当前权威全量位置, 对端超前即拒绝 (权威落后不可接受).
	positions, err := transfer.owner.authority.store.List(transfer.ctx)
	if err != nil {
		return failure(err)
	}
	transfer.plan = make(map[storage.Scope]storage.Version, len(positions))
	for _, position := range positions {
		transfer.plan[position.Scope] = position.Version
	}
	for scope, version := range transfer.installed {
		if current, exists := transfer.plan[scope]; !exists || version > current {
			return status.Error(codes.Aborted, "Star is ahead of authority database")
		}
		transfer.sent[scope] = version
	}
	// 下发首轮计划, 逐范围同步到计划位置, 每范围后处理控制帧.
	if err := transfer.manifest(positions); err != nil {
		return err
	}
	for _, position := range positions {
		if err := transfer.synchronize(position); err != nil {
			return err
		}
		if err := transfer.drain(); err != nil {
			return err
		}
	}
	// 稳态循环: 核对结果、首轮完成、周期核对、提交后缀四者按序处理, 事件驱动无空转.
	period := time.NewTimer(30*time.Second + time.Duration(rand.IntN(5000))*time.Millisecond)
	defer period.Stop()
	for {
		// 上轮核对结果优先处理.
		if transfer.actual != nil {
			actual := transfer.actual
			transfer.actual = nil
			if err := transfer.reconcile(actual); err != nil {
				return err
			}
		}
		// 首轮完成检查 (只发送一次 Ready).
		if err := transfer.complete(); err != nil {
			return err
		}
		// 持续写入也必须处理核对时钟, 不能只在 Feed 空闲后才进入 select.
		select {
		case <-period.C:
			if err := transfer.assess(); err != nil {
				return err
			}
			period.Reset(30*time.Second + time.Duration(rand.IntN(5000))*time.Millisecond)
		default:
		}
		// 取提交后缀事件, 按范围分组合成有界批次发送.
		changed := feed.Ready()
		events, err := feed.Take()
		if err != nil {
			return failure(err)
		}
		for begin := 0; begin < len(events); {
			// 同 Scope 的相邻事件合成有界批次, 保留每次 +1, 不把一条提交强制变成一次网络往返.
			scope := events[begin].Scope
			if _, exists := transfer.sent[scope]; !exists {
				// 新范围没有版本零基线, 先发送真实完整快照, 不能直接用首个 Patch 偷建空表.
				if err := transfer.synchronize(storage.Position{Scope: scope, Version: events[begin].Version}); err != nil {
					return err
				}
			}
			changes := make([]storage.Change, 0, min(128, len(events)-begin))
			version := transfer.sent[scope]
			// 批次内版本必须连续 +1, 跳过已发送的旧事件, 断档直接报错.
			for begin < len(events) && events[begin].Scope == scope && len(changes) < 128 {
				event := events[begin]
				begin++
				if event.Version <= version {
					continue
				}
				if event.Version != version+1 {
					return status.Error(codes.OutOfRange, "Almanac suffix is not continuous")
				}
				changes = append(changes, event.Change)
				version = event.Version
			}
			if err := transfer.update(scope, changes); err != nil {
				return err
			}
			if err := transfer.drain(); err != nil {
				return err
			}
		}
		if len(events) != 0 {
			// 本轮有事件, 直接继续取下一批, 不进入等待.
			continue
		}
		// 无事件则等待: 上下文取消、后缀就绪、控制帧、核对时钟四选一.
		select {
		case <-transfer.ctx.Done():
			return transfer.ctx.Err()
		case <-changed:
		case item := <-transfer.input:
			if item.err != nil {
				return item.err
			}
			if err := transfer.control(item.packet); err != nil {
				return err
			}
		case <-period.C:
			if err := transfer.assess(); err != nil {
				return err
			}
			period.Reset(30*time.Second + time.Duration(rand.IntN(5000))*time.Millisecond)
		}
	}
}

// manifest 只遍历已捕获的元数据, 页间不重新查询并扩张首轮要求.
// positions 为首轮计划位置; 按 256 KiB 目标分页发送, 尾页标记完整.
func (transfer *transfer) manifest(positions []storage.Position) error {
	page := &polaris.Inventory{}
	size := 16
	// 逐项打包, 页满即发送并开新页, 大小计量含单条开销.
	for _, position := range positions {
		item := &polaris.Position{Scope: &comet.Scope{Sector: position.Sector, Spectrum: position.Spectrum}, Version: uint64(position.Version)}
		cost := proto.Size(item) + 6
		if len(page.Positions) != 0 && size+cost > min(transfer.maximum, 256<<10) {
			if err := transfer.send(&polaris.Packet{Body: &polaris.Packet_Plan{Plan: page}}); err != nil {
				return err
			}
			page, size = &polaris.Inventory{}, 16
		}
		page.Positions = append(page.Positions, item)
		size += cost
	}
	page.Complete = true
	return transfer.send(&polaris.Packet{Body: &polaris.Packet_Plan{Plan: page}})
}

// synchronize 优先从持久连续历史恢复; 无基线或断档才准备该 Scope 完整快照.
// position 为目标位置; 已同步则直接返回, 可重放则发补丁, 否则发快照.
func (transfer *transfer) synchronize(position storage.Position) error {
	// 已发送过该范围: 到位即返回, 落后则尝试历史重放.
	if after, present := transfer.sent[position.Scope]; present {
		if after >= position.Version {
			return nil
		}
		replay, err := transfer.owner.authority.store.Since(transfer.ctx, position.Scope, after)
		if err == nil {
			return transfer.update(position.Scope, replay.Changes)
		}
		if !errors.Is(err, storage.ErrHistory) {
			return failure(err)
		}
		// 历史断档才走快照路径, 其他错误直接返回.
	}
	// 无基线或断档: 从缓存钉住快照发送完整内容.
	pin, err := transfer.owner.authority.cache.acquire(transfer.ctx, position.Scope, position.Version)
	if err != nil {
		return failure(err)
	}
	defer pin.release()
	return transfer.snapshot(pin.Snapshot)
}

// update 一次发送同 Scope 的有序后缀, 按编码预算分页, 不合并重复 Key 或跳过无操作提交.
// scope 为目标范围; changes 为有序变更; 空变更直接返回, 非空按预算分页发送.
func (transfer *transfer) update(scope storage.Scope, changes []storage.Change) error {
	page := &polaris.Updates{Scope: &comet.Scope{Sector: scope.Sector, Spectrum: scope.Spectrum}}
	size := proto.Size(page) + 16
	// 逐条转补丁打包, 页满即发送并开新页, 每页后处理控制帧.
	for _, change := range changes {
		entry := &comet.AlmanacChange{Key: change.Key, Action: &comet.AlmanacChange_Value{Value: change.Value}}
		if change.Erase {
			entry.Action = &comet.AlmanacChange_Erase{Erase: &comet.Empty{}}
		}
		patch := &polaris.Patch{Version: uint64(change.Version), Change: entry}
		cost := proto.Size(patch) + 6
		if len(page.Patches) != 0 && size+cost > min(transfer.maximum, 256<<10) {
			if err := transfer.updates(scope, page); err != nil {
				return err
			}
			page = &polaris.Updates{Scope: page.Scope}
			size = proto.Size(page) + 16
			if err := transfer.drain(); err != nil {
				return err
			}
		}
		page.Patches = append(page.Patches, patch)
		size += cost
	}
	if len(page.Patches) == 0 {
		return nil
	}
	return transfer.updates(scope, page)
}

// drain 在发送批次之间处理有界控制帧, 不阻塞下一页, 已在途数据不被 ACK 抢占.
// 最多处理 16 帧, 无帧即返回, 不等待.
func (transfer *transfer) drain() error {
	for count := 0; count < 16; count++ {
		select {
		case item := <-transfer.input:
			if item.err != nil {
				return item.err
			}
			if err := transfer.control(item.packet); err != nil {
				return err
			}
		default:
			return nil
		}
	}
	return nil
}

// control 只接受实际发送位置以内的单调 ACK, 或主动核对时的完整清单.
// packet 为控制帧; 先验身份, 再按 ACK 或核对清单分支处理.
func (transfer *transfer) control(packet *polaris.Packet) error {
	// 空帧或实例已更替直接拒绝, 不处理任何控制内容.
	if packet == nil || !transfer.owner.node.Directory().Current(transfer.remote) {
		return status.Error(codes.Unauthenticated, "Almanac stream identity unavailable")
	}
	// ACK 分支: 位置必须合法且不超过已发送, 推进已安装并释放窗口.
	if position := packet.GetAcknowledged(); position != nil && position.Scope != nil {
		scope := storage.Scope{Sector: position.Scope.Sector, Spectrum: position.Scope.Spectrum}
		sent, exists := transfer.sent[scope]
		if !scope.Valid() || !exists || storage.Version(position.Version) > sent {
			return status.Error(codes.InvalidArgument, "Acknowledgement exceeds sent state")
		}
		transfer.installed[scope] = max(transfer.installed[scope], storage.Version(position.Version))
		transfer.release(scope, storage.Version(position.Version))
		return nil
	}
	// 核对清单分支: 仅核对进行中接受, 完整后校验丢失与超前, 结果交 actual 异步处理.
	if inventory := packet.GetInventory(); inventory != nil && transfer.probe != nil {
		if err := transfer.positions(transfer.probe, inventory); err != nil {
			return err
		}
		if inventory.Complete {
			// 核对处理不递归发送网络数据. 先释放真实位置覆盖的窗口, 在外层发送边界恢复差异.
			// 先校验下限: 对端丢失已确认状态即数据丢失, 直接报错.
			for scope, minimum := range transfer.floor {
				if version, exists := transfer.probe[scope]; !exists || version < minimum {
					return status.Error(codes.DataLoss, "Star lost previously acknowledged state")
				}
			}
			// 再校验上限: 对端超前于已发送即协议错误, 同样报错.
			for scope, version := range transfer.probe {
				if sent, exists := transfer.sent[scope]; !exists || version > sent {
					return status.Error(codes.Aborted, "Installation exceeds sent state")
				}
				transfer.installed[scope] = max(transfer.installed[scope], version)
				transfer.release(scope, version)
			}
			transfer.actual = transfer.probe
			transfer.probe = nil
		}
		return nil
	}
	return status.Error(codes.InvalidArgument, "Unexpected Almanac control frame")
}

// assess 固定发出 Probe 时的最低发送位置, 不把核对期间新发送的版本误判为对端丢失.
// 核对进行中重复触发即报错; 快照边界与下限后发送探测帧.
func (transfer *transfer) assess() error {
	// 上次核对未完成时不再发起, 防止核对重叠.
	if transfer.probe != nil || transfer.actual != nil {
		return status.Error(codes.DeadlineExceeded, "Installation assessment not completed")
	}
	transfer.probe = make(map[storage.Scope]storage.Version)
	transfer.boundary = make(map[storage.Scope]storage.Version, len(transfer.sent))
	transfer.floor = make(map[storage.Scope]storage.Version, len(transfer.installed))
	// 固定当前已发送为边界、已安装为下限, 后续新发送不影响本次判定.
	for scope, version := range transfer.sent {
		transfer.boundary[scope] = version
	}
	for scope, version := range transfer.installed {
		transfer.floor[scope] = version
	}
	return transfer.send(&polaris.Packet{Body: &polaris.Packet_Probe{Probe: &comet.Empty{}}})
}

// reconcile 对照持久权威与真实安装, 只恢复有差异范围; 确认丢失可由完整清单补齐.
// actual 为核对收集的对端实际位置; 逐范围比对权威, 落后则重放或快照补齐.
func (transfer *transfer) reconcile(actual map[storage.Scope]storage.Version) error {
	// 先取权威全量位置作为比对基准.
	positions, err := transfer.owner.authority.store.List(transfer.ctx)
	if err != nil {
		return failure(err)
	}
	authoritative := make(map[storage.Scope]storage.Version, len(positions))
	for _, position := range positions {
		authoritative[position.Scope] = position.Version
	}
	// 校验对端位置不超权威与已发送, 超限即协议错误.
	for scope, version := range actual {
		if current, exists := authoritative[scope]; !exists || version > current || version > transfer.sent[scope] {
			return status.Error(codes.Aborted, "Installation exceeds confirmed authority state")
		}
		transfer.installed[scope] = max(transfer.installed[scope], version)
	}
	// 逐范围恢复差异: 落后于边界则重放, 重放断档则快照, 已到位则常规同步.
	for _, position := range positions {
		after, present := actual[position.Scope]
		minimum, expected := transfer.boundary[position.Scope]
		if expected && (!present || after < minimum) {
			if present {
				replay, err := transfer.owner.authority.store.Since(transfer.ctx, position.Scope, after)
				if err == nil {
					if err := transfer.update(position.Scope, replay.Changes); err != nil {
						return err
					}
					continue
				}
				if !errors.Is(err, storage.ErrHistory) {
					return failure(err)
				}
			}
			pin, err := transfer.owner.authority.cache.acquire(transfer.ctx, position.Scope, position.Version)
			if err != nil {
				return failure(err)
			}
			err = transfer.snapshot(pin.Snapshot)
			pin.release()
			if err != nil {
				return err
			}
			continue
		}
		if err := transfer.synchronize(position); err != nil {
			return err
		}
	}
	// 核对结束, 清除边界与下限, 下次核对重新固定.
	transfer.boundary = nil
	transfer.floor = nil
	return nil
}

// complete 首轮所有最低版本均得到安装确认后只发送一次 ready, 后续新增范围不改变首轮定义.
// 首轮计划全部达标才发送 Ready 并置位, 未达标直接返回等待.
func (transfer *transfer) complete() error {
	if transfer.ready {
		return nil
	}
	// 逐范围检查已安装是否达到计划, 任一未达标即返回.
	for scope, minimum := range transfer.plan {
		if installed, present := transfer.installed[scope]; !present || installed < minimum {
			return nil
		}
	}
	if err := transfer.send(&polaris.Packet{Body: &polaris.Packet_Ready{Ready: &comet.Empty{}}}); err != nil {
		return err
	}
	transfer.ready = true
	return nil
}
