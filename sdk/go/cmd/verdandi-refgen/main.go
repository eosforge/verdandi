// Command verdandi-refgen 为一组 Attr/Data struct 生成 Selector 引用型只读视图和字段编辑器。
package main

import (
	"bytes"
	"errors"
	"flag"
	"fmt"
	"go/ast"
	"go/format"
	"go/parser"
	"go/token"
	"io"
	"os"
	"path/filepath"
	"strings"
)

type fieldKind uint8

const (
	fieldValue fieldKind = iota + 1
	fieldSlice
)

type fieldSpec struct {
	name        string
	typeName    string
	elementName string
	kind        fieldKind
}

type bindingSpec struct {
	packageName string
	name        string
	attrName    string
	dataName    string
	attrFields  []fieldSpec
	dataFields  []fieldSpec
}

type generatorOptions struct {
	directory string
	attrName  string
	dataName  string
	name      string
	output    string
	check     bool
}

func main() {
	if err := run(os.Args[1:], os.Stderr); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}

// run 解析稳定命令行界面，并把所有文件系统工作委托给 generate。
func run(arguments []string, stderr io.Writer) error {
	flags := flag.NewFlagSet("verdandi-refgen", flag.ContinueOnError)
	flags.SetOutput(stderr)
	options := generatorOptions{directory: ".", output: "zz_verdandi_reference_gen.go"}
	flags.StringVar(&options.attrName, "attr", "", "Attr struct type name")
	flags.StringVar(&options.dataName, "data", "", "Data struct type name")
	flags.StringVar(&options.name, "name", "", "exported generated API prefix")
	flags.StringVar(&options.output, "output", options.output, "generated Go output file")
	flags.BoolVar(&options.check, "check", false, "verify that the output is current without writing")
	if err := flags.Parse(arguments); err != nil {
		return err
	}
	if flags.NArg() != 0 {
		return fmt.Errorf("unexpected arguments: %s", strings.Join(flags.Args(), " "))
	}
	return generate(options)
}

// generate 读取当前包、构造确定性源代码，并按普通生成或只读检查模式处理产物。
func generate(options generatorOptions) error {
	if !token.IsIdentifier(options.attrName) || !token.IsIdentifier(options.dataName) {
		return errors.New("-attr and -data must name local Go types")
	}
	if !token.IsIdentifier(options.name) || !ast.IsExported(options.name) {
		return errors.New("-name must be an exported Go identifier")
	}
	if options.output == "" {
		return errors.New("-output must not be empty")
	}

	spec, err := inspectPackage(options)
	if err != nil {
		return err
	}
	source, err := render(spec)
	if err != nil {
		return err
	}
	output := options.output
	if !filepath.IsAbs(output) {
		output = filepath.Join(options.directory, output)
	}
	if options.check {
		current, readErr := os.ReadFile(output)
		if readErr != nil {
			return fmt.Errorf("read generated output: %w", readErr)
		}
		if !bytes.Equal(current, source) {
			return fmt.Errorf("generated output is stale: %s", output)
		}
		return nil
	}
	if err := os.WriteFile(output, source, 0o644); err != nil {
		return fmt.Errorf("write generated output: %w", err)
	}
	return nil
}

