//! Go 与 Rust 共用 JSON 结构的严格加载和语言原生配置转换。

mod convert;
mod model;

pub use model::{Auth, Catalog, Config, Pool, Reconnect, Recovery, Redis, Registration, RegistrationPolicy, Selector, Tls};

#[cfg(test)]
#[path = "../../tests/internal/configuration.rs"]
mod tests;
