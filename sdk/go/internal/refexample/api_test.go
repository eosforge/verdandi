package refexample

import (
	"context"

	"github.com/eosforge/verdandi/sdk/go/registration"
)

func compileReferenceAPI(ctx context.Context, selector *registration.Selector[ProxyAttr, ProxyData]) {
	reference, _ := NewProxyReferenceSelector(selector)
	_, _ = reference.WithOne(ctx, func(candidates ProxyReferenceCandidates) (ProxyReferenceSelection, bool, error) {
		candidate, ok := candidates.At(0)
		if !ok {
			return ProxyReferenceSelection{}, false, nil
		}
		selection := candidate.Select()
		if err := selection.Edit().SetPower(candidate.Data().Power() + 1); err != nil {
			return ProxyReferenceSelection{}, false, err
		}
		return selection, true, nil
	})
	_, _ = reference.WithAny(ctx, func(candidates ProxyReferenceCandidates) ([]ProxyReferenceSelection, error) {
		candidate, ok := candidates.At(0)
		if !ok {
			return nil, nil
		}
		return []ProxyReferenceSelection{candidate.Select()}, nil
	})
}
