package verdandi

import (
	"context"
	"errors"
	"fmt"
)

// Code 是跨语言稳定、可供程序判断的 Verdandi 结果类别。
type Code string

const (
	CodeInvalid     Code = "invalid"
	CodeProtocol    Code = "protocol"
	CodeContract    Code = "contract"
	CodeTarget      Code = "target"
	CodeCapacity    Code = "capacity"
	CodeMissing     Code = "missing"
	CodeStale       Code = "stale"
	CodeTransition  Code = "transition"
	CodeImmutable   Code = "immutable"
	CodeCorrupt     Code = "corrupt"
	CodeUnavailable Code = "unavailable"
	CodeDeadline    Code = "deadline"
	CodeAmbiguous   Code = "ambiguous"
	CodeClosed      Code = "closed"
)

// Error 表示一个稳定的 Verdandi 类别及受限的附加上下文。
// 后端或 Context 原始错误可通过 Unwrap 取得，但协议逻辑不得依赖其文本内容。
type Error struct {
	// Code 标识机器可读的结果类别。
	Code Code
	// Field 在适用时标识被拒绝或损坏的协议字段。
	Field string
	// Revision 在适用时携带拒绝结果关联的权威版本号。
	Revision uint64
	// Cause 保存底层驱动、编码器或 Context 错误；它不参与稳定结果码比较。
	Cause error
}

// Error 生成供日志和诊断使用的有界错误文本。
// nil 接收者返回 "<nil>"；调用方若需机器判断应读取 Code 或调用 IsCode。
func (err *Error) Error() string {
	if err == nil {
		return "<nil>"
	}
	message := "verdandi: " + string(err.Code)
	if err.Field != "" {
		message += ": field " + err.Field
	}
	if err.Revision != 0 {
		message += fmt.Sprintf(": revision %d", err.Revision)
	}
	if err.Cause != nil {
		message += ": " + err.Cause.Error()
	}
	return message
}

// Unwrap 返回 Cause 中保存的底层错误；没有 Cause 或接收者为 nil 时返回 nil。
func (err *Error) Unwrap() error {
	if err == nil {
		return nil
	}
	return err.Cause
}

// IsCode 判断 err 的错误链中是否包含指定 code 的 Verdandi Error。
// err 为 nil 或错误链中没有 Verdandi Error 时返回 false。
func IsCode(err error, code Code) bool {
	var target *Error
	return errors.As(err, &target) && target.Code == code
}

// protocolError 构造不带底层 Cause 的稳定协议错误。
// field 和 revision 只在相应错误类别需要上下文时填写，零值表示省略。
func protocolError(code Code, field string, revision uint64) error {
	return &Error{Code: code, Field: field, Revision: revision}
}

// wrapError 用 code 包装 cause，并保留 errors.Is/errors.As 错误链。
// cause 为 nil 时返回 nil，便于调用方直接包装可选错误。
func wrapError(code Code, cause error) error {
	if cause == nil {
		return nil
	}
	return &Error{Code: code, Cause: cause}
}

// wrapContext 把标准 Context 结束原因映射为稳定 Verdandi 错误类别。
// 未知 Context 错误保守地归类为 unavailable，同时保留原始错误链。
func wrapContext(err error) error {
	if errors.Is(err, context.DeadlineExceeded) {
		return wrapError(CodeDeadline, err)
	}
	if errors.Is(err, context.Canceled) {
		return wrapError(CodeClosed, err)
	}
	return wrapError(CodeUnavailable, err)
}
