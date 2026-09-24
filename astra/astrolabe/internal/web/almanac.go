package web

import (
	"context"
	"encoding/base64"
	"encoding/json"
	"errors"
	"net/http"
	"net/url"
	"strconv"
	"strings"
	"time"
	"unicode/utf8"

	"github.com/eosforge/verdandi/astra/internal/generated/comet"
	"github.com/eosforge/verdandi/astra/internal/generated/orbit"
	"github.com/eosforge/verdandi/astra/internal/generated/polaris"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"
	"google.golang.org/protobuf/proto"
)

// text 与服务侧采用相同 UTF-8/非空/无 NUL 字节范围, 不把斜线解释为路径或权限模式.
// value 为待校验文本; maximum 为最大字节数; 返回是否合法.
func text(value string, maximum int) bool {
	return len(value) != 0 && len(value) <= maximum && utf8.ValidString(value) && !strings.ContainsRune(value, 0)
}

// position 对浏览器将 uint64 始终编码为十进制字符串, 避免 JavaScript Number 损失高位.
// value 为权威确认的位置; 返回范围与版本的可序列化投影.
func position(value *polaris.Position) map[string]string {
	return map[string]string{"sector": value.Scope.Sector, "spectrum": value.Scope.Spectrum, "version": strconv.FormatUint(value.Version, 10)}
}

// almanac 的 GET 无副作用, POST 恰好提交一个完整 Set/Delete, 不保存发布工作流或自动重试.
// response/request 为当前 HTTP 交换; 按方法分发到提交、目录或快照加载.
func (server *Server) almanac(response http.ResponseWriter, request *http.Request) {
	switch request.Method {
	case http.MethodPost:
		server.commit(response, request)
	case http.MethodGet:
		// 查询串必须严格解析, 错误不静默忽略; 空查询走目录, 双参数查询走快照.
		values, err := url.ParseQuery(request.URL.RawQuery)
		if err != nil || len(values) != 0 && (len(values) != 2 || len(values["sector"]) != 1 || len(values["spectrum"]) != 1) {
			problem(response, http.StatusBadRequest, "input", "unapplied")
			return
		}
		if len(values) == 0 {
			server.inventory(response, request)
			return
		}
		scope := &comet.Scope{Sector: values.Get("sector"), Spectrum: values.Get("spectrum")}
		if !text(scope.Sector, 128) || !text(scope.Spectrum, 128) {
			problem(response, http.StatusBadRequest, "input", "unapplied")
			return
		}
		server.load(response, request, scope)
	default:
		problem(response, http.StatusMethodNotAllowed, "method", "unapplied")
	}
}

// commit 只从 Polaris 的实际确认返回 committed; HTTP/RPC 超时不会被改写为未提交或重新发送.
// response/request 为当前 HTTP 交换; 输入为单条 Set/Delete, 版本乐观并发由 Polaris 裁决.
func (server *Server) commit(response http.ResponseWriter, request *http.Request) {
	// 输入三选一: 设置值 (value) 或删除 (erase=true), 附加范围与期望版本.
	var input struct {
		Sector   string  `json:"sector"`
		Spectrum string  `json:"spectrum"`
		Version  string  `json:"version"`
		Key      string  `json:"key"`
		Value    *string `json:"value"`
		Erase    *bool   `json:"erase"`
	}
	if body(response, request, 2<<20, &input) != nil || !text(input.Sector, 128) || !text(input.Spectrum, 128) || !text(input.Key, 1024) || (input.Value == nil) == (input.Erase == nil) || input.Erase != nil && !*input.Erase {
		problem(response, http.StatusBadRequest, "input", "unapplied")
		return
	}
	// 版本必须为规范十进制非零 uint64, 变体写法拒绝.
	version, err := strconv.ParseUint(input.Version, 10, 64)
	if err != nil || version == 0 || strconv.FormatUint(version, 10) != input.Version {
		problem(response, http.StatusBadRequest, "version", "unapplied")
		return
	}
	change := &comet.AlmanacChange{Key: input.Key}
	if input.Value != nil {
		// 值经标准 base64 严格解码, 上限 1 MiB.
		value, err := base64.StdEncoding.Strict().DecodeString(*input.Value)
		if err != nil || len(value) > 1<<20 {
			problem(response, http.StatusBadRequest, "input", "unapplied")
			return
		}
		change.Action = &comet.AlmanacChange_Value{Value: value}
	} else {
		// 删除分支构造擦除动作, 无载荷.
		change.Action = &comet.AlmanacChange_Erase{Erase: &comet.Empty{}}
	}
	server.submit(response, request, &comet.Scope{Sector: input.Sector, Spectrum: input.Spectrum}, change, version)
}

