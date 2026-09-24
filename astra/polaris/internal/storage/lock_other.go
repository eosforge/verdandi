//go:build !linux

package storage

import "os"

// 首版服务运行范围是 Linux, 其他系统不能静默取消单进程独占约束.
// 任何调用都直接返回不可用, 不提供降级锁.
func lock(string) (*os.File, error) { return nil, ErrUnavailable }
