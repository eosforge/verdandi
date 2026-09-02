package configuration

import (
	"crypto/tls"
	"crypto/x509"
	"io"
	"net"
	"os"
	"strconv"
	"strings"
	"time"
	"unicode/utf8"

	verdandi "github.com/eosforge/verdandi/sdk/go"
	"github.com/eosforge/verdandi/sdk/go/catalog"
	"github.com/eosforge/verdandi/sdk/go/internal/validate"
	"github.com/eosforge/verdandi/sdk/go/registration"
)

const maximumTLSFileBytes = 1024 * 1024

// check 在不读取证书文件或建立连接的情况下校验所有已启用领域的结构、范围和关系。
func (config Config) check() error {
	if _, err := config.redisConfig(false); err != nil {
		return err
	}
	if _, err := config.RegistrationConfig(); err != nil {
		return err
	}
	_, err := config.CatalogConfig()
	return err
}

// RedisConfig 把跨语言 Redis JSON 配置转换为 Go 的拓扑、Duration、TLS 和连接池配置。
// 返回值已经展开所有 JSON 默认值、读取并解析启用的证书文件且通过 Check，但不建立网络连接。
func (config Config) RedisConfig() (verdandi.Config, error) {
	return config.redisConfig(true)
}

// redisConfig 根据 loadTLS 决定只校验 TLS 文件关系，还是同时读取文件并构造完整 tls.Config。
func (config Config) redisConfig(loadTLS bool) (verdandi.Config, error) {
	redis := config.Redis
	if redis.Mode != "standalone" && redis.Mode != "sentinel" {
		return verdandi.Config{}, configError(verdandi.CodeInvalid, "redis.mode", nil)
	}
	if err := checkAddresses(redis.Addresses); err != nil {
		return verdandi.Config{}, err
	}
	database, err := optionalInt(redis.Database, 0, "redis.database", 0, 255)
	if err != nil {
		return verdandi.Config{}, err
	}
	timeout, err := optionalDuration(redis.TimeoutMS, 2000, "redis.timeout_ms", 10, 15_000)
	if err != nil {
		return verdandi.Config{}, err
	}
	connectTimeout, err := optionalDuration(redis.ConnectTimeoutMS, 5000, "redis.connect_timeout_ms", 20, 30_000)
	if err != nil {
		return verdandi.Config{}, err
	}
	poolMin, err := optionalInt(redis.Pool.MinConnections, 1, "redis.pool.min_connections", 1, 1024)
	if err != nil {
		return verdandi.Config{}, err
	}
	poolMax, err := optionalInt(redis.Pool.MaxConnections, 4, "redis.pool.max_connections", 1, 1024)
	if err != nil {
		return verdandi.Config{}, err
	}
	poolIdle, err := optionalDuration(redis.Pool.IdleTimeoutMS, 10_000, "redis.pool.idle_timeout_ms", 1000, 3_600_000)
	if err != nil {
		return verdandi.Config{}, err
	}
	if poolMin > poolMax {
		return verdandi.Config{}, configError(verdandi.CodeInvalid, "redis.pool.min_connections", nil)
	}
	reconnectDelay, err := optionalDuration(redis.Reconnect.DelayMS, 100, "redis.reconnect.delay_ms", 10, 30_000)
	if err != nil {
		return verdandi.Config{}, err
	}

	tlsConfig, err := buildTLSConfig(redis.TLS, redis.Mode, loadTLS)
	if err != nil {
		return verdandi.Config{}, err
	}
	native := verdandi.Config{
		Timeout:        timeout,
		ConnectTimeout: connectTimeout,
		Pool: verdandi.PoolConfig{
			MinConnections: poolMin,
			MaxConnections: poolMax,
			IdleTimeout:    poolIdle,
		},
		Reconnect: verdandi.ReconnectConfig{
			Delay: reconnectDelay,
		},
	}
	// 拓扑检查同时拒绝无意义的另一模式字段，避免不同语言静默忽略同一 JSON 的不同部分。
	switch redis.Mode {
	case "standalone":
		if len(redis.Addresses) != 1 {
			return verdandi.Config{}, configError(verdandi.CodeInvalid, "redis.addresses", nil)
		}
		if redis.MasterName != "" {
			return verdandi.Config{}, configError(verdandi.CodeInvalid, "redis.master_name", nil)
		}
		if !emptyAuth(redis.SentinelAuth) {
			return verdandi.Config{}, configError(verdandi.CodeInvalid, "redis.sentinel_auth", nil)
		}
		native.Standalone = &verdandi.Standalone{
			Address:  redis.Addresses[0],
			Username: redis.Auth.Username,
			Password: redis.Auth.Password,
			Database: database,
			TLS:      tlsConfig,
		}
	case "sentinel":
		if !utf8.ValidString(redis.MasterName) || strings.TrimSpace(redis.MasterName) == "" || strings.TrimSpace(redis.MasterName) != redis.MasterName || strings.IndexByte(redis.MasterName, 0) >= 0 {
			return verdandi.Config{}, configError(verdandi.CodeInvalid, "redis.master_name", nil)
		}
		native.Sentinel = &verdandi.Sentinel{
			Addresses:        append([]string(nil), redis.Addresses...),
			MasterName:       redis.MasterName,
			Username:         redis.Auth.Username,
			Password:         redis.Auth.Password,
			SentinelUsername: redis.SentinelAuth.Username,
			SentinelPassword: redis.SentinelAuth.Password,
			Database:         database,
			TLS:              tlsConfig,
		}
	}
	if err := native.Check(); err != nil {
		return verdandi.Config{}, err
	}
	return native, nil
}