// inspectPackage 只解析非测试 Go 源码，并排除将被覆盖的生成文件。
func inspectPackage(options generatorOptions) (bindingSpec, error) {
	entries, err := os.ReadDir(options.directory)
	if err != nil {
		return bindingSpec{}, fmt.Errorf("read package directory: %w", err)
	}
	outputBase := filepath.Base(options.output)
	fileSet := token.NewFileSet()
	var files []*ast.File
	for _, entry := range entries {
		name := entry.Name()
		if entry.IsDir() || !strings.HasSuffix(name, ".go") || strings.HasSuffix(name, "_test.go") || name == outputBase {
			continue
		}
		file, parseErr := parser.ParseFile(fileSet, filepath.Join(options.directory, name), nil, 0)
		if parseErr != nil {
			return bindingSpec{}, fmt.Errorf("parse %s: %w", name, parseErr)
		}
		files = append(files, file)
	}
	if len(files) == 0 {
		return bindingSpec{}, errors.New("package contains no non-test Go source files")
	}

	packageName := files[0].Name.Name
	types := make(map[string]*ast.TypeSpec)
	for _, file := range files {
		if file.Name.Name != packageName {
			return bindingSpec{}, fmt.Errorf("mixed packages %s and %s", packageName, file.Name.Name)
		}
		for _, declaration := range file.Decls {
			generic, ok := declaration.(*ast.GenDecl)
			if !ok || generic.Tok != token.TYPE {
				continue
			}
			for _, item := range generic.Specs {
				typeSpec, ok := item.(*ast.TypeSpec)
				if ok {
					if types[typeSpec.Name.Name] != nil {
						return bindingSpec{}, fmt.Errorf("type %s is declared more than once; generated models must be platform-independent", typeSpec.Name.Name)
					}
					types[typeSpec.Name.Name] = typeSpec
				}
			}
		}
	}

	attrFields, err := inspectStruct(fileSet, types, options.attrName)
	if err != nil {
		return bindingSpec{}, err
	}
	dataFields, err := inspectStruct(fileSet, types, options.dataName)
	if err != nil {
		return bindingSpec{}, err
	}
	for _, generatedName := range []string{
		options.name + "AttrRef",
		options.name + "DataRef",
		options.name + "DataEditor",
		options.name + "ReferenceCandidates",
		options.name + "ReferenceCandidate",
		options.name + "ReferenceSelection",
		options.name + "ReferenceSelector",
	} {
		if types[generatedName] != nil {
			return bindingSpec{}, fmt.Errorf("generated type %s conflicts with an existing declaration", generatedName)
		}
	}
	return bindingSpec{
		packageName: packageName,
		name:        options.name,
		attrName:    options.attrName,
		dataName:    options.dataName,
		attrFields:  attrFields,
		dataFields:  dataFields,
	}, nil
}

// inspectStruct 提取公开命名字段，并拒绝生成器无法证明只读或安全复制的类型。
func inspectStruct(fileSet *token.FileSet, types map[string]*ast.TypeSpec, name string) ([]fieldSpec, error) {
	typeSpec := types[name]
	if typeSpec == nil {
		return nil, fmt.Errorf("type %s not found", name)
	}
	if typeSpec.TypeParams != nil && len(typeSpec.TypeParams.List) != 0 {
		return nil, fmt.Errorf("type %s: generic structs are not supported", name)
	}
	structure, ok := typeSpec.Type.(*ast.StructType)
	if !ok {
		return nil, fmt.Errorf("type %s must be a struct", name)
	}
	fields := make([]fieldSpec, 0, len(structure.Fields.List))
	for _, field := range structure.Fields.List {
		if len(field.Names) == 0 {
			return nil, fmt.Errorf("type %s: embedded fields are not supported", name)
		}
		kind, kindErr := classifyType(field.Type, types, make(map[string]bool))
		if kindErr != nil {
			return nil, fmt.Errorf("type %s field %s: %w", name, field.Names[0].Name, kindErr)
		}
		var rendered bytes.Buffer
		if err := format.Node(&rendered, fileSet, field.Type); err != nil {
			return nil, fmt.Errorf("format type %s field %s: %w", name, field.Names[0].Name, err)
		}
		elementName := ""
		if kind == fieldSlice {
			element, elementErr := sliceElement(field.Type, types, make(map[string]bool))
			if elementErr != nil {
				return nil, fmt.Errorf("type %s field %s: %w", name, field.Names[0].Name, elementErr)
			}
			var renderedElement bytes.Buffer
			if err := format.Node(&renderedElement, fileSet, element); err != nil {
				return nil, fmt.Errorf("format element type %s field %s: %w", name, field.Names[0].Name, err)
			}
			elementName = renderedElement.String()
		}
		for _, identifier := range field.Names {
			if !ast.IsExported(identifier.Name) {
				return nil, fmt.Errorf("type %s field %s must be exported", name, identifier.Name)
			}
			fields = append(fields, fieldSpec{
				name:        identifier.Name,
				typeName:    rendered.String(),
				elementName: elementName,
				kind:        kind,
			})
		}
	}
	return fields, nil
}

