//! Verdandi 强类型 Redis、Registration、服务发现与带版本 Catalog SDK。
//!
//! Registration 与 Selector 由 [`registration`] 模块拥有。应用 Attr 和 Data 是有界的
//! 顶层字段 map，字段值是不透明字节。根 [`Client`] 还提供有容量约束的普通 Redis
//! Key 与 Hash 命令。

extern crate self as verdandi;

pub mod catalog;
mod client;
mod config;
pub mod configuration;
mod error;
mod fields;
mod identifier;
mod lifecycle;
mod redis;
pub mod registration;

pub use client::Client;
pub use config::{Config, PoolConfig, ReconnectConfig, TlsConfig};
pub use error::{Code, Error, Result};
pub use fields::{FieldValue, Fields};
pub use redis::{DecodeValue, EncodeValue, HashCommands, HashValue, KeyCommands, TtlKeyCommands};
pub use verdandi_derive::HashValue;
