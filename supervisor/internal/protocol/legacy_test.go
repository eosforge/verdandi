// Package protocol 仅保存旧 TCP 帧向量的测试实现, 不编入生产服务或提供备用传输.
package protocol

import (
	"encoding/binary"
	"errors"
	"io"
	"slices"

	"google.golang.org/protobuf/proto"

	wire "github.com/eosforge/verdandi/supervisor/internal/generated"
)

// ErrFrame 表示不支持的消息, 非法正文或超限输入. 不把远端正文写入错误日志.
var ErrFrame = errors.New("invalid protocol frame")

// Read 读取恰好一帧. 帧边界 EOF 原样返回, 半包返回 UnexpectedEOF.
// maximum 是本次操作允许的 payload 字节数, 必须由调用方根据当前连接阶段设置.
// 函数不创建 goroutine; 调用方在 socket 上设置总读写期限并负责关闭.
// accepted 非空时在分配正文或解码 repeated 字段之前限制当前阶段的消息类型.
func Read(reader io.Reader, maximum uint32, accepted ...wire.MessageID) (proto.Message, error) {
	var header [6]byte
	if _, err := io.ReadFull(reader, header[:]); err != nil {
		return nil, err
	}
	id := wire.MessageID(binary.BigEndian.Uint16(header[:2]))
	length := binary.BigEndian.Uint32(header[2:])
	if len(accepted) != 0 && !slices.Contains(accepted, id) {
		return nil, ErrFrame
	}
	message := wire.NewMessage(id)
	// 编号和长度都验证之后才分配正文. uint64 比较兼容 32 位编译目标.
	if message == nil || length > maximum || uint64(length) > uint64(^uint(0)>>1) {
		return nil, ErrFrame
	}
	payload := make([]byte, int(length))
	if _, err := io.ReadFull(reader, payload); err != nil {
		// 完整头之后即使一个正文字节也没收到, 也属于截断帧而非帧边界 EOF.
		if errors.Is(err, io.EOF) {
			err = io.ErrUnexpectedEOF
		}
		return nil, err
	}
	if err := (proto.UnmarshalOptions{RecursionLimit: 32}).Unmarshal(payload, message); err != nil {
		return nil, ErrFrame
	}
	return message, nil
}

// Write 在触碰 socket 前检查消息和上限, 然后顺序写出一帧.
// 一个连接只能有一个写入者, 不能并发调用. 中途失败后调用方必须关闭连接.
// 调用期间调用方不得并发修改 message; 编码和写入共享这一只读借用.
func Write(writer io.Writer, message proto.Message, maximum uint32) error {
	id, ok := wire.IDOf(message)
	if !ok || message == nil || !message.ProtoReflect().IsValid() {
		return ErrFrame
	}
	size := proto.Size(message)
	if uint64(size) > uint64(maximum) || size > int(^uint(0)>>1)-6 {
		return ErrFrame
	}
	// 头和正文合并为一次有界编码, 避免两次独立写入让小控制帧频繁触发系统调用.
	frame := make([]byte, 6, 6+size)
	binary.BigEndian.PutUint16(frame, uint16(id))
	binary.BigEndian.PutUint32(frame[2:], uint32(size))
	frame, err := proto.MarshalOptions{}.MarshalAppend(frame, message)
	if err != nil {
		return ErrFrame
	}
	for len(frame) != 0 {
		n, err := writer.Write(frame)
		if n < 0 || n > len(frame) {
			return io.ErrShortWrite
		}
		frame = frame[n:]
		if err != nil {
			return err
		}
		if n == 0 {
			return io.ErrShortWrite
		}
	}
	return nil
}
