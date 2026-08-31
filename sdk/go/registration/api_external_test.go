package registration_test

import (
	"context"
	"testing"
	"time"

	verdandi "github.com/LaconisIves/verdandi/sdk/go"
	"github.com/LaconisIves/verdandi/sdk/go/registration"
)

type externalAttr struct {
	region string
}

func (value externalAttr) Encode() (verdandi.Fields, error) {
	return verdandi.Fields{"region": []byte(value.region)}, nil
}

func (value *externalAttr) Decode(src verdandi.Fields) error {
	value.region = string(src["region"])
	return nil
}

type externalData struct {
	power byte
}

func (value externalData) Encode() (verdandi.Fields, error) {
	return verdandi.Fields{"power": []byte{value.power}}, nil
}

func (value *externalData) Decode(src verdandi.Fields) error {
	value.power = src["power"][0]
	return nil
}

func TestPublicRegistrationConstructionInfersPrivateDecoderTypes(t *testing.T) {
	client := &registration.Client{}
	handle, err := client.Registration[externalAttr, externalData](
		registration.RegistrationOptions{Type: "proxy", TTL: 3 * time.Second, Version: 1},
	)
	if err != nil {
		t.Fatal(err)
	}
	if err := handle.Unregister(context.Background()); err != nil {
		t.Fatal(err)
	}

	raw, err := client.Registration[verdandi.Fields, verdandi.Fields](
		registration.RegistrationOptions{Type: "raw", TTL: 3 * time.Second, Version: 1},
	)
	if err != nil {
		t.Fatal(err)
	}
	if err := raw.Unregister(context.Background()); err != nil {
		t.Fatal(err)
	}
}

// compileSelector verifies that callers name only Attr and Data even though
// their decoder pointer constraints remain internal to the child package.
func compileSelector(ctx context.Context, client *registration.Client) {
	_, _ = client.Selector[externalAttr, externalData](
		ctx,
		registration.SelectorOptions{Type: "proxy"},
	)
}
