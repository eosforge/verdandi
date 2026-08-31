package catalog

// zonePrefix 返回指定 Zone 的 Catalog 键前缀。
func zonePrefix(zone string) string {
	return "verdandi:catalog:" + zone
}

// metaKey 返回 Zone 全局 revision/floor 元数据 Hash 键。
func metaKey(zone string) string {
	return zonePrefix(zone) + ":@meta"
}

// liveKey 返回 Zone 当前存在 Path 的 revision ZSET 键。
func liveKey(zone string) string {
	return zonePrefix(zone) + ":@live"
}

// deletedKey 返回 Zone tombstone Path 的 revision ZSET 键。
func deletedKey(zone string) string {
	return zonePrefix(zone) + ":@deleted"
}

// deletedTimeKey 返回 Zone tombstone 写入时间 ZSET 键。
func deletedTimeKey(zone string) string {
	return zonePrefix(zone) + ":@deleted_time"
}

// catalogKey 返回单个 Path 的完整 Catalog Hash 键，同时也是其 Pub/Sub 频道。
func catalogKey(zone string, path Path) string {
	return zonePrefix(zone) + ":" + path.part + ":" + path.id
}

// fieldRevisionsKey 返回单个 Path 各字段 revision 的 Hash 键。
func fieldRevisionsKey(zone string, path Path) string {
	return catalogKey(zone, path) + ":@field_revisions"
}

// readKeys 按只读 Lua 固定 ABI 返回一个 Path 所需的全部键。
func readKeys(zone string, path Path) []string {
	return []string{
		liveKey(zone),
		deletedKey(zone),
		deletedTimeKey(zone),
		catalogKey(zone, path),
		fieldRevisionsKey(zone, path),
	}
}

// mutationKeys 按变更 Lua 固定 ABI 返回完整键列表。
func mutationKeys(zone string, path Path) []string {
	return []string{
		metaKey(zone),
		liveKey(zone),
		deletedKey(zone),
		deletedTimeKey(zone),
		catalogKey(zone, path),
		fieldRevisionsKey(zone, path),
	}
}