// submit 是普通内容与凭据管理唯一的实际写入点, 不重复提交不确定操作, 不返回任何载荷或秘密.
// scope/change/version 为提交三元组; 成功返回 committed, 结果不匹配即报网关协议错误.
func (server *Server) submit(response http.ResponseWriter, request *http.Request, scope *comet.Scope, change *comet.AlmanacChange, version uint64) {
	// 单次提交限五秒, 超时由 failure 映射为 unknown 效果, 调用方不得假设未提交.
	ctx, cancel := context.WithTimeout(request.Context(), 5*time.Second)
	defer cancel()
	result, err := server.backend.Commit(ctx, &polaris.CommitRequest{Scope: scope, Version: version, Change: change})
	if err != nil {
		code, reason, effect := failure(err, true)
		problem(response, code, reason, effect)
		return
	}
	// 确认位置必须与请求范围版本一致, 否则视为网关协议错误, 不向浏览器伪造成功.
	if result == nil || result.Scope == nil || result.Scope.Sector != scope.Sector || result.Scope.Spectrum != scope.Spectrum || result.Version != version {
		problem(response, http.StatusBadGateway, "protocol", "unknown")
		return
	}
	respond(response, map[string]any{"effect": "committed", "position": position(result)})
}

// inventory 返回一次完整有界目录, 不返回每条内容或 Star 就绪状态.
// response/request 为当前 HTTP 交换; 目录项上限 16384, 超限或残缺即报网关错误.
func (server *Server) inventory(response http.ResponseWriter, request *http.Request) {
	// 目录读取限五秒, 与提交共用超时量级.
	ctx, cancel := context.WithTimeout(request.Context(), 5*time.Second)
	defer cancel()
	result, err := server.backend.List(ctx)
	if err != nil {
		code, reason, effect := failure(err, false)
		problem(response, code, reason, effect)
		return
	}
	if result == nil || !result.Complete || len(result.Positions) > 16384 {
		problem(response, http.StatusBadGateway, "protocol", "unapplied")
		return
	}
	// 逐项校验范围文本并去重, 任何残缺项都导致整个目录失败, 不返回部分目录.
	positions := make([]map[string]string, 0, len(result.Positions))
	seen := make(map[[2]string]bool, len(result.Positions))
	for _, value := range result.Positions {
		if value == nil || value.Scope == nil || !text(value.Scope.Sector, 128) || !text(value.Scope.Spectrum, 128) || seen[[2]string{value.Scope.Sector, value.Scope.Spectrum}] {
			problem(response, http.StatusBadGateway, "protocol", "unapplied")
			return
		}
		seen[[2]string{value.Scope.Sector, value.Scope.Spectrum}] = true
		positions = append(positions, position(value))
	}
	respond(response, map[string]any{"positions": positions, "complete": true})
}