// sliceElement 解析直接或本地命名 slice 的元素语法，供生成不暴露原始 slice 的只读包装。
func sliceElement(expression ast.Expr, types map[string]*ast.TypeSpec, visiting map[string]bool) (ast.Expr, error) {
	switch value := expression.(type) {
	case *ast.ParenExpr:
		return sliceElement(value.X, types, visiting)
	case *ast.ArrayType:
		if value.Len == nil {
			return value.Elt, nil
		}
	case *ast.Ident:
		declaration := types[value.Name]
		if declaration == nil {
			break
		}
		if visiting[value.Name] {
			return nil, fmt.Errorf("recursive named type %s", value.Name)
		}
		visiting[value.Name] = true
		element, err := sliceElement(declaration.Type, types, visiting)
		delete(visiting, value.Name)
		return element, err
	}
	return nil, errors.New("unable to resolve slice element type")
}

// classifyType 允许值复制安全的标量/定长数组/本地结构，以及可逐项复制的标量 slice。
// map、指针、接口、函数、嵌套 slice 和无法审查的外部复合类型必须先由调用方压平。
func classifyType(expression ast.Expr, types map[string]*ast.TypeSpec, visiting map[string]bool) (fieldKind, error) {
	switch value := expression.(type) {
	case *ast.ParenExpr:
		return classifyType(value.X, types, visiting)
	case *ast.Ident:
		if isScalar(value.Name) {
			return fieldValue, nil
		}
		declaration := types[value.Name]
		if declaration == nil {
			return 0, fmt.Errorf("unsupported named type %s", value.Name)
		}
		if visiting[value.Name] {
			return 0, fmt.Errorf("recursive named type %s", value.Name)
		}
		visiting[value.Name] = true
		kind, err := classifyType(declaration.Type, types, visiting)
		delete(visiting, value.Name)
		return kind, err
	case *ast.ArrayType:
		if value.Len == nil {
			kind, err := classifyType(value.Elt, types, visiting)
			if err != nil {
				return 0, err
			}
			if kind != fieldValue {
				return 0, errors.New("slices containing reference values are not supported")
			}
			return fieldSlice, nil
		}
		kind, err := classifyType(value.Elt, types, visiting)
		if err != nil {
			return 0, err
		}
		if kind != fieldValue {
			return 0, errors.New("arrays containing reference values are not supported")
		}
		return fieldValue, nil
	case *ast.StructType:
		for _, field := range value.Fields.List {
			kind, err := classifyType(field.Type, types, visiting)
			if err != nil {
				return 0, err
			}
			if kind != fieldValue {
				return 0, errors.New("structs containing reference values are not supported")
			}
		}
		return fieldValue, nil
	default:
		return 0, fmt.Errorf("unsupported field type %T", expression)
	}
}

// isScalar 列出赋值复制不会暴露可变别名的 Go 预声明值类型。
func isScalar(name string) bool {
	switch name {
	case "bool", "byte", "complex64", "complex128", "float32", "float64", "int", "int8", "int16", "int32", "int64",
		"rune", "string", "uint", "uint8", "uint16", "uint32", "uint64", "uintptr":
		return true
	default:
		return false
	}
}