// buildTLSConfig 校验跨语言 TLS 关系，并在 load 为 true 时有界读取 PEM 文件。
func buildTLSConfig(source TLS, mode string, load bool) (*tls.Config, error) {
	systemRoots := true
	if source.SystemRoots != nil {
		systemRoots = *source.SystemRoots
	}

	// 禁用 TLS 时只允许保留默认信任根选择，避免静默忽略证书或 SNI 配置。
	if !source.Enabled {
		if !systemRoots || source.ServerName != "" || source.CAFile != "" || source.CertFile != "" || source.KeyFile != "" {
			return nil, configError(verdandi.CodeInvalid, "redis.tls", nil)
		}
		return nil, nil
	}

	// Standalone 可省略固定身份并使用连接地址；Sentinel 必须用一个不随动态发现结果变化的共享证书身份。
	if source.ServerName != "" {
		if !utf8.ValidString(source.ServerName) || strings.TrimSpace(source.ServerName) != source.ServerName || containsASCIIWhitespace(source.ServerName) || len(source.ServerName) > 253 || strings.IndexByte(source.ServerName, 0) >= 0 {
			return nil, configError(verdandi.CodeInvalid, "redis.tls.server_name", nil)
		}
	}
	if mode == "sentinel" && source.ServerName == "" {
		return nil, configError(verdandi.CodeInvalid, "redis.tls.server_name", nil)
	}

	// 私有根文件可以补充或替代系统根；客户端证书链与私钥必须成对出现。
	if !systemRoots && source.CAFile == "" {
		return nil, configError(verdandi.CodeInvalid, "redis.tls.ca_file", nil)
	}
	if (source.CertFile == "") != (source.KeyFile == "") {
		return nil, configError(verdandi.CodeInvalid, "redis.tls.cert_file", nil)
	}
	if err := checkTLSPath(source.CAFile, "redis.tls.ca_file"); err != nil {
		return nil, err
	}
	if err := checkTLSPath(source.CertFile, "redis.tls.cert_file"); err != nil {
		return nil, err
	}
	if err := checkTLSPath(source.KeyFile, "redis.tls.key_file"); err != nil {
		return nil, err
	}

	native := &tls.Config{
		MinVersion: tls.VersionTLS12,
		ServerName: source.ServerName,
	}
	if !load {
		return native, nil
	}

	// 明确构造根集合，使 system_roots=false 真正收窄信任边界，ca_file 则向选定集合追加 PEM 证书。
	var roots *x509.CertPool
	if systemRoots {
		var err error
		roots, err = x509.SystemCertPool()
		if err != nil {
			return nil, configError(verdandi.CodeUnavailable, "redis.tls.system_roots", err)
		}
	} else {
		roots = x509.NewCertPool()
	}
	if source.CAFile != "" {
		pem, err := readTLSFile(source.CAFile, "redis.tls.ca_file")
		if err != nil {
			return nil, err
		}
		if !roots.AppendCertsFromPEM(pem) {
			return nil, configError(verdandi.CodeInvalid, "redis.tls.ca_file", nil)
		}
	}
	native.RootCAs = roots

	// tls.X509KeyPair 同时验证 PEM 形状、证书链、私钥编码以及叶证书与私钥是否匹配。
	if source.CertFile != "" {
		certificatePEM, err := readTLSFile(source.CertFile, "redis.tls.cert_file")
		if err != nil {
			return nil, err
		}
		keyPEM, err := readTLSFile(source.KeyFile, "redis.tls.key_file")
		if err != nil {
			return nil, err
		}
		certificate, err := tls.X509KeyPair(certificatePEM, keyPEM)
		if err != nil {
			return nil, configError(verdandi.CodeInvalid, "redis.tls.cert_file", err)
		}
		native.Certificates = []tls.Certificate{certificate}
	}
	return native, nil
}

