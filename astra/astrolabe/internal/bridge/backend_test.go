package bridge

import (
	"encoding/json"
	"math"
	"strings"
	"testing"
	"time"

	"github.com/eosforge/verdandi/astra/internal/generated/orbit"
)

// 只验证目录投影与旧观察寿命, 不把它冒充真实准入/Polaris RPC 运行证据.
func TestObservation(t *testing.T) {
	member := &orbit.Member{Id: "node", Role: orbit.Role_ROLE_STAR, Galaxy: "test", Group: "region", Advertise: "127.0.0.1:4000", Epoch: math.MaxUint64, Principal: "private-principal"}
	backend := &Backend{nodes: []node{describe(member)}, observed: time.Now()}
	old := backend.Nodes()
	member.Id = "changed" // 捕获的是固定字段, 不保留可变 Protobuf 指针.
	backend.mutex.Lock()
	backend.nodes = []node{describe(member)}
	backend.stale = true
	backend.mutex.Unlock()
	before, err := json.Marshal(old)
	if err != nil || !strings.Contains(string(before), `"id":"node"`) || !strings.Contains(string(before), `"epoch":"18446744073709551615"`) || !strings.Contains(string(before), `"stale":false`) || strings.Contains(string(before), "principal") {
		t.Fatal("old observation changed or exposed private identity")
	}
	after, err := json.Marshal(backend.Nodes())
	if err != nil || !strings.Contains(string(after), `"id":"changed"`) || !strings.Contains(string(after), `"stale":true`) {
		t.Fatal("new observation not published")
	}
	backend = &Backend{}
	empty, err := json.Marshal(backend.Nodes())
	if err != nil || !strings.Contains(string(empty), `"nodes":[]`) || !strings.Contains(string(empty), `"stale":true`) {
		t.Fatal("unobserved directory claimed freshness")
	}
}
