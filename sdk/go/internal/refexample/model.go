// Package refexample 编译检查 verdandi-refgen 产物与公开 Registration API 的组合。
package refexample

//go:generate go run ../../cmd/verdandi-refgen -attr ProxyAttr -data ProxyData -name Proxy -output reference_generated.go

// Certificate 是用于验证命名 byte slice 深复制规则的示例类型。
type Certificate []byte

// ProxyAttr 是代码生成编译样例的不可变放置属性。
type ProxyAttr struct {
	Certificate Certificate
	Endpoint    string
}

// ProxyData 是代码生成编译样例的可变服务数据。
type ProxyData struct {
	Payload []byte
	Power   int64
	Ready   bool
}