// checkTLSPath 要求非空 JSON 路径不超过 4096 个 UTF-8 字节且不含操作系统无法接受的 NUL。
func checkTLSPath(path, field string) error {
	if path != "" && (!utf8.ValidString(path) || len(path) > 4096 || strings.IndexByte(path, 0) >= 0) {
		return configError(verdandi.CodeInvalid, field, nil)
	}
	return nil
}

// readTLSFile 最多读取 1 MiB 加一个探测字节，防止错误路径把任意大文件载入内存。
func readTLSFile(path, field string) ([]byte, error) {
	file, err := os.Open(path)
	if err != nil {
		return nil, configError(verdandi.CodeUnavailable, field, err)
	}
	defer file.Close()
	content, err := io.ReadAll(io.LimitReader(file, maximumTLSFileBytes+1))
	if err != nil {
		return nil, configError(verdandi.CodeUnavailable, field, err)
	}
	if len(content) > maximumTLSFileBytes {
		return nil, configError(verdandi.CodeCapacity, field, nil)
	}
	return content, nil
}

// RegistrationConfig 把可选 JSON 领域配置转换为 Go Registration/Selector 精确类型。
// 未配置该领域时返回 nil；返回的非 nil 值已经完成范围和关系检查。
func (config Config) RegistrationConfig() (*registration.Config, error) {
	if config.Registration == nil {
		return nil, nil
	}
	source := config.Registration
	if !validate.Zone(source.Zone) {
		return nil, configError(verdandi.CodeInvalid, "registration.zone", nil)
	}
	native := &registration.Config{Zone: source.Zone}
	var err error
	if native.BufferCapacity, err = optionalInt(source.BufferCapacity, 8, "registration.buffer_capacity", 1, 256); err != nil {
		return nil, err
	}
	if native.ErrorBufferCapacity, err = optionalInt(source.ErrorBufferCapacity, 16, "registration.error_buffer_capacity", 1, 1024); err != nil {
		return nil, err
	}
	if native.MinimumRenewInterval, err = optionalDuration(source.MinRenewIntervalMS, 100, "registration.min_renew_interval_ms", 10, 60_000); err != nil {
		return nil, err
	}
	if native.RenewJitterPercent, err = optionalIntPointer(source.RenewJitterPercent, 10, "registration.renew_jitter_percent", 0, 50); err != nil {
		return nil, err
	}
	if native.PolicyRefreshJitterPercent, err = optionalIntPointer(
		source.PolicyRefreshJitterPercent,
		10,
		"registration.policy_refresh_jitter_percent",
		0,
		50,
	); err != nil {
		return nil, err
	}
	if native.Policy, err = registrationPolicy(source.Policy); err != nil {
		return nil, err
	}
	if err := fillSelector(native, source.Selector); err != nil {
		return nil, err
	}
	if native.SelectorRecoveryInitialDelay > native.SelectorRecoveryMaxDelay {
		return nil, configError(verdandi.CodeInvalid, "registration.selector.recovery.initial_delay_ms", nil)
	}
	if err := native.Check(); err != nil {
		return nil, err
	}
	return native, nil
}

