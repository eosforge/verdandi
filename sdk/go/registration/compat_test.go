package registration

import (
	"context"

	verdandi "github.com/LaconisIves/verdandi/sdk/go"
)

type Standalone = verdandi.Standalone
type Sentinel = verdandi.Sentinel
type Fields = verdandi.Fields
type Code = verdandi.Code
type Error = verdandi.Error
type RawRegistration = registrationCore
type RawSelector = selectorCore
type RegistrationConfig = registrationConfig
type SelectorConfig = selectorConfig
type Update = registrationUpdateFields
type Record = rawRecord
type RetainedRecord = rawRetainedRecord
type Snapshot = rawSnapshot

type rawRecord struct {
	Meta Meta
	Attr fields
	Data fields
}

type rawRetainedRecord struct {
	Record        rawRecord
	RetainedUntil uint64
}

type rawSnapshot struct {
	Generation   uint64
	Synchronized bool
	Records      []rawRecord
	Retained     []rawRetainedRecord
}

const (
	CodeInvalid     = verdandi.CodeInvalid
	CodeProtocol    = verdandi.CodeProtocol
	CodeContract    = verdandi.CodeContract
	CodeTarget      = verdandi.CodeTarget
	CodeCapacity    = verdandi.CodeCapacity
	CodeMissing     = verdandi.CodeMissing
	CodeStale       = verdandi.CodeStale
	CodeTransition  = verdandi.CodeTransition
	CodeImmutable   = verdandi.CodeImmutable
	CodeCorrupt     = verdandi.CodeCorrupt
	CodeUnavailable = verdandi.CodeUnavailable
	CodeDeadline    = verdandi.CodeDeadline
	CodeAmbiguous   = verdandi.CodeAmbiguous
	CodeClosed      = verdandi.CodeClosed
)

var IsCode = verdandi.IsCode

func newTestRuntime(config runtimeConfig, limits zoneConfig) *clientRuntime {
	runtime := &clientRuntime{
		config: config,
		errors: make(chan error, 16),
		done:   make(chan struct{}),
	}
	runtime.zoneConfig.Store(&limits)
	return runtime
}

func registerRaw(ctx context.Context, client *Client, config RegistrationConfig) (*RawRegistration, error) {
	runtime, err := runtimeFor(client)
	if err != nil {
		return nil, err
	}
	uuid, err := newRegistrationUUID()
	if err != nil {
		return nil, wrapError(codeUnavailable, err)
	}
	return runtime.registerWithUUID(ctx, config, uuid)
}

func (registration *registrationCore) UUID() string {
	if registration == nil {
		return ""
	}
	return registration.uuid
}

func (registration *registrationCore) Update(ctx context.Context, update registrationUpdateFields) error {
	return registration.updateOwned(ctx, detachUpdate(update))
}

func detachUpdate(update registrationUpdateFields) registrationUpdateFields {
	if update.Version != nil {
		version := *update.Version
		update.Version = &version
	}
	update.Data = cloneFields(update.Data)
	return update
}

func (selector *selectorCore) Snapshot() (rawSnapshot, error) {
	if selector == nil {
		return rawSnapshot{}, protocolError(codeClosed, "", 0)
	}
	view := selector.view.Load()
	if view == nil || !view.synchronized {
		return rawSnapshot{}, protocolError(codeUnavailable, "selector", 0)
	}
	result := rawSnapshot{
		Generation:   view.generation,
		Synchronized: view.synchronized,
		Records:      make([]rawRecord, 0, len(view.orderedRecords)),
		Retained:     make([]rawRetainedRecord, 0, len(view.orderedRetained)),
	}
	for _, record := range view.orderedRecords {
		result.Records = append(result.Records, cloneRecord(record))
	}
	for _, retained := range view.orderedRetained {
		result.Retained = append(result.Retained, cloneRetainedRecord(retained))
	}
	return result, nil
}

func (selector *selectorCore) Find(uuid string) (rawRecord, bool, error) {
	if selector == nil {
		return rawRecord{}, false, protocolError(codeClosed, "", 0)
	}
	view := selector.view.Load()
	if view == nil || !view.synchronized {
		return rawRecord{}, false, protocolError(codeUnavailable, "selector", 0)
	}
	record, exists := view.records[uuid]
	if !exists {
		return rawRecord{}, false, nil
	}
	return cloneRecord(record), true, nil
}

func (selector *selectorCore) FindRetained(uuid string) (rawRetainedRecord, bool, error) {
	if selector == nil {
		return rawRetainedRecord{}, false, protocolError(codeClosed, "", 0)
	}
	view := selector.view.Load()
	if view == nil || !view.synchronized {
		return rawRetainedRecord{}, false, protocolError(codeUnavailable, "selector", 0)
	}
	retained, exists := view.retained[uuid]
	if !exists {
		return rawRetainedRecord{}, false, nil
	}
	return cloneRetainedRecord(retained), true, nil
}

func cloneRecord(record *selectorRecord) rawRecord {
	if record == nil {
		return rawRecord{}
	}
	return rawRecord{Meta: record.meta, Attr: cloneFields(record.attr), Data: cloneFields(record.data)}
}

func cloneRetainedRecord(retained retainedSelectorRecord) rawRetainedRecord {
	return rawRetainedRecord{Record: cloneRecord(retained.record), RetainedUntil: retained.until}
}

func selectRaw(ctx context.Context, client *Client, config SelectorConfig) (*RawSelector, error) {
	runtime, err := runtimeFor(client)
	if err != nil {
		return nil, err
	}
	return runtime.selectRegistry(ctx, config, nil, nil)
}

func callRawRegistration(
	ctx context.Context,
	client *Client,
	kind registrationScriptKind,
	typeName string,
	uuid string,
	arguments []any,
) (registrationReply, error) {
	runtime, err := runtimeFor(client)
	if err != nil {
		return registrationReply{}, err
	}
	return runtime.callRegistration(ctx, kind, typeName, uuid, arguments)
}
