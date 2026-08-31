use std::collections::HashSet;

use proc_macro::TokenStream;
use quote::quote;
use syn::{Data, DeriveInput, Error, Fields, LitStr, Result, parse_macro_input, parse_quote};

#[proc_macro_derive(HashValue, attributes(redis))]
/// 为具名字段结构体生成 `verdandi::HashValue` 实现。
///
/// `input` 是编译器传入的派生目标；语法或字段契约错误会转换为定位到源代码的编译错误 TokenStream。
pub fn derive_hash_value(input: TokenStream) -> TokenStream {
    let input = parse_macro_input!(input as DeriveInput);
    expand_hash_value(input).unwrap_or_else(Error::into_compile_error).into()
}

/// 展开已经解析的 `input`，生成固定字段表及强类型 Hash 编解码实现。
///
/// 仅接受至少含一个非 skip 具名字段的结构体；返回值由过程宏入口转换为最终 TokenStream。
fn expand_hash_value(input: DeriveInput) -> Result<proc_macro2::TokenStream> {
    let Data::Struct(data) = &input.data else {
        return Err(Error::new_spanned(&input.ident, "HashValue can only be derived for a struct"));
    };
    let Fields::Named(fields) = &data.fields else {
        return Err(Error::new_spanned(&input.ident, "HashValue requires a struct with named fields"));
    };

    // 在原泛型约束上补充每个字段实际需要的 Default/编解码边界，不把无关约束强加给调用方。
    let mut generics = input.generics.clone();
    let mut names = HashSet::new();
    let mut selected = Vec::new();
    let mut initializers = Vec::with_capacity(fields.named.len());
    for field in &fields.named {
        let Some(identifier) = &field.ident else {
            return Err(Error::new_spanned(field, "HashValue requires named fields"));
        };
        let field_type = &field.ty;
        let redis = redis_field(field)?;
        generics
            .make_where_clause()
            .predicates
            .push(parse_quote!(#field_type: ::core::default::Default));
        if let Some(name) = redis {
            if !names.insert(name.value()) {
                return Err(Error::new_spanned(field, format!("duplicate Redis field name {:?}", name.value())));
            }
            generics
                .make_where_clause()
                .predicates
                .push(parse_quote!(#field_type: ::verdandi::DecodeValue + ::verdandi::EncodeValue));
            let index = selected.len();
            initializers.push(quote! {
                #identifier: match values[#index].as_deref() {
                    Some(value) => <#field_type as ::verdandi::DecodeValue>::decode_value(value)?,
                    None => ::core::default::Default::default(),
                }
            });
            selected.push((identifier, field_type, name));
        } else {
            initializers.push(quote!(#identifier: ::core::default::Default::default()));
        }
    }
    if selected.is_empty() {
        return Err(Error::new_spanned(&input.ident, "HashValue requires at least one non-skipped field"));
    }

    // 生成代码使用绝对 crate 路径，避免调用方作用域导入影响派生结果。
    let identifier = &input.ident;
    let (implementation, type_generics, where_clause) = generics.split_for_impl();
    let field_names = selected.iter().map(|(_, _, name)| name);
    let encoders = selected.iter().map(|(field, field_type, name)| {
        quote! {
            let mut encoded = ::std::vec::Vec::new();
            <#field_type as ::verdandi::EncodeValue>::encode_value(&self.#field, &mut encoded)?;
            let _ = destination.insert(#name.to_owned(), encoded);
        }
    });

    Ok(quote! {
        impl #implementation ::verdandi::HashValue for #identifier #type_generics #where_clause {
            const FIELDS: &'static [&'static str] = &[#(#field_names),*];

            /// 按静态 FIELDS 顺序解码 HMGET 结果；缺失位置使用字段 Default，存在值调用 DecodeValue。
            fn decode_hash(values: &[::core::option::Option<::std::vec::Vec<u8>>]) -> ::verdandi::Result<Self> {
                if values.len() != Self::FIELDS.len() {
                    return Err(::verdandi::Error::field(::verdandi::Code::Corrupt, "hash"));
                }
                Ok(Self { #(#initializers),* })
            }

            /// 按静态 FIELDS 契约编码全部选中字段，并把拥有型字节写入 destination。
            fn encode_hash(&self, destination: &mut ::verdandi::Fields) -> ::verdandi::Result<()> {
                #(#encoders)*
                Ok(())
            }
        }
    })
}

/// 解析单个结构体 `field` 的 `#[redis(...)]` 规则。
///
/// 返回 Some 表示参与 Redis Hash 编解码并携带最终字段名，None 表示 skip；重复、冲突或非法长度返回语法错误。
fn redis_field(field: &syn::Field) -> Result<Option<LitStr>> {
    let Some(identifier) = &field.ident else {
        return Err(Error::new_spanned(field, "HashValue requires named fields"));
    };
    let mut name = None;
    let mut skipped = false;
    let mut attribute_seen = false;
    // 每个字段最多接受一个 redis attribute，内部只允许 name 或 skip 二选一。
    for attribute in &field.attrs {
        if !attribute.path().is_ident("redis") {
            continue;
        }
        if attribute_seen {
            return Err(Error::new_spanned(attribute, "duplicate redis attribute"));
        }
        attribute_seen = true;
        attribute.parse_nested_meta(|meta| {
            if meta.path.is_ident("skip") {
                if skipped || name.is_some() {
                    return Err(meta.error("redis skip cannot be combined with another option"));
                }
                skipped = true;
                return Ok(());
            }
            if meta.path.is_ident("name") {
                if skipped || name.is_some() {
                    return Err(meta.error("redis name cannot be combined with another option"));
                }
                name = Some(meta.value()?.parse::<LitStr>()?);
                return Ok(());
            }
            Err(meta.error("expected redis name = \"...\" or skip"))
        })?;
    }
    if skipped {
        return Ok(None);
    }
    if attribute_seen && name.is_none() {
        return Err(Error::new_spanned(field, "redis attribute requires name = \"...\" or skip"));
    }
    let name = name.unwrap_or_else(|| LitStr::new(identifier.to_string().trim_start_matches("r#"), identifier.span()));
    if name.value().is_empty() || name.value().len() > 1024 {
        return Err(Error::new_spanned(name, "Redis field name must contain 1..=1024 bytes"));
    }
    Ok(Some(name))
}