// CatalogConfig 把可选 JSON 领域配置转换为 Go Catalog 的 Duration、容量和路径类型。
// 未配置该领域时返回 nil；返回值由原生 Catalog Config 再执行关系检查。
func (config Config) CatalogConfig() (*catalog.Config, error) {
	if config.Catalog == nil {
		return nil, nil
	}
	source := config.Catalog
	if !validate.Zone(source.Zone) {
		return nil, configError(verdandi.CodeInvalid, "catalog.zone", nil)
	}
	native := &catalog.Config{Zone: source.Zone}
	var err error
	if native.SyncTimeout, err = optionalDuration(source.SyncTimeoutMS, 30_000, "catalog.sync_timeout_ms", 100, 3_600_000); err != nil {
		return nil, err
	}
	if native.ScanPageSize, err = optionalInt(source.ScanPageSize, 256, "catalog.scan_page_size", 1, 4096); err != nil {
		return nil, err
	}
	if native.MaxInflightReads, err = optionalInt(source.MaxInflightReads, 32, "catalog.max_inflight_reads", 1, 256); err != nil {
		return nil, err
	}
	if native.EventBufferCapacity, err = optionalInt(source.EventBufferCapacity, 256, "catalog.event_buffer_capacity", 1, 65_536); err != nil {
		return nil, err
	}
	if native.ErrorBufferCapacity, err = optionalInt(source.ErrorBufferCapacity, 64, "catalog.error_buffer_capacity", 1, 4096); err != nil {
		return nil, err
	}
	if native.MaxViewBytes, err = optionalInt64(source.MaxViewBytes, 0, "catalog.max_view_bytes", 0, 64*1024*1024*1024); err != nil {
		return nil, err
	}
	if native.MaxRecordBytes, err = optionalInt(source.MaxRecordBytes, 512*1024, "catalog.max_record_bytes", 1, 4*1024*1024); err != nil {
		return nil, err
	}
	if native.RecoveryInitialDelay, err = optionalDuration(source.Recovery.InitialDelayMS, 250, "catalog.recovery.initial_delay_ms", 10, 5000); err != nil {
		return nil, err
	}
	if native.RecoveryMaxDelay, err = optionalDuration(source.Recovery.MaxDelayMS, 5000, "catalog.recovery.max_delay_ms", 100, 30_000); err != nil {
		return nil, err
	}
	if native.RecoveryMultiplier, err = optionalInt(source.Recovery.Multiplier, 2, "catalog.recovery.multiplier", 1, 8); err != nil {
		return nil, err
	}
	if native.RecoveryJitterPercent, err = optionalIntPointer(source.Recovery.JitterPercent, 10, "catalog.recovery.jitter_percent", 0, 50); err != nil {
		return nil, err
	}
	if native.RecoveryInitialDelay > native.RecoveryMaxDelay {
		return nil, configError(verdandi.CodeInvalid, "catalog.recovery.initial_delay_ms", nil)
	}
	if source.LocalStorePath != nil {
		if *source.LocalStorePath == "" || !utf8.ValidString(*source.LocalStorePath) || len(*source.LocalStorePath) > 4096 || strings.IndexByte(*source.LocalStorePath, 0) >= 0 {
			return nil, configError(verdandi.CodeInvalid, "catalog.local_store_path", nil)
		}
		native.LocalStorePath = *source.LocalStorePath
	}
	if err := native.Check(); err != nil {
		return nil, err
	}
	return native, nil
}

