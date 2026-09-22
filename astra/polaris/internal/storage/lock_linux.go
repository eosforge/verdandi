//go:build linux

package storage

import (
	"os"

	"golang.org/x/sys/unix"
)

// lock 独占库旁的稳定锁文件, 正常退出不删除文件, 避免 unlink 后双进程锁住不同 inode.
func lock(path string) (*os.File, error) {
	file, err := os.OpenFile(path+".lock", os.O_RDWR|os.O_CREATE|unix.O_NOFOLLOW, 0600)
	if err != nil {
		return nil, ErrUnavailable
	}
	info, err := file.Stat()
	if err != nil || !info.Mode().IsRegular() {
		file.Close()
		return nil, ErrUnavailable
	}
	if err = unix.Flock(int(file.Fd()), unix.LOCK_EX|unix.LOCK_NB); err != nil {
		file.Close()
		return nil, ErrUnavailable
	}
	return file, nil
}
