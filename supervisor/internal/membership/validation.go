// 成员与登记 RPC 共享名称, 地址和 UUID 校验, 不维护第二份网络规则.
package membership

import "net/netip"

// Address 验证规范的具体 IP:PORT, 与 Rust Member 的地址规则一致.
// 不执行 DNS, 拒绝通配, 多播, 端口零, IPv4 映射别名和仅本机有效的 IPv6 scope.
func Address(value string) bool {
	address, err := netip.ParseAddrPort(value)
	return err == nil && address.String() == value && address.Port() != 0 && address.Addr().Zone() == "" &&
		!address.Addr().Is4In6() && !address.Addr().IsMulticast() && !address.Addr().IsUnspecified()
}

// Name 是与 Peer 对齐的群组名称规则, 接受 1..64 个 ASCII 字母, 数字, 点, 下划线或连字符.
func Name(value string) bool {
	if len(value) == 0 || len(value) > 64 {
		return false
	}
	for _, c := range []byte(value) {
		if !(c >= 'a' && c <= 'z' || c >= 'A' && c <= 'Z' || c >= '0' && c <= '9' || c == '.' || c == '_' || c == '-') {
			return false
		}
	}
	return true
}

// UUID 接受规范的 32 位小写十六进制 UUIDv4, 与项目内进程身份编码保持一致.
func UUID(value string) bool {
	return lowerHex(value, 32) && value[12] == '4' && (value[16] == '8' || value[16] == '9' || value[16] == 'a' || value[16] == 'b')
}

func lowerHex(value string, length int) bool {
	if len(value) != length {
		return false
	}
	for _, c := range []byte(value) {
		if !(c >= '0' && c <= '9' || c >= 'a' && c <= 'f') {
			return false
		}
	}
	return true
}