// registrationPolicy 转换 Redis Zone 初始化策略，并保留省略字段的原生默认语义。
func registrationPolicy(source RegistrationPolicy) (registration.RegistrationLimits, error) {
	var result registration.RegistrationLimits
	var err error
	if result.AttrMaxFields, err = optionalInt(source.AttrMaxFields, 16, "registration.policy.attr_max_fields", 1, 128); err != nil {
		return result, err
	}
	if result.DataMaxFields, err = optionalInt(source.DataMaxFields, 32, "registration.policy.data_max_fields", 1, 128); err != nil {
		return result, err
	}
	if result.FieldNameMaxBytes, err = optionalInt(source.FieldNameMaxBytes, 64, "registration.policy.field_name_max_bytes", 1, 64); err != nil {
		return result, err
	}
	if result.AttrValueMaxBytes, err = optionalInt(source.AttrValueMaxBytes, 128, "registration.policy.attr_value_max_bytes", 1, 16_384); err != nil {
		return result, err
	}
	if result.DataValueMaxBytes, err = optionalInt(source.DataValueMaxBytes, 128, "registration.policy.data_value_max_bytes", 1, 16_384); err != nil {
		return result, err
	}
	if result.RecordMaxBytes, err = optionalInt(source.RecordMaxBytes, 16_384, "registration.policy.record_max_bytes", 1, 65_536); err != nil {
		return result, err
	}
	if result.ConfigurationRefresh, err = optionalDuration(source.RefreshMS, 30_000, "registration.policy.refresh_ms", 1000, 86_400_000); err != nil {
		return result, err
	}
	return result, nil
}

// fillSelector 把嵌套 JSON Selector 设置写入 Registration 包的原生扁平配置。
func fillSelector(native *registration.Config, source Selector) error {
	var err error
	if native.SelectorPageSize, err = optionalInt(source.ScanPageSize, 256, "registration.selector.scan_page_size", 1, 1024); err != nil {
		return err
	}
	if native.SelectorEventBuffer, err = optionalInt(source.MaxPendingEntries, 4096, "registration.selector.max_pending_entries", 1, 65_536); err != nil {
		return err
	}
	if native.SelectorEventBytes, err = optionalInt(
		source.MaxPendingBytes,
		64*1024*1024,
		"registration.selector.max_pending_bytes",
		1,
		1024*1024*1024,
	); err != nil {
		return err
	}
	if native.SelectorPublishInterval, err = optionalDurationPointer(
		source.ViewPublishIntervalMS,
		10,
		"registration.selector.view_publish_interval_ms",
		0,
		1000,
	); err != nil {
		return err
	}
	if native.SelectorSyncTimeout, err = optionalDuration(source.SyncTimeoutMS, 30_000, "registration.selector.sync_timeout_ms", 100, 3_600_000); err != nil {
		return err
	}
	if native.SelectorMaxBytes, err = optionalInt(source.MaxActiveBytes, 256*1024*1024, "registration.selector.max_active_bytes", 1, 1024*1024*1024); err != nil {
		return err
	}
	if native.SelectorRetainedBytes, err = optionalIntPointer(
		source.MaxRetainedBytes,
		64*1024*1024,
		"registration.selector.max_retained_bytes",
		0,
		1024*1024*1024,
	); err != nil {
		return err
	}
	if native.ClockRefresh, err = optionalDuration(
		source.ClockRefreshIntervalMS,
		30_000,
		"registration.selector.clock_refresh_interval_ms",
		1000,
		3_600_000,
	); err != nil {
		return err
	}
	if native.ClockUncertainty, err = optionalDurationPointer(source.ClockUncertaintyMS, 1, "registration.selector.clock_uncertainty_ms", 0, 1000); err != nil {
		return err
	}
	if native.SelectorErrorBufferCapacity, err = optionalInt(source.ErrorBufferCapacity, 16, "registration.selector.error_buffer_capacity", 1, 1024); err != nil {
		return err
	}
	if native.SelectorRecoveryInitialDelay, err = optionalDuration(
		source.Recovery.InitialDelayMS,
		100,
		"registration.selector.recovery.initial_delay_ms",
		10,
		5000,
	); err != nil {
		return err
	}
	if native.SelectorRecoveryMaxDelay, err = optionalDuration(
		source.Recovery.MaxDelayMS,
		5000,
		"registration.selector.recovery.max_delay_ms",
		100,
		30_000,
	); err != nil {
		return err
	}
	if native.SelectorRecoveryMultiplier, err = optionalInt(source.Recovery.Multiplier, 2, "registration.selector.recovery.multiplier", 1, 8); err != nil {
		return err
	}
	native.SelectorRecoveryJitterPercent, err = optionalIntPointer(
		source.Recovery.JitterPercent,
		50,
		"registration.selector.recovery.jitter_percent",
		0,
		50,
	)
	return err
}

