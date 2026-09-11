package admission

import (
	"bytes"
	"context"
	"encoding/json"
	wire "github.com/eosforge/verdandi/supervisor/internal/generated"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"
	"strings"
	"testing"
)

func TestProvisionedAccountHasIndependentSaltAndNoPassword(t *testing.T) {
	input := `{"username":"node","password":"test-secret","roles":["star","planet"]}`
	var previous string
	for range 2 {
		var output bytes.Buffer
		if err := WriteAccount(strings.NewReader(input), &output); err != nil {
			t.Fatal(err)
		}
		if strings.Contains(output.String(), "test-secret") {
			t.Fatal("password leaked")
		}
		accounts, err := loadAccounts([]byte("[" + output.String() + "]"))
		if err != nil {
			t.Fatal(err)
		}
		if err := accounts.authenticate(context.Background(), "node", "test-secret", wire.NodeRole_NODE_ROLE_PLANET); err != nil {
			t.Fatal(err)
		}
		if accounts.entries["node"].Salt == previous {
			t.Fatal("salt reused")
		}
		previous = accounts.entries["node"].Salt
	}
}
func TestInvalidAccountFilesFailClosed(t *testing.T) {
	authority, err := Load(fixture("supervisor"))
	if err != nil {
		t.Fatal(err)
	}
	original := authority.accounts.entries["stars"]
	encoded, err := json.Marshal([]Account{original})
	if err != nil {
		t.Fatal(err)
	}
	for _, data := range []string{"[]", "null", string(encoded) + "{}", strings.Replace(string(encoded), `"username"`, `"user"`, 1), strings.Replace(string(encoded), original.Hash, "invalid", 1)} {
		if _, err := loadAccounts([]byte(data)); err == nil {
			t.Fatal("invalid account file accepted")
		}
	}
	duplicate, _ := json.Marshal([]Account{original, original})
	if _, err := loadAccounts(duplicate); err == nil {
		t.Fatal("duplicate accepted")
	}
}
func TestCancelledLoginDoesNotWaitForPasswordCapacity(t *testing.T) {
	a := &accounts{entries: map[string]Account{}, work: make(chan struct{}, 1)}
	a.work <- struct{}{}
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	if err := a.authenticate(ctx, "node", "password", wire.NodeRole_NODE_ROLE_STAR); status.Code(err) != codes.Canceled {
		t.Fatal(err)
	}
}
