package catalog

import (
	"strings"

	verdandi "github.com/eosforge/verdandi/sdk/go"
)

// Path 标识 Client Zone 内的一项 Catalog，包含 part 和 id 两段。
// 通过 NewPath 构造的 Path 不可变、可比较，可安全作为 map 键。
type Path struct {
	part string
	id   string
}

// NewPath 校验并构造一个 part/id 身份；part 最大 64 字节，id 最大 128 字节。
func NewPath(part string, id string) (Path, error) {
	if !validPathSegment(part, 64) {
		return Path{}, newError(verdandi.CodeInvalid, "part", 0, nil)
	}
	if !validPathSegment(id, 128) {
		return Path{}, newError(verdandi.CodeInvalid, "id", 0, nil)
	}
	return Path{part: part, id: id}, nil
}

// Part 返回不可变的分区部分。
func (path Path) Part() string {
	return path.part
}

// ID 返回不可变的标识部分。
func (path Path) ID() string {
	return path.id
}

// valid 重新校验 Path 两段，防止零值或包内错误构造进入协议路径。
func (path Path) valid() bool {
	return validPathSegment(path.part, 64) && validPathSegment(path.id, 128)
}

// member 返回索引使用的规范 part:id 成员文本。
func (path Path) member() string {
	return path.part + ":" + path.id
}

// matchesMember 无分配地判断 member 是否精确表示当前 Path。
func (path Path) matchesMember(member string) bool {
	separator := len(path.part)
	return len(member) == separator+1+len(path.id) &&
		member[:separator] == path.part && member[separator] == ':' &&
		member[separator+1:] == path.id
}

// pathFromMember 从规范 part:id 文本解析并校验 Path。
func pathFromMember(member string) (Path, bool) {
	part, id, found := strings.Cut(member, ":")
	if !found {
		return Path{}, false
	}
	path := Path{part: part, id: id}
	return path, path.valid()
}

// validPathSegment 校验非空、长度受限且只含 ASCII 字母数字、下划线、连字符和点的段。
func validPathSegment(value string, maximum int) bool {
	if len(value) == 0 || len(value) > maximum {
		return false
	}
	for index := range len(value) {
		character := value[index]
		if !asciiLetter(character) && (character < '0' || character > '9') &&
			character != '_' && character != '-' && character != '.' {
			return false
		}
	}
	return true
}

// asciiLetter 判断一个字节是否为 ASCII 大小写字母。
func asciiLetter(value byte) bool {
	return value >= 'a' && value <= 'z' || value >= 'A' && value <= 'Z'
}