// checkAddresses 统一验证 host:port、IPv6 方括号和端口范围，避免驱动间接受集合不同。
func checkAddresses(addresses []string) error {
	if len(addresses) == 0 {
		return configError(verdandi.CodeInvalid, "redis.addresses", nil)
	}
	for _, address := range addresses {
		if !utf8.ValidString(address) || strings.TrimSpace(address) != address || address == "" || strings.IndexByte(address, 0) >= 0 {
			return configError(verdandi.CodeInvalid, "redis.addresses", nil)
		}
		host, portText, err := net.SplitHostPort(address)
		if err != nil || invalidEndpointHost(host) {
			return configError(verdandi.CodeInvalid, "redis.addresses", err)
		}
		port, err := strconv.ParseUint(portText, 10, 16)
		if err != nil || port == 0 || !asciiDigits(portText) {
			return configError(verdandi.CodeInvalid, "redis.addresses", err)
		}
	}
	return nil
}

// asciiDigits 要求端口只含十进制数字，拒绝不同标准库可能接受的前导正号。
func asciiDigits(value string) bool {
	if value == "" {
		return false
	}
	for index := range len(value) {
		if value[index] < '0' || value[index] > '9' {
			return false
		}
	}
	return true
}

// invalidEndpointHost 统一拒绝主机部分中的 ASCII 空白和控制字符。
func invalidEndpointHost(host string) bool {
	if host == "" {
		return true
	}
	for index := range len(host) {
		if host[index] <= ' ' || host[index] == 0x7f {
			return true
		}
	}
	return false
}

// containsASCIIWhitespace 判断文本是否含驱动可能各自处理的 ASCII 空白。
func containsASCIIWhitespace(value string) bool {
	for index := range len(value) {
		switch value[index] {
		case ' ', '\t', '\n', '\r', '\v', '\f':
			return true
		}
	}
	return false
}

// emptyAuth 判断一个 Sentinel 身份是否确实未配置。
func emptyAuth(auth Auth) bool {
	return auth.Username == "" && auth.Password == ""
}

// optionalInt 把可选十进制整数安全转换为 int；nil 展开为 fallback。
func optionalInt(value *int64, fallback int64, field string, minimum, maximum int64) (int, error) {
	converted, err := optionalInt64(value, fallback, field, minimum, maximum)
	if err != nil {
		return int(converted), err
	}
	if int64(int(converted)) != converted {
		return 0, configError(verdandi.CodeCapacity, field, nil)
	}
	return int(converted), nil
}

// optionalInt64 校验可选十进制整数；nil 展开为 fallback。
func optionalInt64(value *int64, fallback int64, field string, minimum, maximum int64) (int64, error) {
	if value == nil {
		value = &fallback
	}
	if *value < minimum || *value > maximum {
		return 0, configError(verdandi.CodeInvalid, field, nil)
	}
	return *value, nil
}

// optionalIntPointer 展开 nil 默认值，并保留显式零为一个独立 *int 值。
func optionalIntPointer(value *int64, fallback int64, field string, minimum, maximum int64) (*int, error) {
	converted, err := optionalInt(value, fallback, field, minimum, maximum)
	if err != nil {
		return nil, err
	}
	return &converted, nil
}

// optionalDuration 把可选整毫秒安全转换为 Duration；nil 展开为 fallback。
func optionalDuration(value *int64, fallback int64, field string, minimum, maximum int64) (time.Duration, error) {
	milliseconds, err := optionalInt64(value, fallback, field, minimum, maximum)
	if err != nil {
		return 0, err
	}
	return time.Duration(milliseconds) * time.Millisecond, nil
}

// optionalDurationPointer 展开 nil 默认值，并保留显式零为一个独立 Duration 指针。
func optionalDurationPointer(value *int64, fallback int64, field string, minimum, maximum int64) (*time.Duration, error) {
	duration, err := optionalDuration(value, fallback, field, minimum, maximum)
	if err != nil {
		return nil, err
	}
	return &duration, nil
}
