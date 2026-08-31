// Package lifecycle 提供领域客户端共享的关闭准入栅栏。
//
// 此包位于 internal 下，只合并 SDK 内部的取消登记和等待规则，不扩大公开 API。
package lifecycle

import (
	"context"
	"sync"
)

// Gate 保证关闭开始后不再增加等待计数，并集中取消已经接纳的工作。
// 零值可直接使用，但 Gate 首次使用后不得复制。
type Gate struct {
	mu            sync.Mutex
	active        sync.WaitGroup
	closing       bool
	next          uint64
	cancellations map[uint64]context.CancelFunc
}

// Closing 返回栅栏是否已经永久关闭。
func (gate *Gate) Closing() bool {
	if gate == nil {
		return true
	}
	gate.mu.Lock()
	closing := gate.closing
	gate.mu.Unlock()
	return closing
}

// Track 在关闭栅栏内登记一项工作，并返回幂等释放函数。
//
// cancel 可为 nil；非 nil 时既会在 Gate 关闭时调用，也会在工作主动释放时调用。
// 返回 false 表示关闭已经开始，调用方不得启动对应工作。
func (gate *Gate) Track(cancel context.CancelFunc) (func(), bool) {
	if gate == nil {
		if cancel != nil {
			cancel()
		}
		return nil, false
	}
	gate.mu.Lock()
	if gate.closing {
		gate.mu.Unlock()
		if cancel != nil {
			cancel()
		}
		return nil, false
	}
	gate.next++
	activity := gate.next
	gate.active.Add(1)
	if cancel != nil {
		if gate.cancellations == nil {
			gate.cancellations = make(map[uint64]context.CancelFunc)
		}
		gate.cancellations[activity] = cancel
	}
	gate.mu.Unlock()

	var once sync.Once
	return func() {
		once.Do(func() {
			gate.mu.Lock()
			delete(gate.cancellations, activity)
			gate.mu.Unlock()
			if cancel != nil {
				cancel()
			}
			gate.active.Done()
		})
	}, true
}

// Start 原子封住后续准入，调用 onStart 发布领域关闭，再取消当前全部工作。
//
// onStart 只在首次调用且仍持有栅栏锁时执行，因此可以安全关闭一次性广播通道。
// 取消函数在解锁后调用，避免取消回调重入释放路径形成死锁。
func (gate *Gate) Start(onStart func()) bool {
	if gate == nil {
		return false
	}
	gate.mu.Lock()
	if gate.closing {
		gate.mu.Unlock()
		return false
	}
	gate.closing = true
	if onStart != nil {
		onStart()
	}
	cancellations := make([]context.CancelFunc, 0, len(gate.cancellations))
	for _, cancel := range gate.cancellations {
		cancellations = append(cancellations, cancel)
	}
	gate.mu.Unlock()

	for _, cancel := range cancellations {
		cancel()
	}
	return true
}

// Wait 阻塞到所有在 Start 前接纳的工作都已释放。
func (gate *Gate) Wait() {
	if gate != nil {
		gate.active.Wait()
	}
}
