//go:build linux

package storage

import (
	"os"

	"golang.org/x/sys/unix"
)

// lock 独占库旁的稳定锁文件, 正常退出不删除文件, 避免 unlink 后双进程锁住不同 inode.
// path 为库文件路径, 锁文件为其同目录 .lock 后缀; 返回锁句柄, 加锁失败返回不可用.
func lock(path string) (*os.File, error) {
	// 打开或创建锁文件, 禁跟随符号链接, 权限 0600.
	file, err := os.OpenFile(path+".lock", os.O_RDWR|os.O_CREATE|unix.O_NOFOLLOW, 0600)
	if err != nil {
		return nil, ErrUnavailable
	}
	// 必须为普通文件, 防止锁住设备或目录.
	info, err := file.Stat()
	if err != nil || !info.Mode().IsRegular() {
		file.Close()
		return nil, ErrUnavailable
	}
	// 非阻塞独占锁, 已有进程持有即失败.
	if err = unix.Flock(int(file.Fd()), unix.LOCK_EX|unix.LOCK_NB); err != nil {
		file.Close()
		return nil, ErrUnavailable
	}
	return file, nil
}
