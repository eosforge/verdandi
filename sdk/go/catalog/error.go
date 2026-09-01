package catalog

import (
	"context"
	"errors"
	"fmt"

	verdandi "github.com/eosforge/verdandi/sdk/go"
	redis "github.com/redis/go-redis/v9"
)

// newError 构造 Catalog 领域的稳定错误，field/revision/cause 为可选上下文。
func newError(code verdandi.Code, field string, revision uint64, cause error) error {
	return &verdandi.Error{Code: code, Field: field, Revision: revision, Cause: cause}
}

// wrapContext 把 Context 截止、取消和未知错误映射为稳定 Verdandi 类别。
func wrapContext(err error) error {
	if errors.Is(err, context.DeadlineExceeded) {
		return newError(verdandi.CodeDeadline, "", 0, err)
	}
	if errors.Is(err, context.Canceled) {
		return newError(verdandi.CodeClosed, "", 0, err)
	}
	return newError(verdandi.CodeUnavailable, "", 0, err)
}

// wrapDriver 把 go-redis 失败映射为稳定类别；code 表示未知响应应采用的读/写语义。
func wrapDriver(code verdandi.Code, err error) error {
	if err == nil {
		return nil
	}
	if errors.Is(err, redis.ErrClosed) {
		return newError(verdandi.CodeClosed, "", 0, fmt.Errorf("catalog redis operation: %w", err))
	}
	if code == verdandi.CodeAmbiguous {
		return newError(code, "", 0, fmt.Errorf("catalog redis operation: %w", err))
	}
	if errors.Is(err, context.DeadlineExceeded) {
		code = verdandi.CodeDeadline
	} else if errors.Is(err, context.Canceled) {
		code = verdandi.CodeClosed
	}
	return newError(code, "", 0, fmt.Errorf("catalog redis operation: %w", err))
}