// render 生成经过 gofmt 的确定性源码，并保留 Attr/Data 的字段声明顺序。
func render(spec bindingSpec) ([]byte, error) {
	var output bytes.Buffer
	fmt.Fprintf(&output, "// Code generated by verdandi-refgen; DO NOT EDIT.\n\n")
	fmt.Fprintf(&output, "package %s\n\n", spec.packageName)
	fmt.Fprintln(&output, "import verdandiregistration \"github.com/eosforge/verdandi/sdk/go/registration\"")
	fmt.Fprintln(&output)

	emitView(&output, spec.name+"AttrRef", spec.attrName, spec.attrFields)
	emitView(&output, spec.name+"DataRef", spec.dataName, spec.dataFields)
	emitEditor(&output, spec.name+"DataEditor", spec.attrName, spec.dataName, spec.dataFields)

	attrRef := spec.name + "AttrRef"
	dataRef := spec.name + "DataRef"
	dataEditor := spec.name + "DataEditor"
	genericTypeNames := []string{spec.attrName, spec.dataName, attrRef, dataRef, dataEditor}
	genericTypes := strings.Join(genericTypeNames, ", ")
	genericTypeLines := strings.Join(genericTypeNames, ",\n\t")
	fmt.Fprintf(&output, "// %sReferenceCandidates 是代码生成的回调期候选集合。\n", spec.name)
	fmt.Fprintf(&output, "type %sReferenceCandidates = verdandiregistration.ReferenceCandidates[\n\t%s,\n]\n\n", spec.name, genericTypeLines)
	fmt.Fprintf(&output, "// %sReferenceCandidate 是代码生成的回调期只读候选。\n", spec.name)
	fmt.Fprintf(&output, "type %sReferenceCandidate = verdandiregistration.ReferenceCandidate[\n\t%s,\n]\n\n", spec.name, genericTypeLines)
	fmt.Fprintf(&output, "// %sReferenceSelection 是可由策略返回并按需编辑的候选。\n", spec.name)
	fmt.Fprintf(&output, "type %sReferenceSelection = verdandiregistration.ReferenceSelection[\n\t%s,\n]\n\n", spec.name, genericTypeLines)
	fmt.Fprintf(&output, "// %sReferenceSelector 嵌入 Verdandi 引用型热路径，并保留其 WithOne/WithAny API。\n", spec.name)
	fmt.Fprintf(&output, "type %sReferenceSelector struct {\n\t*verdandiregistration.ReferenceSelector[\n\t\t%s,\n\t]\n}\n\n", spec.name,
		strings.Join(genericTypeNames, ",\n\t\t"))
	fmt.Fprintf(&output, "// New%sReferenceSelector 把普通强类型 Selector 包装为生成的引用型入口。\n", spec.name)
	fmt.Fprintf(&output, "func New%sReferenceSelector(\n\tselector *verdandiregistration.Selector[%s, %s],\n) (*%sReferenceSelector, error) {\n",
		spec.name, spec.attrName, spec.dataName, spec.name)
	fmt.Fprintln(&output, "\tcore, err := verdandiregistration.NewReferenceSelector(")
	fmt.Fprintln(&output, "\t\tselector,")
	fmt.Fprintf(&output, "\t\tverdandiregistration.ReferenceSchema[%s]{\n", genericTypes)
	emitViewFactory(&output, "Attr", spec.attrName, attrRef, spec.attrFields)
	emitViewFactory(&output, "Data", spec.dataName, dataRef, spec.dataFields)
	fmt.Fprintf(&output, "\t\t\tEdit: func(editor verdandiregistration.ReferenceEditor[%s, %s]) %s {\n", spec.attrName, spec.dataName, dataEditor)
	fmt.Fprintf(&output, "\t\t\t\treturn %s{editor: editor}\n\t\t\t},\n", dataEditor)
	fmt.Fprintf(&output, "\t\t\tCloneData: func(value %s) %s {\n", spec.dataName, spec.dataName)
	for _, field := range spec.dataFields {
		if field.kind == fieldSlice {
			fmt.Fprintf(&output,
				"\t\t\t\tif value.%s != nil {\n\t\t\t\t\tvalue.%s = append(%s{}, value.%s...)\n\t\t\t\t}\n",
				field.name, field.name, field.typeName, field.name)
		}
	}
	fmt.Fprintln(&output, "\t\t\t\treturn value")
	fmt.Fprintln(&output, "\t\t\t},")
	fmt.Fprintln(&output, "\t\t},")
	fmt.Fprintln(&output, "\t)")
	fmt.Fprintln(&output, "\tif err != nil {")
	fmt.Fprintln(&output, "\t\treturn nil, err")
	fmt.Fprintln(&output, "\t}")
	fmt.Fprintf(&output, "\treturn &%sReferenceSelector{ReferenceSelector: core}, nil\n", spec.name)
	fmt.Fprintln(&output, "}")

	formatted, err := format.Source(output.Bytes())
	if err != nil {
		return nil, fmt.Errorf("format generated source: %w\n%s", err, output.String())
	}
	return formatted, nil
}