// load 按 NDJSON 逐记录转发, 不在 Astrolabe 再复制完整 64 MiB 底稿; 浏览器只在 complete 行后安装.
// 最终标志必须等 Polaris 流的成功 EOF, 中途错误输出 error 行, 不把已发送的部分记录冒充完整快照.
// scope 为要加载的范围; 并发加载数由 server.loads 信号量限制.
func (server *Server) load(response http.ResponseWriter, request *http.Request, scope *comet.Scope) {
	// 先占并发槽, 满时直接返回忙, 不排队等待.
	select {
	case server.loads <- struct{}{}:
		defer func() { <-server.loads }()
	default:
		problem(response, http.StatusTooManyRequests, "busy", "unapplied")
		return
	}
	// 快照加载限 30 秒, 覆盖大范围全量转发.
	ctx, cancel := context.WithTimeout(request.Context(), 30*time.Second)
	defer cancel()
	response.Header().Set("Content-Type", "application/x-ndjson")
	encoder := json.NewEncoder(response)
	seen := make(map[string]bool) // 已发送键去重, 上限 65536, 防止超大范围耗尽内存.
	var version uint64            // 快照版本号, 全流必须一致.
	var bytes int                 // 累计转发字节, 上限 64 MiB.
	started, complete, wrote, disconnected := false, false, false, false
	err := server.backend.Load(ctx, scope, func(page *polaris.Snapshot) error {
		// 页级校验: 范围一致、版本一致、非空页必须有条目、结束后不再收页.
		if page == nil || page.Scope == nil || page.Scope.Sector != scope.Sector || page.Scope.Spectrum != scope.Spectrum || page.Version == nil || complete || started && version != page.GetVersion() || !page.Complete && len(page.Entries) == 0 {
			return errInput
		}
		version, started = page.GetVersion(), true
		// 逐条校验并转发: 键合法唯一、值有界, 内部范围只给脱敏行.
		for _, entry := range page.Entries {
			if entry == nil || !text(entry.Key, 1024) || seen[entry.Key] || len(seen) == 65536 {
				return errInput
			}
			value, ok := entry.Action.(*comet.AlmanacChange_Value)
			if !ok || value == nil || len(value.Value) > 1<<20 || version == 0 {
				return errInput
			}
			bytes += len(entry.Key) + len(value.Value)
			if bytes > 64<<20 {
				return errInput
			}
			seen[entry.Key] = true
			row := map[string]any{"key": entry.Key}
			if scope.Sector == "__auth" && scope.Spectrum == "comet" {
				var credential orbit.Credential
				if !text(entry.Key, 128) || proto.Unmarshal(value.Value, &credential) != nil || len(credential.Secret) == 0 || len(credential.Secret) > 4096 {
					return errInput
				}
				row["redacted"] = true // 管理读取仅展示 APIKEY, 不把现存 SECRET 送回浏览器.
			} else {
				row["value"] = base64.StdEncoding.EncodeToString(value.Value)
			}
			wrote = true
			if err := encoder.Encode(row); err != nil {
				// 编码失败视为浏览器断开, 直接返回不再输出错误行.
				disconnected = true
				return err
			}
		}
		complete = page.Complete
		return nil
	})
	if disconnected {
		return
	}
	// 流异常或未完整结束: 未写过任何行返回错误码, 写过则补 error 行, 不冒充完整快照.
	if err != nil || !started || !complete {
		code, reason, _ := failure(err, false)
		if err == nil || errors.Is(err, errInput) {
			code, reason = http.StatusBadGateway, "protocol"
		}
		if !wrote {
			problem(response, code, reason, "unapplied")
		} else {
			_ = encoder.Encode(map[string]any{"error": reason, "complete": false})
		}
		return
	}
	// 成功 EOF 后输出 complete 行, 浏览器据此安装快照.
	_ = encoder.Encode(map[string]any{"complete": true, "position": position(&polaris.Position{Scope: scope, Version: version})})
}

// failure 只映射固定分类, 写入中的网络/截止/取消默认不确定; 不透传 gRPC message 或数据库文本.
// err 为原始错误; writing 表示是否为写入路径 (决定默认效果 unknown 还是 unapplied).
// 返回 HTTP 状态码、原因词与效果词.
func failure(err error, writing bool) (int, string, string) {
	effect := "unapplied"
	if writing {
		effect = "unknown"
	}
	// 上下文超限先行判定, 不进入 gRPC 状态码分支.
	if errors.Is(err, context.DeadlineExceeded) {
		return http.StatusGatewayTimeout, "timeout", effect
	}
	// gRPC 状态码到 HTTP 的固定映射, 未知码一律视为后端不可用, 不透传原文.
	switch status.Code(err) {
	case codes.InvalidArgument:
		return http.StatusBadRequest, "input", "unapplied"
	case codes.Aborted:
		return http.StatusConflict, "version", "unapplied"
	case codes.OutOfRange:
		return http.StatusConflict, "history", "unapplied"
	case codes.ResourceExhausted:
		return http.StatusTooManyRequests, "capacity", "unapplied"
	case codes.Unauthenticated, codes.PermissionDenied:
		return http.StatusServiceUnavailable, "backend_identity", "unapplied"
	case codes.DeadlineExceeded:
		return http.StatusGatewayTimeout, "timeout", effect
	default:
		return http.StatusServiceUnavailable, "backend", effect
	}
}