// emitView 为每个字段生成只读 getter；slice 通过核心包装在被实际读取时才复制。
func emitView(output *bytes.Buffer, viewName string, sourceName string, fields []fieldSpec) {
	fmt.Fprintf(output, "// %s 是 %s 的回调期只读视图；不得在选择回调结束后保留。\n", viewName, sourceName)
	fmt.Fprintf(output, "type %s struct {\n", viewName)
	for _, field := range fields {
		if field.kind == fieldSlice {
			fmt.Fprintf(output, "\tfield%s verdandiregistration.ReferenceSlice[%s, %s]\n", field.name, field.typeName, field.elementName)
		} else {
			fmt.Fprintf(output, "\tfield%s %s\n", field.name, field.typeName)
		}
	}
	fmt.Fprintln(output, "}")
	fmt.Fprintln(output)
	for _, field := range fields {
		fmt.Fprintf(output, "// %s 返回 %s.%s；引用型字段会返回独立副本。\n", field.name, sourceName, field.name)
		fmt.Fprintf(output, "func (view %s) %s() %s {\n", viewName, field.name, field.typeName)
		if field.kind == fieldSlice {
			fmt.Fprintf(output, "\treturn view.field%s.Clone()\n", field.name)
		} else {
			fmt.Fprintf(output, "\treturn view.field%s\n", field.name)
		}
		fmt.Fprintln(output, "}")
		fmt.Fprintln(output)
	}
}

// emitViewFactory 把事务内值投影成不含原始 Attr/Data 指针的生成视图。
func emitViewFactory(output *bytes.Buffer, schemaField string, sourceName string, viewName string, fields []fieldSpec) {
	fmt.Fprintf(output, "\t\t\t%s: func(value *%s) %s {\n", schemaField, sourceName, viewName)
	fmt.Fprintf(output, "\t\t\t\treturn %s{\n", viewName)
	for _, field := range fields {
		if field.kind == fieldSlice {
			fmt.Fprintf(output, "\t\t\t\t\tfield%s: verdandiregistration.NewReferenceSlice(value.%s),\n", field.name, field.name)
		} else {
			fmt.Fprintf(output, "\t\t\t\t\tfield%s: value.%s,\n", field.name, field.name)
		}
	}
	fmt.Fprintln(output, "\t\t\t\t}")
	fmt.Fprintln(output, "\t\t\t},")
}

// emitEditor 为 Data 每个字段生成一个只在活动事务内生效的 setter。
func emitEditor(output *bytes.Buffer, editorName string, attrName string, dataName string, fields []fieldSpec) {
	fmt.Fprintf(output, "// %s 通过字段 setter 修改所选 %s 的本地预测副本。\n", editorName, dataName)
	fmt.Fprintf(output, "type %s struct {\n\teditor verdandiregistration.ReferenceEditor[%s, %s]\n}\n\n", editorName, attrName, dataName)
	for _, field := range fields {
		fmt.Fprintf(output, "// Set%s 暂存 %s.%s；只有最终选中且回调成功才提交。\n", field.name, dataName, field.name)
		fmt.Fprintf(output, "func (editor %s) Set%s(value %s) error {\n", editorName, field.name, field.typeName)
		fmt.Fprintf(output, "\treturn editor.editor.Apply(func(target *%s) {\n", dataName)
		if field.kind == fieldSlice {
			fmt.Fprintln(output, "\t\tif value == nil {")
			fmt.Fprintf(output, "\t\t\ttarget.%s = nil\n\t\t\treturn\n\t\t}\n", field.name)
			fmt.Fprintf(output, "\t\ttarget.%s = append(%s{}, value...)\n", field.name, field.typeName)
		} else {
			fmt.Fprintf(output, "\t\ttarget.%s = value\n", field.name)
		}
		fmt.Fprintln(output, "\t})")
		fmt.Fprintln(output, "}")
		fmt.Fprintln(output)
	}
}
